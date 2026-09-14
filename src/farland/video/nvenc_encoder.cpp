// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// NVENC backend: see nvenc_encoder.hpp for the design and nvenc_abi.hpp for
// the interface versions. The session setup follows NVIDIA's Video Codec
// SDK samples (NvEncoder.cpp, NvEncoderCuda.cpp) and FFmpeg's nvenc.c where
// they agree with the low-latency settings of h264_encoder.hpp.

#include <farland/base/assert.hpp>
#include <farland/base/log.hpp>
#include <farland/video/nvenc_abi.hpp>
#include <farland/video/nvenc_encoder.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <utility>
#include <vector>

namespace farland::video::nvenc {

namespace {

constexpr std::string_view component = "video.nvenc";

/// Imported dmabufs kept at a time; a capture cycles through a few buffers.
constexpr std::size_t max_imports = 8;

/// BGRX (or RGBX) to NV12, one thread per 2x2 block of the coded picture.
/// It is codec::bgrx_to_yuv420 in PTX: each output sample is
///     ((c0 R + c1 G + c2 B + round) >> 8) + offset
/// (arithmetic shift, clamped to 0..255), chroma from the 2x2 average
/// (rounded down) of R, G and B; odd image sides repeat the last pixel
/// inside the block, and blocks outside the image are black. PTX ISA 6.0
/// for sm_50 (Maxwell, the oldest GPUs NVENC 12 supports); the driver
/// compiles it for the GPU at hand.
constexpr std::string_view bgrx_to_nv12_ptx = R"PTX(
.version 6.0
.target sm_50
.address_size 64

.visible .entry bgrx_to_nv12(
	.param .u64 p_src,
	.param .u32 p_src_pitch,
	.param .u32 p_src_width,
	.param .u32 p_src_height,
	.param .u64 p_dst_y,
	.param .u64 p_dst_uv,
	.param .u32 p_dst_pitch,
	.param .u32 p_blocks_x,
	.param .u32 p_blocks_y,
	.param .u32 p_red_shift,
	.param .u32 p_blue_shift,
	.param .s32 p_yr,
	.param .s32 p_yg,
	.param .s32 p_yb,
	.param .s32 p_ur,
	.param .s32 p_ug,
	.param .s32 p_ub,
	.param .s32 p_vr,
	.param .s32 p_vg,
	.param .s32 p_vb,
	.param .s32 p_round,
	.param .s32 p_y_offset
)
{
	.reg .pred %p<4>;
	.reg .b16 %h<3>;
	.reg .b32 %r<64>;
	.reg .b64 %d<12>;

	// Block (bx, by) = (%r3, %r4).
	mov.u32 %r0, %ctaid.x;
	mov.u32 %r1, %ntid.x;
	mov.u32 %r2, %tid.x;
	mad.lo.u32 %r3, %r0, %r1, %r2;
	mov.u32 %r0, %ctaid.y;
	mov.u32 %r1, %ntid.y;
	mov.u32 %r2, %tid.y;
	mad.lo.u32 %r4, %r0, %r1, %r2;
	ld.param.u32 %r5, [p_blocks_x];
	ld.param.u32 %r6, [p_blocks_y];
	setp.ge.u32 %p0, %r3, %r5;
	setp.ge.u32 %p1, %r4, %r6;
	or.pred %p0, %p0, %p1;
	@%p0 bra DONE;

	// The four pixels (%r20..%r23), zero (black) outside the image.
	ld.param.u64 %d0, [p_src];
	ld.param.u32 %r7, [p_src_pitch];
	ld.param.u32 %r8, [p_src_width];
	ld.param.u32 %r9, [p_src_height];
	shl.b32 %r10, %r3, 1;
	shl.b32 %r11, %r4, 1;
	mov.u32 %r20, 0;
	mov.u32 %r21, 0;
	mov.u32 %r22, 0;
	mov.u32 %r23, 0;
	setp.ge.u32 %p2, %r10, %r8;
	setp.ge.u32 %p3, %r11, %r9;
	or.pred %p2, %p2, %p3;
	@%p2 bra CONVERT;
	add.u32 %r12, %r10, 1;
	sub.u32 %r13, %r8, 1;
	min.u32 %r12, %r12, %r13;
	add.u32 %r14, %r11, 1;
	sub.u32 %r15, %r9, 1;
	min.u32 %r14, %r14, %r15;
	mul.wide.u32 %d1, %r11, %r7;
	add.u64 %d1, %d0, %d1;
	mul.wide.u32 %d2, %r14, %r7;
	add.u64 %d2, %d0, %d2;
	mul.wide.u32 %d3, %r10, 4;
	mul.wide.u32 %d4, %r12, 4;
	add.u64 %d5, %d1, %d3;
	ld.global.u32 %r20, [%d5];
	add.u64 %d5, %d1, %d4;
	ld.global.u32 %r21, [%d5];
	add.u64 %d5, %d2, %d3;
	ld.global.u32 %r22, [%d5];
	add.u64 %d5, %d2, %d4;
	ld.global.u32 %r23, [%d5];

CONVERT:
	ld.param.u32 %r16, [p_red_shift];
	ld.param.u32 %r17, [p_blue_shift];
	ld.param.s32 %r40, [p_yr];
	ld.param.s32 %r41, [p_yg];
	ld.param.s32 %r42, [p_yb];
	ld.param.s32 %r43, [p_ur];
	ld.param.s32 %r44, [p_ug];
	ld.param.s32 %r45, [p_ub];
	ld.param.s32 %r46, [p_vr];
	ld.param.s32 %r47, [p_vg];
	ld.param.s32 %r48, [p_vb];
	ld.param.s32 %r49, [p_round];
	ld.param.s32 %r50, [p_y_offset];

	// R, G, B of each pixel.
	bfe.u32 %r24, %r20, %r16, 8;
	bfe.u32 %r25, %r20, 8, 8;
	bfe.u32 %r26, %r20, %r17, 8;
	bfe.u32 %r27, %r21, %r16, 8;
	bfe.u32 %r28, %r21, 8, 8;
	bfe.u32 %r29, %r21, %r17, 8;
	bfe.u32 %r30, %r22, %r16, 8;
	bfe.u32 %r31, %r22, 8, 8;
	bfe.u32 %r32, %r22, %r17, 8;
	bfe.u32 %r33, %r23, %r16, 8;
	bfe.u32 %r34, %r23, 8, 8;
	bfe.u32 %r35, %r23, %r17, 8;

	// Y of each pixel (%r51..%r54).
	mad.lo.s32 %r51, %r40, %r24, %r49;
	mad.lo.s32 %r51, %r41, %r25, %r51;
	mad.lo.s32 %r51, %r42, %r26, %r51;
	shr.s32 %r51, %r51, 8;
	add.s32 %r51, %r51, %r50;
	max.s32 %r51, %r51, 0;
	min.s32 %r51, %r51, 255;
	mad.lo.s32 %r52, %r40, %r27, %r49;
	mad.lo.s32 %r52, %r41, %r28, %r52;
	mad.lo.s32 %r52, %r42, %r29, %r52;
	shr.s32 %r52, %r52, 8;
	add.s32 %r52, %r52, %r50;
	max.s32 %r52, %r52, 0;
	min.s32 %r52, %r52, 255;
	mad.lo.s32 %r53, %r40, %r30, %r49;
	mad.lo.s32 %r53, %r41, %r31, %r53;
	mad.lo.s32 %r53, %r42, %r32, %r53;
	shr.s32 %r53, %r53, 8;
	add.s32 %r53, %r53, %r50;
	max.s32 %r53, %r53, 0;
	min.s32 %r53, %r53, 255;
	mad.lo.s32 %r54, %r40, %r33, %r49;
	mad.lo.s32 %r54, %r41, %r34, %r54;
	mad.lo.s32 %r54, %r42, %r35, %r54;
	shr.s32 %r54, %r54, 8;
	add.s32 %r54, %r54, %r50;
	max.s32 %r54, %r54, 0;
	min.s32 %r54, %r54, 255;

	// U and V (%r58, %r59) of the 2x2 average (%r55..%r57).
	add.u32 %r55, %r24, %r27;
	add.u32 %r55, %r55, %r30;
	add.u32 %r55, %r55, %r33;
	shr.u32 %r55, %r55, 2;
	add.u32 %r56, %r25, %r28;
	add.u32 %r56, %r56, %r31;
	add.u32 %r56, %r56, %r34;
	shr.u32 %r56, %r56, 2;
	add.u32 %r57, %r26, %r29;
	add.u32 %r57, %r57, %r32;
	add.u32 %r57, %r57, %r35;
	shr.u32 %r57, %r57, 2;
	mad.lo.s32 %r58, %r43, %r55, %r49;
	mad.lo.s32 %r58, %r44, %r56, %r58;
	mad.lo.s32 %r58, %r45, %r57, %r58;
	shr.s32 %r58, %r58, 8;
	add.s32 %r58, %r58, 128;
	max.s32 %r58, %r58, 0;
	min.s32 %r58, %r58, 255;
	mad.lo.s32 %r59, %r46, %r55, %r49;
	mad.lo.s32 %r59, %r47, %r56, %r59;
	mad.lo.s32 %r59, %r48, %r57, %r59;
	shr.s32 %r59, %r59, 8;
	add.s32 %r59, %r59, 128;
	max.s32 %r59, %r59, 0;
	min.s32 %r59, %r59, 255;

	// Two Y pairs and one UV pair, as 16-bit stores.
	ld.param.u64 %d6, [p_dst_y];
	ld.param.u64 %d7, [p_dst_uv];
	ld.param.u32 %r18, [p_dst_pitch];
	shl.b32 %r60, %r52, 8;
	or.b32 %r60, %r60, %r51;
	shl.b32 %r61, %r54, 8;
	or.b32 %r61, %r61, %r53;
	shl.b32 %r62, %r59, 8;
	or.b32 %r62, %r62, %r58;
	cvt.u64.u32 %d8, %r10;
	mul.wide.u32 %d9, %r11, %r18;
	add.u64 %d9, %d6, %d9;
	add.u64 %d9, %d9, %d8;
	cvt.u16.u32 %h0, %r60;
	st.global.u16 [%d9], %h0;
	cvt.u64.u32 %d10, %r18;
	add.u64 %d9, %d9, %d10;
	cvt.u16.u32 %h1, %r61;
	st.global.u16 [%d9], %h1;
	mul.wide.u32 %d11, %r4, %r18;
	add.u64 %d11, %d7, %d11;
	add.u64 %d11, %d11, %d8;
	cvt.u16.u32 %h2, %r62;
	st.global.u16 [%d11], %h2;

DONE:
	ret;
}
)PTX";

/// Integer coefficients of the kernel, in 1/256.
struct Coefficients {
    std::array<std::int32_t, 9> m{};  ///< Y, U, V rows of R, G, B
    std::int32_t round = 0;
    std::int32_t y_offset = 0;
};

/// Rounds the matrix of `color` to 1/256. For full-range BT.709 this gives
/// FreeRDP's coefficients (codec/yuv.hpp) with no rounding term, so the
/// result is bit-identical to codec::bgrx_to_yuv420; limited range scales
/// by 219/255 and 224/255, adds 16 to Y and rounds to nearest.
[[nodiscard]] Coefficients coefficients(const ColorSpace& color)
{
    const bool bt709 = color.matrix == ColorSpace::Matrix::bt709;
    const double kr = bt709 ? 0.2126 : 0.299;
    const double kb = bt709 ? 0.0722 : 0.114;
    const double kg = 1.0 - kr - kb;
    const double ys = color.full_range ? 1.0 : 219.0 / 255.0;
    const double cs = color.full_range ? 1.0 : 224.0 / 255.0;
    const std::array<double, 9> m{
        ys * kr,
        ys * kg,
        ys * kb,
        cs * -kr / (2.0 * (1.0 - kb)),
        cs * -kg / (2.0 * (1.0 - kb)),
        cs * 0.5,
        cs * 0.5,
        cs * -kg / (2.0 * (1.0 - kr)),
        cs * -kb / (2.0 * (1.0 - kr)),
    };
    Coefficients c;
    for (std::size_t i = 0; i < m.size(); ++i) {
        c.m.at(i) = static_cast<std::int32_t>(std::lround(m.at(i) * 256.0));
    }
    c.round = color.full_range ? 0 : 128;
    c.y_offset = color.full_range ? 0 : 16;
    return c;
}

/// Byte positions of R and B in the little-endian word of a supported
/// packed RGB format; G is always bits 8..15.
struct Channels {
    std::uint32_t red_shift = 16;
    std::uint32_t blue_shift = 0;
};

[[nodiscard]] std::optional<Channels> channels(std::uint32_t fourcc)
{
    if (fourcc == drm_fourcc::xrgb8888 || fourcc == drm_fourcc::argb8888) {
        return Channels{.red_shift = 16, .blue_shift = 0};  // B, G, R, X in memory
    }
    if (fourcc == drm_fourcc::xbgr8888 || fourcc == drm_fourcc::abgr8888) {
        return Channels{.red_shift = 0, .blue_shift = 16};  // R, G, B, X in memory
    }
    return std::nullopt;
}

template <class Fn>
[[nodiscard]] Fn symbol(void* handle, const char* name)
{
    // POSIX requires that the object pointer dlsym returns converts to a
    // function pointer; C++ makes that conversion conditionally-supported.
    return reinterpret_cast<Fn>(dlsym(handle, name));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

/// A dlopen'ed library.
class Library {
public:
    Library() = default;
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    Library(Library&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Library& operator=(Library&& other) noexcept
    {
        if (this != &other) {
            close();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    ~Library() { close(); }

    /// `missing` must be a string literal (Error::what).
    [[nodiscard]] static Result<Library> open(const char* name, std::string_view missing)
    {
        Library lib;
        lib.handle_ = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (lib.handle_ == nullptr) {
            const char* reason = dlerror();
            log::debug(component, "dlopen {}: {}", name, reason != nullptr ? reason : "unknown error");
            return fail(Errc::unsupported, missing);
        }
        return lib;
    }

    template <class Fn>
    bool get(Fn& fn, const char* name) const
    {
        fn = symbol<Fn>(handle_, name);
        if (fn == nullptr) {
            log::warn(component, "the NVIDIA driver library lacks {}", name);
        }
        return fn != nullptr;
    }

private:
    void close() noexcept
    {
        if (handle_ != nullptr) {
            dlclose(handle_);
            handle_ = nullptr;
        }
    }

    void* handle_ = nullptr;
};

/// The CUDA driver API (libcuda.so.1).
struct Cuda {
    Library lib;
    abi::cuInit_t init = nullptr;
    abi::cuDriverGetVersion_t driver_get_version = nullptr;
    abi::cuGetErrorName_t get_error_name = nullptr;
    abi::cuDeviceGetCount_t device_get_count = nullptr;
    abi::cuDeviceGet_t device_get = nullptr;
    abi::cuDeviceGetByPCIBusId_t device_get_by_pci_bus_id = nullptr;
    abi::cuDeviceGetPCIBusId_t device_get_pci_bus_id = nullptr;
    abi::cuDeviceGetName_t device_get_name = nullptr;
    abi::cuCtxCreate_v2_t ctx_create = nullptr;
    abi::cuCtxDestroy_v2_t ctx_destroy = nullptr;
    abi::cuCtxPushCurrent_v2_t ctx_push = nullptr;
    abi::cuCtxPopCurrent_v2_t ctx_pop = nullptr;
    abi::cuCtxSynchronize_t ctx_synchronize = nullptr;
    abi::cuMemAllocPitch_v2_t mem_alloc_pitch = nullptr;
    abi::cuMemFree_v2_t mem_free = nullptr;
    abi::cuMemsetD8_v2_t memset_d8 = nullptr;
    abi::cuMemcpy2D_v2_t memcpy_2d = nullptr;
    abi::cuModuleLoadDataEx_t module_load_data_ex = nullptr;
    abi::cuModuleUnload_t module_unload = nullptr;
    abi::cuModuleGetFunction_t module_get_function = nullptr;
    abi::cuLaunchKernel_t launch_kernel = nullptr;
    abi::cuGraphicsGLRegisterImage_t graphics_gl_register_image = nullptr;
    abi::cuGraphicsUnregisterResource_t graphics_unregister_resource = nullptr;
    abi::cuGraphicsMapResources_t graphics_map_resources = nullptr;
    abi::cuGraphicsUnmapResources_t graphics_unmap_resources = nullptr;
    abi::cuGraphicsSubResourceGetMappedArray_t graphics_sub_resource_get_mapped_array = nullptr;

    [[nodiscard]] static Result<std::unique_ptr<Cuda>> load()
    {
        auto cu = std::make_unique<Cuda>();
        FARLAND_TRY(cu->lib, Library::open("libcuda.so.1", "CUDA driver library (libcuda.so.1) not found"));
        bool ok = cu->lib.get(cu->init, "cuInit");
        ok = cu->lib.get(cu->driver_get_version, "cuDriverGetVersion") && ok;
        ok = cu->lib.get(cu->get_error_name, "cuGetErrorName") && ok;
        ok = cu->lib.get(cu->device_get_count, "cuDeviceGetCount") && ok;
        ok = cu->lib.get(cu->device_get, "cuDeviceGet") && ok;
        ok = cu->lib.get(cu->device_get_by_pci_bus_id, "cuDeviceGetByPCIBusId") && ok;
        ok = cu->lib.get(cu->device_get_pci_bus_id, "cuDeviceGetPCIBusId") && ok;
        ok = cu->lib.get(cu->device_get_name, "cuDeviceGetName") && ok;
        ok = cu->lib.get(cu->ctx_create, "cuCtxCreate_v2") && ok;
        ok = cu->lib.get(cu->ctx_destroy, "cuCtxDestroy_v2") && ok;
        ok = cu->lib.get(cu->ctx_push, "cuCtxPushCurrent_v2") && ok;
        ok = cu->lib.get(cu->ctx_pop, "cuCtxPopCurrent_v2") && ok;
        ok = cu->lib.get(cu->ctx_synchronize, "cuCtxSynchronize") && ok;
        ok = cu->lib.get(cu->mem_alloc_pitch, "cuMemAllocPitch_v2") && ok;
        ok = cu->lib.get(cu->mem_free, "cuMemFree_v2") && ok;
        ok = cu->lib.get(cu->memset_d8, "cuMemsetD8_v2") && ok;
        ok = cu->lib.get(cu->memcpy_2d, "cuMemcpy2D_v2") && ok;
        ok = cu->lib.get(cu->module_load_data_ex, "cuModuleLoadDataEx") && ok;
        ok = cu->lib.get(cu->module_unload, "cuModuleUnload") && ok;
        ok = cu->lib.get(cu->module_get_function, "cuModuleGetFunction") && ok;
        ok = cu->lib.get(cu->launch_kernel, "cuLaunchKernel") && ok;
        ok = cu->lib.get(cu->graphics_gl_register_image, "cuGraphicsGLRegisterImage") && ok;
        ok = cu->lib.get(cu->graphics_unregister_resource, "cuGraphicsUnregisterResource") && ok;
        ok = cu->lib.get(cu->graphics_map_resources, "cuGraphicsMapResources") && ok;
        ok = cu->lib.get(cu->graphics_unmap_resources, "cuGraphicsUnmapResources") && ok;
        ok = cu->lib.get(cu->graphics_sub_resource_get_mapped_array, "cuGraphicsSubResourceGetMappedArray") && ok;
        if (!ok) {
            return fail(Errc::unsupported, "libcuda.so.1 lacks CUDA driver functions");
        }
        return cu;
    }

    [[nodiscard]] std::string_view name(abi::CUresult result) const
    {
        const char* text = nullptr;
        if (get_error_name(result, &text) == abi::cuda_success && text != nullptr) {
            return text;
        }
        return "unknown CUDA error";
    }

    /// `what` must be a string literal (Error::what).
    [[nodiscard]] Result<void> check(abi::CUresult result, std::string_view what) const
    {
        if (result == abi::cuda_success) {
            return {};
        }
        log::warn(component, "{}: {}", what, name(result));
        return fail(Errc::io, what);
    }
};

/// NVENC (libnvidia-encode.so.1).
struct Nvenc {
    Library lib;
    abi::NV_ENCODE_API_FUNCTION_LIST api{};
    std::uint32_t max_version = 0;  ///< (major << 4) | minor

    [[nodiscard]] static Result<std::unique_ptr<Nvenc>> load()
    {
        auto nv = std::make_unique<Nvenc>();
        FARLAND_TRY(nv->lib, Library::open("libnvidia-encode.so.1", "NVENC library (libnvidia-encode.so.1) not found"));
        abi::NvEncodeAPIGetMaxSupportedVersionFn get_max_version = nullptr;
        abi::NvEncodeAPICreateInstanceFn create_instance = nullptr;
        if (!nv->lib.get(get_max_version, "NvEncodeAPIGetMaxSupportedVersion") ||
            !nv->lib.get(create_instance, "NvEncodeAPICreateInstance")) {
            return fail(Errc::unsupported, "libnvidia-encode.so.1 lacks the NVENC entry points");
        }
        if (get_max_version(&nv->max_version) != abi::nv_enc_success) {
            return fail(Errc::unsupported, "NVENC does not report its version");
        }
        constexpr std::uint32_t needed = (abi::api_major << 4U) | abi::api_minor;
        if (nv->max_version < needed) {
            log::warn(component,
                      "the NVIDIA driver implements NVENC API {}.{}; farland needs {}.{} (driver 520 or newer)",
                      nv->max_version >> 4U, nv->max_version & 0xFU, abi::api_major, abi::api_minor);
            return fail(Errc::unsupported, "the NVIDIA driver is too old for NVENC API 12.0");
        }
        nv->api.version = abi::encode_api_function_list_ver;
        if (create_instance(&nv->api) != abi::nv_enc_success) {
            return fail(Errc::unsupported, "NvEncodeAPICreateInstance failed");
        }
        const auto& a = nv->api;
        if (a.nvEncOpenEncodeSessionEx == nullptr || a.nvEncGetEncodeCaps == nullptr ||
            a.nvEncGetEncodePresetConfigEx == nullptr || a.nvEncInitializeEncoder == nullptr ||
            a.nvEncCreateInputBuffer == nullptr || a.nvEncDestroyInputBuffer == nullptr ||
            a.nvEncCreateBitstreamBuffer == nullptr || a.nvEncDestroyBitstreamBuffer == nullptr ||
            a.nvEncEncodePicture == nullptr || a.nvEncLockBitstream == nullptr || a.nvEncUnlockBitstream == nullptr ||
            a.nvEncLockInputBuffer == nullptr || a.nvEncUnlockInputBuffer == nullptr ||
            a.nvEncMapInputResource == nullptr || a.nvEncUnmapInputResource == nullptr ||
            a.nvEncDestroyEncoder == nullptr || a.nvEncRegisterResource == nullptr ||
            a.nvEncUnregisterResource == nullptr || a.nvEncReconfigureEncoder == nullptr ||
            a.nvEncGetLastErrorString == nullptr) {
            return fail(Errc::unsupported, "NVENC function list is incomplete");
        }
        return nv;
    }
};

/// EGL and the few OpenGL functions of the dmabuf route (libEGL.so.1, the
/// GL functions through eglGetProcAddress as EGL_KHR_client_get_all_proc_addresses allows).
struct Gl {
    Library lib;
    abi::eglGetProcAddress_t get_proc_address = nullptr;
    abi::eglGetError_t get_error = nullptr;
    abi::eglInitialize_t initialize = nullptr;
    abi::eglQueryString_t query_string = nullptr;
    abi::eglBindAPI_t bind_api = nullptr;
    abi::eglCreateContext_t create_context = nullptr;
    abi::eglDestroyContext_t destroy_context = nullptr;
    abi::eglMakeCurrent_t make_current = nullptr;
    abi::eglGetCurrentContext_t get_current_context = nullptr;
    abi::eglGetCurrentDisplay_t get_current_display = nullptr;
    abi::eglGetCurrentSurface_t get_current_surface = nullptr;
    abi::eglQueryDevicesEXT_t query_devices = nullptr;
    abi::eglQueryDeviceAttribEXT_t query_device_attrib = nullptr;
    abi::eglGetPlatformDisplayEXT_t get_platform_display = nullptr;
    abi::eglCreateImageKHR_t create_image = nullptr;
    abi::eglDestroyImageKHR_t destroy_image = nullptr;
    abi::glGetError_t gl_get_error = nullptr;
    abi::glGenTextures_t gen_textures = nullptr;
    abi::glDeleteTextures_t delete_textures = nullptr;
    abi::glBindTexture_t bind_texture = nullptr;
    abi::glTexParameteri_t tex_parameteri = nullptr;
    abi::glTexStorage2D_t tex_storage_2d = nullptr;
    abi::glCopyImageSubData_t copy_image_sub_data = nullptr;
    abi::glFlush_t flush = nullptr;
    abi::glFinish_t finish = nullptr;
    abi::glEGLImageTargetTexture2DOES_t image_target_texture_2d = nullptr;

    abi::EGLDisplay display = nullptr;
    abi::EGLContext context = nullptr;

    template <class Fn>
    bool proc(Fn& fn, const char* name) const
    {
        // eglGetProcAddress returns a generic function pointer.
        fn = reinterpret_cast<Fn>(get_proc_address(name));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        if (fn == nullptr) {
            log::debug(component, "eglGetProcAddress({}) failed", name);
        }
        return fn != nullptr;
    }

    [[nodiscard]] static Result<std::unique_ptr<Gl>> load(int cuda_device);

    /// Discards OpenGL errors left by earlier calls.
    void clear_errors() const
    {
        for (int i = 0; i < 16 && gl_get_error() != abi::gl_no_error; ++i) {
        }
    }
};

Result<std::unique_ptr<Gl>> Gl::load(int cuda_device)
{
    auto gl = std::make_unique<Gl>();
    FARLAND_TRY(gl->lib, Library::open("libEGL.so.1", "EGL library (libEGL.so.1) not found"));
    if (!gl->lib.get(gl->get_proc_address, "eglGetProcAddress")) {
        return fail(Errc::unsupported, "libEGL.so.1 lacks eglGetProcAddress");
    }
    bool ok = gl->lib.get(gl->get_error, "eglGetError");
    ok = gl->lib.get(gl->initialize, "eglInitialize") && ok;
    ok = gl->lib.get(gl->query_string, "eglQueryString") && ok;
    ok = gl->lib.get(gl->bind_api, "eglBindAPI") && ok;
    ok = gl->lib.get(gl->create_context, "eglCreateContext") && ok;
    ok = gl->lib.get(gl->destroy_context, "eglDestroyContext") && ok;
    ok = gl->lib.get(gl->make_current, "eglMakeCurrent") && ok;
    ok = gl->lib.get(gl->get_current_context, "eglGetCurrentContext") && ok;
    ok = gl->lib.get(gl->get_current_display, "eglGetCurrentDisplay") && ok;
    ok = gl->lib.get(gl->get_current_surface, "eglGetCurrentSurface") && ok;
    ok = gl->proc(gl->query_devices, "eglQueryDevicesEXT") && ok;
    ok = gl->proc(gl->query_device_attrib, "eglQueryDeviceAttribEXT") && ok;
    ok = gl->proc(gl->get_platform_display, "eglGetPlatformDisplayEXT") && ok;
    ok = gl->proc(gl->create_image, "eglCreateImageKHR") && ok;
    ok = gl->proc(gl->destroy_image, "eglDestroyImageKHR") && ok;
    ok = gl->proc(gl->gl_get_error, "glGetError") && ok;
    ok = gl->proc(gl->gen_textures, "glGenTextures") && ok;
    ok = gl->proc(gl->delete_textures, "glDeleteTextures") && ok;
    ok = gl->proc(gl->bind_texture, "glBindTexture") && ok;
    ok = gl->proc(gl->tex_parameteri, "glTexParameteri") && ok;
    ok = gl->proc(gl->tex_storage_2d, "glTexStorage2D") && ok;
    ok = gl->proc(gl->copy_image_sub_data, "glCopyImageSubData") && ok;
    ok = gl->proc(gl->flush, "glFlush") && ok;
    ok = gl->proc(gl->finish, "glFinish") && ok;
    ok = gl->proc(gl->image_target_texture_2d, "glEGLImageTargetTexture2DOES") && ok;
    if (!ok) {
        return fail(Errc::unsupported, "EGL lacks the device, dmabuf or OpenGL functions");
    }

    // The EGL device of our CUDA device.
    constexpr int max_devices = 16;
    std::array<abi::EGLDeviceEXT, max_devices> devices{};
    abi::EGLint count = 0;
    if (gl->query_devices(max_devices, devices.data(), &count) == 0) {
        return fail(Errc::unsupported, "eglQueryDevicesEXT failed");
    }
    for (auto* const device : std::span(devices).first(static_cast<std::size_t>(std::max(count, 0)))) {
        abi::EGLAttrib ordinal = -1;
        if (gl->query_device_attrib(device, abi::egl_cuda_device_nv, &ordinal) != 0 && ordinal == cuda_device) {
            gl->display = gl->get_platform_display(abi::egl_platform_device_ext, device, nullptr);
            break;
        }
    }
    if (gl->display == nullptr) {
        return fail(Errc::unsupported, "no EGL device belongs to the CUDA device");
    }
    abi::EGLint major = 0;
    abi::EGLint minor = 0;
    if (gl->initialize(gl->display, &major, &minor) == 0) {
        // Typically no access to the render node (not in the render group, no seat).
        log::info(component, "eglInitialize on the NVIDIA device failed (EGL error {:#x})", gl->get_error());
        return fail(Errc::unsupported, "EGL cannot open the NVIDIA device");
    }
    const char* extensions = gl->query_string(gl->display, abi::egl_extensions);
    const std::string_view list = extensions != nullptr ? extensions : "";
    for (const std::string_view needed :
         {"EGL_EXT_image_dma_buf_import", "EGL_KHR_no_config_context", "EGL_KHR_surfaceless_context"}) {
        if (list.find(needed) == std::string_view::npos) {
            log::info(component, "the NVIDIA EGL display lacks {}", needed);
            return fail(Errc::unsupported, "the NVIDIA EGL display cannot import dmabufs");
        }
    }
    if (gl->bind_api(abi::egl_opengl_api) == 0) {
        return fail(Errc::unsupported, "EGL has no desktop OpenGL");
    }
    const std::array<abi::EGLint, 1> attributes{abi::egl_none};
    gl->context = gl->create_context(gl->display, nullptr, nullptr, attributes.data());
    if (gl->context == nullptr) {
        log::info(component, "eglCreateContext failed (EGL error {:#x})", gl->get_error());
        return fail(Errc::unsupported, "no OpenGL context on the NVIDIA device");
    }
    return gl;
}

/// Makes a CUDA context current for its lifetime.
class CudaScope {
public:
    CudaScope(const Cuda& cu, abi::CUcontext context) : cu_(cu), pushed_(cu.ctx_push(context) == abi::cuda_success) {}
    CudaScope(const CudaScope&) = delete;
    CudaScope& operator=(const CudaScope&) = delete;
    CudaScope(CudaScope&&) = delete;
    CudaScope& operator=(CudaScope&&) = delete;
    ~CudaScope()
    {
        if (pushed_) {
            abi::CUcontext popped = nullptr;
            cu_.ctx_pop(&popped);
        }
    }

private:
    const Cuda& cu_;
    bool pushed_;
};

/// Makes our OpenGL context current for its lifetime and restores whatever
/// was current before (the caller's thread may use EGL itself).
class GlScope {
public:
    explicit GlScope(const Gl* gl) : gl_(gl)
    {
        if (gl_ == nullptr) {
            return;
        }
        display_ = gl_->get_current_display();
        context_ = gl_->get_current_context();
        draw_ = gl_->get_current_surface(abi::egl_draw);
        read_ = gl_->get_current_surface(abi::egl_read);
        if (context_ != gl_->context) {
            current_ = gl_->make_current(gl_->display, nullptr, nullptr, gl_->context) != 0;
            switched_ = true;
        } else {
            current_ = true;
        }
    }
    GlScope(const GlScope&) = delete;
    GlScope& operator=(const GlScope&) = delete;
    GlScope(GlScope&&) = delete;
    GlScope& operator=(GlScope&&) = delete;
    ~GlScope()
    {
        if (gl_ == nullptr || !switched_) {
            return;
        }
        if (context_ != nullptr) {
            gl_->make_current(display_, draw_, read_, context_);
        } else {
            gl_->make_current(gl_->display, nullptr, nullptr, nullptr);
        }
    }

    [[nodiscard]] bool current() const noexcept { return current_; }

private:
    const Gl* gl_;
    abi::EGLDisplay display_ = nullptr;
    abi::EGLContext context_ = nullptr;
    abi::EGLSurface draw_ = nullptr;
    abi::EGLSurface read_ = nullptr;
    bool switched_ = false;
    bool current_ = false;
};

/// The PCI address of a DRM node, from sysfs ("0000:01:00.0").
[[nodiscard]] std::string pci_address(const std::string& node)
{
    std::error_code error;
    const auto name = std::filesystem::path(node).filename();
    const auto device = std::filesystem::canonical(std::filesystem::path("/sys/class/drm") / name / "device", error);
    return error ? std::string{} : device.filename().string();
}

/// The render node of a PCI address, or empty.
[[nodiscard]] std::string render_node_of(std::string_view pci)
{
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm", error)) {
        const auto name = entry.path().filename().string();
        if (name.starts_with("renderD")) {
            const auto node = "/dev/dri/" + name;
            if (pci_address(node) == pci) {
                return node;
            }
        }
    }
    return {};
}

/// A pitched CUDA allocation.
struct DeviceBuffer {
    abi::CUdeviceptr pointer = 0;
    std::size_t pitch = 0;
};

/// What identifies an imported dmabuf.
struct ImportKey {
    dev_t device = 0;
    ino_t inode = 0;
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t plane_count = 0;
    std::array<std::uint32_t, 4> offsets{};
    std::array<std::uint32_t, 4> pitches{};

    friend bool operator==(const ImportKey&, const ImportKey&) = default;
};

struct Import {
    ImportKey key;
    abi::EGLImageKHR image = nullptr;
    unsigned texture = 0;
    unsigned target = 0;
    std::uint64_t last_use = 0;
};

}  // namespace

struct NvencEncoder::Impl {
    std::unique_ptr<Cuda> cu;
    std::unique_ptr<Nvenc> nv;
    std::unique_ptr<Gl> gl;  ///< nullptr: no dmabuf route
    DeviceInfo info;
    Tuning tuning;
    ColorSpace color;
    Coefficients coeffs;
    abi::CUdevice device = 0;
    abi::CUcontext context = nullptr;
    abi::CUmodule module = nullptr;
    abi::CUfunction kernel = nullptr;

    EncoderConfig config;
    void* session = nullptr;
    abi::NV_ENC_INITIALIZE_PARAMS init{};
    abi::NV_ENC_CONFIG encode_config{};
    void* bitstream = nullptr;
    void* host_input = nullptr;  ///< IYUV input buffer of encode()
    DeviceBuffer nv12;           ///< GPU conversion output
    void* nv12_registered = nullptr;
    DeviceBuffer staging;                ///< BGRX of the frame, linear
    void* staging_registered = nullptr;  ///< Tuning::nvenc_rgb_conversion only
    /// RGBA8 texture of the config size that dmabufs are copied into, for CUDA to read.
    unsigned copy_texture = 0;
    abi::CUgraphicsResource copy_resource = nullptr;
    std::vector<Import> imports;
    std::uint64_t import_clock = 0;
    bool force_idr = true;
    std::uint64_t frames = 0;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl();

    [[nodiscard]] static Result<std::unique_ptr<Impl>> open(const std::string& render_node);

    /// `what` must be a string literal. Logs NVENC's own description.
    [[nodiscard]] Result<void> check(abi::NVENCSTATUS status, std::string_view what, Errc code = Errc::io) const;
    [[nodiscard]] bool gpu_conversion() const noexcept { return kernel != nullptr || tuning.nvenc_rgb_conversion; }
    [[nodiscard]] bool dmabuf_ready() const noexcept { return gl != nullptr && gpu_conversion(); }

    [[nodiscard]] Result<void> configure(const EncoderConfig& next);
    [[nodiscard]] Result<void> open_session();
    void close_session() noexcept;
    static void fill_rate(abi::NV_ENC_RC_PARAMS& rc, const RateControl& rate);
    [[nodiscard]] Result<void> set_rate_control(const RateControl& rate);

    [[nodiscard]] Result<void> ensure_gpu_buffers();
    [[nodiscard]] Result<void> ensure_copy_texture();
    [[nodiscard]] Result<const Import*> import(const DmabufFrame& frame);
    void forget_imports() noexcept;
    [[nodiscard]] Result<void> copy_dmabuf(const DmabufFrame& frame);
    [[nodiscard]] Result<void> convert_staging(std::uint32_t width, std::uint32_t height, const Channels& order) const;
    [[nodiscard]] Result<codec::Yuv420Frame> read_nv12() const;

    [[nodiscard]] Result<EncodedFrame> encode_input(void* input, std::uint32_t format, std::uint32_t pitch,
                                                    const FrameOptions& options);
    [[nodiscard]] Result<EncodedFrame> encode_registered(void* registered, std::uint32_t pitch,
                                                         const FrameOptions& options);
    [[nodiscard]] Result<EncodedFrame> encode(const codec::Yuv420View& picture, const FrameOptions& options);
    [[nodiscard]] Result<void> check_frame(const DmabufFrame& frame) const;
    [[nodiscard]] Result<EncodedFrame> encode_dmabuf(const DmabufFrame& frame, const FrameOptions& options);
};

Result<std::unique_ptr<NvencEncoder::Impl>> NvencEncoder::Impl::open(const std::string& render_node)
{
    auto impl = std::make_unique<Impl>();
    FARLAND_TRY(impl->cu, Cuda::load());
    const Cuda& cu = *impl->cu;
    if (const auto result = cu.init(0); result != abi::cuda_success) {
        // CUDA_ERROR_NO_DEVICE and friends: an NVIDIA library without a usable GPU.
        log::debug(component, "cuInit: {}", cu.name(result));
        return fail(Errc::unsupported, "no NVIDIA GPU is usable through CUDA");
    }
    if (!render_node.empty()) {
        const auto pci = pci_address(render_node);
        if (pci.empty() || cu.device_get_by_pci_bus_id(&impl->device, pci.c_str()) != abi::cuda_success) {
            log::debug(component, "{} ({}) is not a CUDA device", render_node, pci);
            return fail(Errc::unsupported, "the render node is not an NVIDIA GPU");
        }
    } else {
        int count = 0;
        if (cu.device_get_count(&count) != abi::cuda_success || count < 1 ||
            cu.device_get(&impl->device, 0) != abi::cuda_success) {
            return fail(Errc::unsupported, "no NVIDIA GPU is usable through CUDA");
        }
    }
    DeviceInfo& info = impl->info;
    info.cuda_device = impl->device;
    std::array<char, 256> name{};
    if (cu.device_get_name(name.data(), static_cast<int>(name.size()), impl->device) == abi::cuda_success) {
        info.name = name.data();
    }
    std::array<char, 32> bus{};
    if (cu.device_get_pci_bus_id(bus.data(), static_cast<int>(bus.size()), impl->device) == abi::cuda_success) {
        info.pci_bus_id = bus.data();
        std::ranges::transform(info.pci_bus_id, info.pci_bus_id.begin(),
                               [](char c) { return c >= 'A' && c <= 'F' ? static_cast<char>(c - 'A' + 'a') : c; });
    }
    info.render_node = render_node.empty() ? render_node_of(info.pci_bus_id) : render_node;
    cu.driver_get_version(&info.cuda_driver_version);

    FARLAND_TRY(impl->nv, Nvenc::load());
    info.nvenc_api_major = impl->nv->max_version >> 4U;
    info.nvenc_api_minor = impl->nv->max_version & 0xFU;

    // Blocking sync: waiting for the GPU sleeps instead of spinning a core.
    FARLAND_TRY_VOID(
        cu.check(cu.ctx_create(&impl->context, abi::cu_ctx_sched_blocking_sync, impl->device), "cuCtxCreate failed"));
    const CudaScope scope(cu, impl->context);

    // The conversion kernel; without it only encode() works.
    std::array<char, 4096> jit_log{};
    std::array<int, 2> keys{abi::cu_jit_error_log_buffer, abi::cu_jit_error_log_buffer_size_bytes};
    // CUDA passes option values as pointers, sizes cast to pointers.
    std::array<void*, 2> values{jit_log.data(), reinterpret_cast<void*>(jit_log.size())};  // NOLINT
    const std::string ptx(bgrx_to_nv12_ptx);
    if (const auto result = cu.module_load_data_ex(&impl->module, ptx.c_str(), static_cast<unsigned>(keys.size()),
                                                   keys.data(), values.data());
        result != abi::cuda_success) {
        log::warn(component, "the driver cannot compile the colour conversion ({}): {}", cu.name(result),
                  jit_log.data());
        impl->module = nullptr;
    } else if (cu.module_get_function(&impl->kernel, impl->module, "bgrx_to_nv12") != abi::cuda_success) {
        impl->kernel = nullptr;
    }

    auto gl = Gl::load(impl->device);
    if (gl.has_value()) {
        impl->gl = std::move(*gl);
    } else {
        log::info(component, "no zero-copy dmabuf input: {}", gl.error().message());
    }
    info.dmabuf = impl->dmabuf_ready();
    return impl;
}

NvencEncoder::Impl::~Impl()
{
    if (context == nullptr) {
        return;
    }
    {
        const CudaScope scope(*cu, context);
        close_session();
        forget_imports();
        if (module != nullptr) {
            cu->module_unload(module);
        }
    }
    if (gl != nullptr && gl->context != nullptr) {
        // The display stays initialised: EGL displays are shared in a process.
        gl->destroy_context(gl->display, gl->context);
    }
    cu->ctx_destroy(context);
}

Result<void> NvencEncoder::Impl::check(abi::NVENCSTATUS status, std::string_view what, Errc code) const
{
    if (status == abi::nv_enc_success) {
        return {};
    }
    const char* detail = session != nullptr ? nv->api.nvEncGetLastErrorString(session) : nullptr;
    log::warn(component, "{}: NVENC status {} ({})", what, status, detail != nullptr ? detail : "");
    return fail(code, what);
}

void NvencEncoder::Impl::fill_rate(abi::NV_ENC_RC_PARAMS& rc, const RateControl& rate)
{
    constexpr std::uint64_t kilo = 1000;
    const auto bits = [](std::uint64_t value) {
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(value, std::numeric_limits<std::uint32_t>::max()));
    };
    rc.version = abi::rc_params_ver;
    rc.constQP = {};
    rc.averageBitRate = 0;
    rc.maxBitRate = 0;
    rc.vbvBufferSize = 0;
    rc.vbvInitialDelay = 0;
    rc.targetQuality = 0;
    rc.targetQualityLSB = 0;
    rc.enableMinQP = 0;
    rc.enableMaxQP = 0;
    rc.enableAQ = 0;
    rc.enableTemporalAQ = 0;
    rc.enableLookahead = 0;
    rc.zeroReorderDelay = 1;
    rc.multiPass = 0;  // one pass: the second pass costs latency

    std::uint32_t peak_kbps = 0;
    if (rate.mode == RateControl::Mode::constant_quality) {
        if (rate.max_bitrate_kbps == 0) {
            rc.rateControlMode = abi::rc_constqp;
            rc.constQP = {.qpInterP = rate.quality, .qpInterB = rate.quality, .qpIntra = rate.quality};
            return;
        }
        // NVENC's constant quality: VBR without an average, capped.
        rc.rateControlMode = abi::rc_vbr;
        rc.targetQuality = std::max<std::uint8_t>(rate.quality, 1);  // 0 would mean "automatic"
        peak_kbps = rate.max_bitrate_kbps;
        rc.maxBitRate = bits(std::uint64_t{peak_kbps} * kilo);
    } else if (rate.max_bitrate_kbps == 0 || rate.max_bitrate_kbps == rate.bitrate_kbps) {
        rc.rateControlMode = abi::rc_cbr;
        peak_kbps = rate.bitrate_kbps;
        rc.averageBitRate = bits(std::uint64_t{peak_kbps} * kilo);
        rc.maxBitRate = rc.averageBitRate;
    } else {
        rc.rateControlMode = abi::rc_vbr;
        peak_kbps = rate.max_bitrate_kbps;
        rc.averageBitRate = bits(std::uint64_t{rate.bitrate_kbps} * kilo);
        rc.maxBitRate = bits(std::uint64_t{peak_kbps} * kilo);
    }
    // kbit/s times ms is bits.
    rc.vbvBufferSize = bits(std::uint64_t{peak_kbps} * rate.vbv_window_ms);
    rc.vbvInitialDelay = rc.vbvBufferSize;
}

Result<void> NvencEncoder::Impl::open_session()
{
    const auto& api = nv->api;
    abi::NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
    params.version = abi::open_encode_session_ex_params_ver;
    params.deviceType = abi::device_type_cuda;
    params.device = context;
    params.apiVersion = abi::api_version;
    if (const auto status = api.nvEncOpenEncodeSessionEx(&params, &session); status != abi::nv_enc_success) {
        session = nullptr;
        // No encoder on this GPU, or all sessions taken (GeForce drivers limit them).
        const bool unavailable =
            status == abi::nv_enc_err_no_encode_device || status == abi::nv_enc_err_unsupported_device ||
            status == abi::nv_enc_err_out_of_memory || status == abi::nv_enc_err_incompatible_client_key;
        log::info(component, "nvEncOpenEncodeSessionEx: NVENC status {}", status);
        return fail(unavailable ? Errc::unsupported : Errc::io, "NVENC cannot open an encode session");
    }

    auto caps = [&](std::uint32_t query) {
        abi::NV_ENC_CAPS_PARAM param{};
        param.version = abi::caps_param_ver;
        param.capsToQuery = query;
        int value = 0;
        return api.nvEncGetEncodeCaps(session, abi::codec_h264_guid, &param, &value) == abi::nv_enc_success
                   ? static_cast<std::uint32_t>(std::max(value, 0))
                   : 0U;
    };
    info.min_width = caps(abi::caps_width_min);
    info.min_height = caps(abi::caps_height_min);
    info.max_width = caps(abi::caps_width_max);
    info.max_height = caps(abi::caps_height_max);
    if ((info.max_width != 0 && config.width > info.max_width) ||
        (info.max_height != 0 && config.height > info.max_height) || config.width < info.min_width ||
        config.height < info.min_height) {
        log::info(component, "NVENC encodes H.264 from {}x{} to {}x{}, not {}x{}", info.min_width, info.min_height,
                  info.max_width, info.max_height, config.width, config.height);
        return fail(Errc::unsupported, "NVENC does not encode pictures of this size");
    }

    const auto preset = abi::preset_guids.at(static_cast<std::size_t>(tuning.preset - 1U));
    const std::uint32_t tuning_info =
        tuning.ultra_low_latency ? abi::tuning_info_ultra_low_latency : abi::tuning_info_low_latency;
    auto preset_config = std::make_unique<abi::NV_ENC_PRESET_CONFIG>();
    preset_config->version = abi::preset_config_ver;
    preset_config->presetCfg.version = abi::config_ver;
    FARLAND_TRY_VOID(
        check(api.nvEncGetEncodePresetConfigEx(session, abi::codec_h264_guid, preset, tuning_info, preset_config.get()),
              "NVENC has no such preset"));

    abi::NV_ENC_CONFIG& c = encode_config;
    c = preset_config->presetCfg;
    c.version = abi::config_ver;
    switch (config.profile) {
    case Profile::constrained_baseline:
        c.profileGUID = abi::h264_profile_baseline_guid;
        break;
    case Profile::main:
        c.profileGUID = abi::h264_profile_main_guid;
        break;
    case Profile::high:
        c.profileGUID = abi::h264_profile_high_guid;
        break;
    }
    c.gopLength = config.keyint != 0 ? config.keyint : abi::infinite_goplength;
    c.frameIntervalP = 1;  // I and P only
    fill_rate(c.rcParams, config.rate);

    abi::NV_ENC_CONFIG_H264& h = c.h264Config;
    h.idrPeriod = c.gopLength;
    h.outputAUD = config.access_unit_delimiters ? 1U : 0U;
    h.repeatSPSPPS = 1;
    h.disableSPSPPS = 0;
    h.outputBufferingPeriodSEI = 0;
    h.outputPictureTimingSEI = 0;
    h.outputRecoveryPointSEI = 0;
    h.enableIntraRefresh = 0;
    h.enableLTR = 0;
    h.enableFillerDataInsertion = 0;
    h.level = 0;  // autoselect
    h.chromaFormatIDC = 1;
    h.maxNumRefFrames = 1;
    h.sliceMode = 0;
    h.sliceModeData = 0;
    const bool baseline = config.profile == Profile::constrained_baseline;
    h.entropyCodingMode = baseline ? abi::entropy_coding_cavlc : abi::entropy_coding_cabac;
    h.adaptiveTransformMode =
        config.profile == Profile::high ? abi::adaptive_transform_autoselect : abi::adaptive_transform_disable;
    abi::NV_ENC_CONFIG_H264_VUI_PARAMETERS& vui = h.h264VUIParameters;
    vui.videoSignalTypePresentFlag = 1;
    vui.videoFormat = abi::vui_video_format_unspecified;
    vui.videoFullRangeFlag = color.full_range ? 1U : 0U;
    vui.colourDescriptionPresentFlag = 1;
    vui.colourPrimaries = abi::vui_color_primaries_bt709;
    vui.transferCharacteristics = abi::vui_transfer_srgb;
    vui.colourMatrix = color.matrix == ColorSpace::Matrix::bt709 ? abi::vui_matrix_bt709 : abi::vui_matrix_smpte170m;
    vui.bitstreamRestrictionFlag = 1;  // no reordering: decoders output every picture at once

    init = {};
    init.version = abi::initialize_params_ver;
    init.encodeGUID = abi::codec_h264_guid;
    init.presetGUID = preset;
    init.encodeWidth = config.width;
    init.encodeHeight = config.height;
    init.darWidth = config.width;
    init.darHeight = config.height;
    init.frameRateNum = config.fps;
    init.frameRateDen = 1;
    init.enableEncodeAsync = 0;
    init.enablePTD = 1;
    init.encodeConfig = &encode_config;
    init.tuningInfo = tuning_info;
    FARLAND_TRY_VOID(check(api.nvEncInitializeEncoder(session, &init), "NVENC rejected the encoder configuration",
                           Errc::invalid_value));

    abi::NV_ENC_CREATE_BITSTREAM_BUFFER output{};
    output.version = abi::create_bitstream_buffer_ver;
    FARLAND_TRY_VOID(check(api.nvEncCreateBitstreamBuffer(session, &output), "NVENC cannot create an output buffer"));
    bitstream = output.bitstreamBuffer;
    return {};
}

void NvencEncoder::Impl::close_session() noexcept
{
    const auto& api = nv->api;
    if (session != nullptr) {
        for (void** registered : {&nv12_registered, &staging_registered}) {
            if (*registered != nullptr) {
                api.nvEncUnregisterResource(session, *registered);
                *registered = nullptr;
            }
        }
        if (host_input != nullptr) {
            api.nvEncDestroyInputBuffer(session, host_input);
            host_input = nullptr;
        }
        if (bitstream != nullptr) {
            api.nvEncDestroyBitstreamBuffer(session, bitstream);
            bitstream = nullptr;
        }
        api.nvEncDestroyEncoder(session);
        session = nullptr;
    }
    for (DeviceBuffer* buffer : {&nv12, &staging}) {
        if (buffer->pointer != 0) {
            cu->mem_free(buffer->pointer);
            *buffer = {};
        }
    }
    if (copy_resource != nullptr || copy_texture != 0) {
        const GlScope scope(gl.get());
        if (copy_resource != nullptr) {
            cu->graphics_unregister_resource(copy_resource);
            copy_resource = nullptr;
        }
        if (copy_texture != 0 && scope.current()) {
            gl->delete_textures(1, &copy_texture);
        }
        copy_texture = 0;
    }
}

Result<void> NvencEncoder::Impl::configure(const EncoderConfig& next)
{
    FARLAND_TRY_VOID(validate(next));
    const CudaScope scope(*cu, context);
    close_session();
    config = next;
    if (auto opened = open_session(); !opened) {
        close_session();
        return opened;
    }
    force_idr = true;
    frames = 0;
    return {};
}

Result<void> NvencEncoder::Impl::set_rate_control(const RateControl& rate)
{
    FARLAND_TRY_VOID(validate(rate));
    if (session == nullptr) {
        return fail(Errc::io, "NVENC encoder is not configured");
    }
    const CudaScope scope(*cu, context);
    auto next = std::make_unique<abi::NV_ENC_CONFIG>(encode_config);
    fill_rate(next->rcParams, rate);
    if (std::memcmp(&next->rcParams, &encode_config.rcParams, sizeof(abi::NV_ENC_RC_PARAMS)) == 0) {
        config.rate = rate;
        return {};
    }
    // A new mode needs a reset, which starts with an IDR. So does a new QP in
    // constant-QP mode: the driver (595) restarts with an IDR anyway, and
    // asking for it keeps the stream as documented.
    const bool new_mode = next->rcParams.rateControlMode != encode_config.rcParams.rateControlMode ||
                          next->rcParams.rateControlMode == abi::rc_constqp;
    auto params = std::make_unique<abi::NV_ENC_RECONFIGURE_PARAMS>();
    params->version = abi::reconfigure_params_ver;
    params->reInitEncodeParams = init;
    params->reInitEncodeParams.encodeConfig = next.get();
    params->resetEncoder = new_mode ? 1U : 0U;
    params->forceIDR = new_mode ? 1U : 0U;
    if (const auto status = nv->api.nvEncReconfigureEncoder(session, params.get()); status != abi::nv_enc_success) {
        const char* detail = nv->api.nvEncGetLastErrorString(session);
        log::info(component, "nvEncReconfigureEncoder: NVENC status {} ({}); re-opening the encoder", status,
                  detail != nullptr ? detail : "");
        EncoderConfig reopened = config;
        reopened.rate = rate;
        return configure(reopened);
    }
    encode_config = *next;
    config.rate = rate;
    force_idr = force_idr || new_mode;
    return {};
}

Result<void> NvencEncoder::Impl::ensure_gpu_buffers()
{
    const auto& api = nv->api;
    if (nv12.pointer == 0) {
        FARLAND_TRY_VOID(cu->check(
            cu->mem_alloc_pitch(&nv12.pointer, &nv12.pitch, config.width, std::size_t{config.height} * 3U / 2U, 16),
            "cannot allocate the NV12 picture"));
        abi::NV_ENC_REGISTER_RESOURCE registration{};
        registration.version = abi::register_resource_ver;
        registration.resourceType = abi::input_resource_type_cudadeviceptr;
        registration.width = config.width;
        registration.height = config.height;
        registration.pitch = static_cast<std::uint32_t>(nv12.pitch);
        registration.resourceToRegister = reinterpret_cast<void*>(
            nv12.pointer);  // NOLINT(performance-no-int-to-ptr,cppcoreguidelines-pro-type-reinterpret-cast)
        registration.bufferFormat = abi::buffer_format_nv12;
        registration.bufferUsage = abi::buffer_usage_input_image;
        FARLAND_TRY_VOID(check(api.nvEncRegisterResource(session, &registration), "NVENC cannot register the picture"));
        nv12_registered = registration.registeredResource;
    }
    if (staging.pointer == 0) {
        FARLAND_TRY_VOID(cu->check(
            cu->mem_alloc_pitch(&staging.pointer, &staging.pitch, std::size_t{config.width} * 4U, config.height, 16),
            "cannot allocate the staging picture"));
        FARLAND_TRY_VOID(cu->check(cu->memset_d8(staging.pointer, 0, staging.pitch * config.height),
                                   "cannot clear the staging picture"));
        if (tuning.nvenc_rgb_conversion) {
            abi::NV_ENC_REGISTER_RESOURCE registration{};
            registration.version = abi::register_resource_ver;
            registration.resourceType = abi::input_resource_type_cudadeviceptr;
            registration.width = config.width;
            registration.height = config.height;
            registration.pitch = static_cast<std::uint32_t>(staging.pitch);
            registration.resourceToRegister = reinterpret_cast<void*>(
                staging.pointer);  // NOLINT(performance-no-int-to-ptr,cppcoreguidelines-pro-type-reinterpret-cast)
            registration.bufferFormat = abi::buffer_format_argb;
            registration.bufferUsage = abi::buffer_usage_input_image;
            FARLAND_TRY_VOID(
                check(api.nvEncRegisterResource(session, &registration), "NVENC cannot register the RGB picture"));
            staging_registered = registration.registeredResource;
        }
    }
    return {};
}

Result<void> NvencEncoder::Impl::ensure_copy_texture()
{
    if (copy_texture != 0) {
        return {};
    }
    gl->clear_errors();
    gl->gen_textures(1, &copy_texture);
    gl->bind_texture(abi::gl_texture_2d, copy_texture);
    gl->tex_storage_2d(abi::gl_texture_2d, 1, abi::gl_rgba8, static_cast<int>(config.width),
                       static_cast<int>(config.height));
    gl->tex_parameteri(abi::gl_texture_2d, abi::gl_texture_min_filter, abi::gl_nearest);
    gl->tex_parameteri(abi::gl_texture_2d, abi::gl_texture_mag_filter, abi::gl_nearest);
    gl->bind_texture(abi::gl_texture_2d, 0);
    if (gl->gl_get_error() != abi::gl_no_error) {
        return fail(Errc::io, "OpenGL cannot create the copy texture");
    }
    return cu->check(cu->graphics_gl_register_image(&copy_resource, copy_texture, abi::gl_texture_2d,
                                                    abi::cu_graphics_register_flags_read_only),
                     "CUDA cannot register the copy texture");
}

Result<const Import*> NvencEncoder::Impl::import(const DmabufFrame& frame)
{
    struct stat st{};
    if (fstat(frame.planes[0].fd, &st) != 0) {
        return fail(Errc::invalid_value, "dmabuf descriptor is not open");
    }
    ImportKey key{.device = st.st_dev,
                  .inode = st.st_ino,
                  .fourcc = frame.fourcc,
                  .modifier = frame.modifier,
                  .width = frame.width,
                  .height = frame.height,
                  .plane_count = frame.plane_count};
    for (std::uint32_t i = 0; i < frame.plane_count; ++i) {
        key.offsets.at(i) = frame.planes.at(i).offset;
        key.pitches.at(i) = frame.planes.at(i).pitch;
    }
    ++import_clock;
    for (Import& known : imports) {
        if (known.key == key) {
            known.last_use = import_clock;
            return &known;
        }
    }

    std::vector<abi::EGLint> attributes{abi::egl_width,
                                        static_cast<abi::EGLint>(frame.width),
                                        abi::egl_height,
                                        static_cast<abi::EGLint>(frame.height),
                                        abi::egl_linux_drm_fourcc_ext,
                                        static_cast<abi::EGLint>(frame.fourcc)};
    for (std::uint32_t i = 0; i < frame.plane_count; ++i) {
        const auto& names = abi::egl_dma_buf_plane.at(i);
        const auto& plane = frame.planes.at(i);
        attributes.insert(attributes.end(), {names.fd, plane.fd, names.offset, static_cast<abi::EGLint>(plane.offset),
                                             names.pitch, static_cast<abi::EGLint>(plane.pitch)});
        if (frame.modifier != drm_modifier_invalid) {
            attributes.insert(attributes.end(),
                              {names.modifier_lo, static_cast<abi::EGLint>(frame.modifier & 0xFFFF'FFFFU),
                               names.modifier_hi, static_cast<abi::EGLint>(frame.modifier >> 32U)});
        }
    }
    attributes.push_back(abi::egl_none);
    Import imported{.key = key};
    imported.image = gl->create_image(gl->display, nullptr, abi::egl_linux_dma_buf_ext, nullptr, attributes.data());
    if (imported.image == nullptr) {
        log::debug(component, "eglCreateImageKHR of fourcc {:#x} modifier {:#x}: EGL error {:#x}", frame.fourcc,
                   frame.modifier, gl->get_error());
        return fail(Errc::unsupported, "the NVIDIA driver cannot import this dmabuf");
    }
    // Tiled images bind as 2D textures; NVIDIA takes LINEAR ones only as external textures.
    gl->clear_errors();
    for (const unsigned target : {abi::gl_texture_2d, abi::gl_texture_external_oes}) {
        unsigned texture = 0;
        gl->gen_textures(1, &texture);
        gl->bind_texture(target, texture);
        gl->image_target_texture_2d(target, imported.image);
        // One level without mipmaps: glCopyImageSubData refuses incomplete textures.
        gl->tex_parameteri(target, abi::gl_texture_min_filter, abi::gl_nearest);
        gl->tex_parameteri(target, abi::gl_texture_mag_filter, abi::gl_nearest);
        const bool bound = gl->gl_get_error() == abi::gl_no_error;
        gl->bind_texture(target, 0);
        if (bound) {
            imported.texture = texture;
            imported.target = target;
            break;
        }
        gl->delete_textures(1, &texture);
    }
    if (imported.texture == 0) {
        gl->destroy_image(gl->display, imported.image);
        return fail(Errc::unsupported, "OpenGL cannot use this dmabuf as a texture");
    }
    log::debug(component, "imported dmabuf {}x{} fourcc {:#x} modifier {:#x} as {} texture", frame.width, frame.height,
               frame.fourcc, frame.modifier, imported.target == abi::gl_texture_2d ? "2D" : "external");

    if (imports.size() >= max_imports) {
        const auto oldest = std::ranges::min_element(imports, {}, &Import::last_use);
        gl->delete_textures(1, &oldest->texture);
        gl->destroy_image(gl->display, oldest->image);
        imports.erase(oldest);
    }
    imported.last_use = import_clock;
    imports.push_back(imported);
    return &imports.back();
}

void NvencEncoder::Impl::forget_imports() noexcept
{
    if (imports.empty()) {
        return;
    }
    const GlScope scope(gl.get());
    for (const Import& imported : imports) {
        if (scope.current()) {
            gl->delete_textures(1, &imported.texture);
        }
        gl->destroy_image(gl->display, imported.image);
    }
    imports.clear();
}

Result<void> NvencEncoder::Impl::copy_dmabuf(const DmabufFrame& frame)
{
    FARLAND_TRY(const Import* imported, import(frame));
    FARLAND_TRY_VOID(ensure_copy_texture());
    gl->clear_errors();
    gl->copy_image_sub_data(imported->texture, imported->target, 0, 0, 0, 0, copy_texture, abi::gl_texture_2d, 0, 0, 0,
                            0, static_cast<int>(frame.width), static_cast<int>(frame.height), 1);
    if (const unsigned error = gl->gl_get_error(); error != abi::gl_no_error) {
        log::debug(component, "glCopyImageSubData: GL error {:#x}", error);
        return fail(Errc::unsupported, "OpenGL cannot copy this dmabuf");
    }
    // Mapping for CUDA waits for the copy; it only has to be submitted.
    gl->flush();

    FARLAND_TRY_VOID(
        cu->check(cu->graphics_map_resources(1, &copy_resource, nullptr), "CUDA cannot map the copy texture"));
    abi::CUarray array = nullptr;
    auto copied = cu->check(cu->graphics_sub_resource_get_mapped_array(&array, copy_resource, 0, 0),
                            "CUDA cannot map the copy texture");
    if (copied.has_value()) {
        abi::CUDA_MEMCPY2D transfer{};
        transfer.srcMemoryType = abi::cu_memorytype_array;
        transfer.srcArray = array;
        transfer.dstMemoryType = abi::cu_memorytype_device;
        transfer.dstDevice = staging.pointer;
        transfer.dstPitch = staging.pitch;
        transfer.WidthInBytes = std::size_t{frame.width} * 4U;
        transfer.Height = frame.height;
        copied = cu->check(cu->memcpy_2d(&transfer), "cannot copy the dmabuf picture");
    }
    cu->graphics_unmap_resources(1, &copy_resource, nullptr);
    return copied;
}

Result<void> NvencEncoder::Impl::convert_staging(std::uint32_t width, std::uint32_t height, const Channels& order) const
{
    if (kernel == nullptr) {
        return fail(Errc::unsupported, "the GPU colour conversion is not available");
    }
    abi::CUdeviceptr src = staging.pointer;
    auto src_pitch = static_cast<std::uint32_t>(staging.pitch);
    std::uint32_t src_width = width;
    std::uint32_t src_height = height;
    abi::CUdeviceptr dst_y = nv12.pointer;
    abi::CUdeviceptr dst_uv = nv12.pointer + (nv12.pitch * config.height);
    auto dst_pitch = static_cast<std::uint32_t>(nv12.pitch);
    std::uint32_t blocks_x = config.width / 2U;
    std::uint32_t blocks_y = config.height / 2U;
    std::uint32_t red_shift = order.red_shift;
    std::uint32_t blue_shift = order.blue_shift;
    Coefficients c = coeffs;
    std::array<void*, 22> params{&src,       &src_pitch, &src_width, &src_height, &dst_y,     &dst_uv,
                                 &dst_pitch, &blocks_x,  &blocks_y,  &red_shift,  &blue_shift};
    for (std::size_t i = 0; i < c.m.size(); ++i) {
        params.at(11 + i) = &c.m.at(i);
    }
    params[20] = &c.round;
    params[21] = &c.y_offset;
    constexpr unsigned block_x = 32;
    constexpr unsigned block_y = 8;
    FARLAND_TRY_VOID(
        cu->check(cu->launch_kernel(kernel, (blocks_x + block_x - 1) / block_x, (blocks_y + block_y - 1) / block_y, 1,
                                    block_x, block_y, 1, 0, nullptr, params.data(), nullptr),
                  "cannot start the colour conversion"));
    // NVENC reads the picture outside CUDA's stream order.
    return cu->check(cu->ctx_synchronize(), "the colour conversion failed");
}

Result<codec::Yuv420Frame> NvencEncoder::Impl::read_nv12() const
{
    const std::size_t rows = std::size_t{config.height} * 3U / 2U;
    std::vector<std::byte> host(std::size_t{config.width} * rows);
    abi::CUDA_MEMCPY2D copy{};
    copy.srcMemoryType = abi::cu_memorytype_device;
    copy.srcDevice = nv12.pointer;
    copy.srcPitch = nv12.pitch;
    copy.dstMemoryType = abi::cu_memorytype_host;
    copy.dstHost = host.data();
    copy.dstPitch = config.width;
    copy.WidthInBytes = config.width;
    copy.Height = rows;
    FARLAND_TRY_VOID(cu->check(cu->memcpy_2d(&copy), "cannot read the NV12 picture back"));
    codec::Yuv420Frame frame(config.width, config.height);
    const std::size_t luma = std::size_t{config.width} * config.height;
    std::ranges::copy(std::span(host).first(luma), frame.y().begin());
    const auto uv = std::span(host).subspan(luma);
    const auto u = frame.u();
    const auto v = frame.v();
    for (std::size_t i = 0; i < u.size(); ++i) {
        u[i] = uv[2 * i];
        v[i] = uv[(2 * i) + 1];
    }
    return frame;
}

Result<EncodedFrame> NvencEncoder::Impl::encode_input(void* input, std::uint32_t format, std::uint32_t pitch,
                                                      const FrameOptions& options)
{
    const auto& api = nv->api;
    auto pic = std::make_unique<abi::NV_ENC_PIC_PARAMS>();
    pic->version = abi::pic_params_ver;
    pic->inputWidth = config.width;
    pic->inputHeight = config.height;
    pic->inputPitch = pitch;
    pic->inputBuffer = input;
    pic->outputBitstream = bitstream;
    pic->bufferFmt = format;
    pic->pictureStruct = abi::pic_struct_frame;
    pic->frameIdx = static_cast<std::uint32_t>(frames);
    pic->inputTimeStamp = options.timestamp_us.value_or(frames);
    const bool idr = force_idr || options.force_idr;
    if (idr) {
        pic->encodePicFlags = abi::pic_flag_forceidr | abi::pic_flag_output_spspps;
    }
    if (auto encoded = check(api.nvEncEncodePicture(session, pic.get()), "NVENC cannot encode the picture"); !encoded) {
        force_idr = true;
        return std::unexpected(encoded.error());
    }

    abi::NV_ENC_LOCK_BITSTREAM lock{};
    lock.version = abi::lock_bitstream_ver;
    lock.outputBitstream = bitstream;
    if (auto locked = check(api.nvEncLockBitstream(session, &lock), "NVENC cannot hand out the bitstream"); !locked) {
        force_idr = true;
        return std::unexpected(locked.error());
    }
    EncodedFrame frame;
    const auto bytes = std::span(static_cast<const std::byte*>(lock.bitstreamBufferPtr), lock.bitstreamSizeInBytes);
    frame.bitstream.assign(bytes.begin(), bytes.end());
    frame.idr = lock.pictureType == abi::pic_type_idr;
    frame.qp = static_cast<std::uint8_t>(std::min(lock.frameAvgQP, 51U));
    api.nvEncUnlockBitstream(session, bitstream);
    force_idr = false;
    ++frames;
    return frame;
}

Result<EncodedFrame> NvencEncoder::Impl::encode_registered(void* registered, std::uint32_t pitch,
                                                           const FrameOptions& options)
{
    const auto& api = nv->api;
    abi::NV_ENC_MAP_INPUT_RESOURCE map{};
    map.version = abi::map_input_resource_ver;
    map.registeredResource = registered;
    FARLAND_TRY_VOID(check(api.nvEncMapInputResource(session, &map), "NVENC cannot map the picture"));
    auto frame = encode_input(map.mappedResource, map.mappedBufferFmt, pitch, options);
    api.nvEncUnmapInputResource(session, map.mappedResource);
    return frame;
}

Result<EncodedFrame> NvencEncoder::Impl::encode(const codec::Yuv420View& picture, const FrameOptions& options)
{
    if (session == nullptr) {
        return fail(Errc::io, "NVENC encoder is not configured");
    }
    FARLAND_ASSERT(picture.width == config.width && picture.height == config.height);
    FARLAND_ASSERT(picture.y_stride >= picture.width && picture.uv_stride >= picture.width / 2U);
    FARLAND_ASSERT(picture.y.size() >= picture.y_stride * picture.height);
    FARLAND_ASSERT(picture.u.size() >= picture.uv_stride * (picture.height / 2U));
    FARLAND_ASSERT(picture.v.size() >= picture.uv_stride * (picture.height / 2U));

    const auto& api = nv->api;
    const CudaScope scope(*cu, context);
    if (host_input == nullptr) {
        abi::NV_ENC_CREATE_INPUT_BUFFER create{};
        create.version = abi::create_input_buffer_ver;
        create.width = config.width;
        create.height = config.height;
        create.bufferFmt = abi::buffer_format_iyuv;
        FARLAND_TRY_VOID(check(api.nvEncCreateInputBuffer(session, &create), "NVENC cannot create an input buffer"));
        host_input = create.inputBuffer;
    }
    abi::NV_ENC_LOCK_INPUT_BUFFER lock{};
    lock.version = abi::lock_input_buffer_ver;
    lock.inputBuffer = host_input;
    FARLAND_TRY_VOID(check(api.nvEncLockInputBuffer(session, &lock), "NVENC cannot lock the input buffer"));
    // IYUV: Y at `pitch`, then U and V at half the pitch (the SDK's layout).
    const std::size_t pitch = lock.pitch;
    const std::size_t chroma_pitch = pitch / 2U;
    const std::size_t width = config.width;
    const std::size_t height = config.height;
    const auto buffer = std::span(static_cast<std::byte*>(lock.bufferDataPtr), (pitch * height * 3U) / 2U);
    for (std::size_t row = 0; row < height; ++row) {
        std::ranges::copy(picture.y.subspan(row * picture.y_stride, width), buffer.subspan(row * pitch).begin());
    }
    const auto u_plane = buffer.subspan(pitch * height);
    const auto v_plane = u_plane.subspan(chroma_pitch * (height / 2U));
    for (std::size_t row = 0; row < height / 2U; ++row) {
        std::ranges::copy(picture.u.subspan(row * picture.uv_stride, width / 2U),
                          u_plane.subspan(row * chroma_pitch).begin());
        std::ranges::copy(picture.v.subspan(row * picture.uv_stride, width / 2U),
                          v_plane.subspan(row * chroma_pitch).begin());
    }
    api.nvEncUnlockInputBuffer(session, host_input);
    return encode_input(host_input, abi::buffer_format_iyuv, lock.pitch, options);
}

Result<void> NvencEncoder::Impl::check_frame(const DmabufFrame& frame) const
{
    if (frame.width == 0 || frame.height == 0 || frame.width > config.width || frame.height > config.height) {
        return fail(Errc::invalid_value, "dmabuf size does not fit the encoder's picture");
    }
    if (frame.plane_count == 0 || frame.plane_count > frame.planes.size()) {
        return fail(Errc::invalid_value, "dmabuf plane count must be 1..4");
    }
    for (std::uint32_t i = 0; i < frame.plane_count; ++i) {
        if (frame.planes.at(i).fd < 0) {
            return fail(Errc::invalid_value, "dmabuf plane has no descriptor");
        }
    }
    if (!channels(frame.fourcc).has_value()) {
        return fail(Errc::unsupported, "NVENC takes XRGB, ARGB, XBGR and ABGR dmabufs");
    }
    if (tuning.nvenc_rgb_conversion && frame.fourcc != drm_fourcc::xrgb8888 && frame.fourcc != drm_fourcc::argb8888) {
        return fail(Errc::unsupported, "NVENC's own conversion is only wired up for XRGB and ARGB");
    }
    return {};
}

Result<EncodedFrame> NvencEncoder::Impl::encode_dmabuf(const DmabufFrame& frame, const FrameOptions& options)
{
    if (session == nullptr) {
        return fail(Errc::io, "NVENC encoder is not configured");
    }
    FARLAND_TRY_VOID(check_frame(frame));
    if (!dmabuf_ready()) {
        return fail(Errc::unsupported, "this NVENC encoder cannot import dmabufs");
    }
    const CudaScope scope(*cu, context);
    const GlScope gl_scope(gl.get());
    if (!gl_scope.current()) {
        return fail(Errc::unsupported, "cannot make the OpenGL context current");
    }
    FARLAND_TRY_VOID(ensure_gpu_buffers());
    FARLAND_TRY_VOID(copy_dmabuf(frame));
    if (tuning.nvenc_rgb_conversion) {
        FARLAND_TRY_VOID(cu->check(cu->ctx_synchronize(), "the dmabuf copy failed"));
        return encode_registered(staging_registered, static_cast<std::uint32_t>(staging.pitch), options);
    }
    const auto order = channels(frame.fourcc);
    if (!order.has_value()) {
        return fail(Errc::unsupported, "NVENC takes XRGB, ARGB, XBGR and ABGR dmabufs");
    }
    FARLAND_TRY_VOID(convert_staging(frame.width, frame.height, *order));
    return encode_registered(nv12_registered, static_cast<std::uint32_t>(nv12.pitch), options);
}

NvencEncoder::NvencEncoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

NvencEncoder::~NvencEncoder() = default;

const EncoderConfig& NvencEncoder::config() const noexcept
{
    return impl_->config;
}

Result<void> NvencEncoder::configure(const EncoderConfig& config)
{
    return impl_->configure(config);
}

Result<void> NvencEncoder::set_rate_control(const RateControl& rate)
{
    return impl_->set_rate_control(rate);
}

void NvencEncoder::request_idr() noexcept
{
    impl_->force_idr = true;
}

Result<EncodedFrame> NvencEncoder::encode(const codec::Yuv420View& picture, const FrameOptions& options)
{
    return impl_->encode(picture, options);
}

bool NvencEncoder::accepts_dmabuf() const noexcept
{
    return impl_->dmabuf_ready();
}

Result<EncodedFrame> NvencEncoder::encode_dmabuf(const DmabufFrame& frame, const FrameOptions& options)
{
    return impl_->encode_dmabuf(frame, options);
}

Result<codec::Yuv420Frame> NvencEncoder::convert(const DmabufFrame& frame)
{
    Impl& impl = *impl_;
    if (impl.session == nullptr) {
        return fail(Errc::io, "NVENC encoder is not configured");
    }
    FARLAND_TRY_VOID(impl.check_frame(frame));
    if (impl.gl == nullptr || impl.kernel == nullptr) {
        return fail(Errc::unsupported, "this NVENC encoder cannot import dmabufs");
    }
    const CudaScope scope(*impl.cu, impl.context);
    const GlScope gl_scope(impl.gl.get());
    if (!gl_scope.current()) {
        return fail(Errc::unsupported, "cannot make the OpenGL context current");
    }
    FARLAND_TRY_VOID(impl.ensure_gpu_buffers());
    FARLAND_TRY_VOID(impl.copy_dmabuf(frame));
    const auto order = channels(frame.fourcc);
    if (!order.has_value()) {
        return fail(Errc::unsupported, "NVENC takes XRGB, ARGB, XBGR and ABGR dmabufs");
    }
    FARLAND_TRY_VOID(impl.convert_staging(frame.width, frame.height, *order));
    return impl.read_nv12();
}

Result<codec::Yuv420Frame> NvencEncoder::convert(const codec::ImageView& image)
{
    Impl& impl = *impl_;
    if (impl.session == nullptr) {
        return fail(Errc::io, "NVENC encoder is not configured");
    }
    if (image.width == 0 || image.height == 0 || image.width > impl.config.width || image.height > impl.config.height) {
        return fail(Errc::invalid_value, "image size does not fit the encoder's picture");
    }
    FARLAND_ASSERT(image.stride >= std::size_t{image.width} * 4U);
    FARLAND_ASSERT(image.data.size() >= ((image.height - 1U) * image.stride) + (std::size_t{image.width} * 4U));
    const CudaScope scope(*impl.cu, impl.context);
    FARLAND_TRY_VOID(impl.ensure_gpu_buffers());
    abi::CUDA_MEMCPY2D copy{};
    copy.srcMemoryType = abi::cu_memorytype_host;
    copy.srcHost = image.data.data();
    copy.srcPitch = image.stride;
    copy.dstMemoryType = abi::cu_memorytype_device;
    copy.dstDevice = impl.staging.pointer;
    copy.dstPitch = impl.staging.pitch;
    copy.WidthInBytes = std::size_t{image.width} * 4U;
    copy.Height = image.height;
    FARLAND_TRY_VOID(impl.cu->check(impl.cu->memcpy_2d(&copy), "cannot upload the image"));
    FARLAND_TRY_VOID(impl.convert_staging(image.width, image.height, Channels{}));
    return impl.read_nv12();
}

const DeviceInfo& NvencEncoder::device() const noexcept
{
    return impl_->info;
}

void NvencEncoder::forget_dmabufs() noexcept
{
    const CudaScope scope(*impl_->cu, impl_->context);
    impl_->forget_imports();
}

Result<DeviceInfo> probe(const std::string& render_node)
{
    FARLAND_TRY(auto impl, NvencEncoder::Impl::open(render_node));
    // The size limits come from an encode session.
    impl->config.width = 256;
    impl->config.height = 256;
    const CudaScope scope(*impl->cu, impl->context);
    auto opened = impl->open_session();
    impl->close_session();
    FARLAND_TRY_VOID(opened);
    return impl->info;
}

std::string describe(const DeviceInfo& info)
{
    return std::format("{} ({}{}{}), CUDA driver {}.{}, NVENC API {}.{}, H.264 {}x{} to {}x{}, dmabuf input {}",
                       info.name, info.pci_bus_id, info.render_node.empty() ? "" : ", ", info.render_node,
                       info.cuda_driver_version / 1000, (info.cuda_driver_version % 1000) / 10, info.nvenc_api_major,
                       info.nvenc_api_minor, info.min_width, info.min_height, info.max_width, info.max_height,
                       info.dmabuf ? "yes" : "no");
}

Result<std::unique_ptr<H264Encoder>> create(const EncoderConfig& config, const BackendOptions& options,
                                            const Tuning& tuning)
{
    FARLAND_TRY_VOID(validate(config));
    if (tuning.preset < 1 || tuning.preset > abi::preset_guids.size()) {
        return fail(Errc::invalid_value, "NVENC preset must be 1..7");
    }
    FARLAND_TRY(auto impl, NvencEncoder::Impl::open(options.render_node));
    impl->tuning = tuning;
    impl->color = options.color;
    impl->coeffs = coefficients(options.color);
    impl->info.dmabuf = impl->dmabuf_ready();
    auto encoder = std::make_unique<NvencEncoder>(std::move(impl));
    FARLAND_TRY_VOID(encoder->configure(config));
    log::debug(component, "{}", describe(encoder->device()));
    return encoder;
}

}  // namespace farland::video::nvenc
