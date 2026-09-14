// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/codec/image.hpp>
#include <farland/codec/yuv.hpp>
#include <farland/video/dmabuf_frame.hpp>
#include <farland/video/h264_encoder.hpp>

#include <cstdint>
#include <memory>
#include <string>

/// NVENC H.264 backend (docs/PLAN.md §3.4, docs/ROADMAP.md M5): hardware
/// encoding on NVIDIA GPUs. Built with -Dnvenc (Linux); nothing is needed to
/// build it, because libnvidia-encode.so.1, libcuda.so.1 and libEGL.so.1 come
/// with the driver and are loaded at runtime (nvenc_abi.hpp: NVENC API 12.0,
/// driver 520 or newer).
///
/// Pictures come in two ways:
/// - encode(): an I420 picture from the CPU, copied into an NVENC input
///   buffer (IYUV);
/// - encode_dmabuf(): a captured frame in GPU memory, converted to NV12 on
///   the GPU without passing through the CPU.
///
/// The dmabuf path. CUDA cannot import dmabufs: cuImportExternalMemory takes
/// only opaque (Vulkan/CUDA) descriptors, and cuGraphicsEGLRegisterImage
/// rejects EGLImages made from dmabufs (driver 595: CUDA_ERROR_INVALID_VALUE,
/// as does cuGraphicsGLRegisterImage on a texture bound to such an image).
/// What works is OpenGL in between:
///
///     dmabuf --EGL_EXT_image_dma_buf_import--> EGLImage --glEGLImageTargetTexture2DOES--> texture
///       --glCopyImageSubData--> our RGBA8 texture --cuGraphicsGLRegisterImage--> CUDA array
///       --copy + conversion kernel--> NV12 buffer registered with NVENC
///
/// on an EGL display of the NVIDIA device (EGL_EXT_platform_device, matched
/// to the CUDA device with EGL_NV_device_cuda) with a surfaceless desktop GL
/// context. Tiled buffers bind as GL_TEXTURE_2D, LINEAR ones only as
/// GL_TEXTURE_EXTERNAL_OES; glCopyImageSubData takes both. The GPU copies
/// the picture twice (about 0.1 ms each at 4K); the CPU touches no pixels.
///
/// Colour conversion runs in a small CUDA kernel, shipped as PTX and
/// compiled by the driver, with the integer arithmetic of
/// codec::bgrx_to_yuv420: for the default ColorSpace (full-range BT.709) GPU
/// and CPU pictures are bit-identical, so a client sees the same colours
/// whichever path a frame took. Other colour spaces round their
/// coefficients to 1/256 the same way. The part of the coded picture outside
/// the frame is black. NVENC could convert RGB input itself
/// (NV_ENC_BUFFER_FORMAT_ARGB): with driver 595 it follows the matrix and
/// range of the VUI, but rounds to nearest where FreeRDP rounds down, and
/// its chroma filter is not specified (Tuning::nvenc_rgb_conversion
/// measures it). FFmpeg's h264_nvenc signals limited-range BT.601 for RGB
/// input, which is why NVENC seems fixed to that.
///
/// The stream is what h264_encoder.hpp promises: P1-P7 presets with the
/// (ultra-)low-latency tuning, no B-frames and no lookahead, so one access
/// unit per picture; IDRs at the start, on request and every keyint frames;
/// NVENC's access unit delimiters; SPS and PPS before every IDR; a VUI with
/// the colour space, sRGB transfer and a bitstream restriction.
///
/// Rate control: constant_quality without a cap is constant QP; with a cap
/// it is VBR with target quality `quality` and the cap as maximum bitrate;
/// bitrate mode without a cap (or a cap equal to the bitrate) is CBR, with a
/// higher cap VBR. Every change goes to nvEncReconfigureEncoder: a new
/// bitrate or target quality applies to the next picture without an IDR; a
/// new mode, or a new QP in constant-QP mode, resets the encoder and makes
/// the next picture an IDR (NVENC restarts on a new constant QP by itself).
/// Rate control budgets by `fps`.
namespace farland::video::nvenc {

/// The NVIDIA GPU an encoder runs on.
struct DeviceInfo {
    int cuda_device = 0;
    std::string name;
    /// PCI address, e.g. "0000:01:00.0".
    std::string pci_bus_id;
    /// Its DRM render node, if one was found.
    std::string render_node;
    /// cuDriverGetVersion, e.g. 13020 for CUDA 13.2.
    int cuda_driver_version = 0;
    /// Newest NVENC API the driver implements (farland uses 12.0).
    std::uint32_t nvenc_api_major = 0;
    std::uint32_t nvenc_api_minor = 0;
    /// Smallest and largest H.264 picture NVENC encodes (at most 4096x4096 up to Ada).
    std::uint32_t min_width = 0;
    std::uint32_t min_height = 0;
    std::uint32_t max_width = 0;
    std::uint32_t max_height = 0;
    /// The EGL/OpenGL route for encode_dmabuf() works on this device.
    bool dmabuf = false;
};

/// Opens the GPU of `render_node` (empty: CUDA device 0) and reports what it
/// offers; Errc::unsupported without an NVIDIA driver or GPU.
[[nodiscard]] Result<DeviceInfo> probe(const std::string& render_node = {});
/// One line for logs.
[[nodiscard]] std::string describe(const DeviceInfo& info);

/// NVENC settings beyond EncoderConfig, for benchmarks and experiments.
struct Tuning {
    /// NVENC preset P1 (fastest) to P7 (best).
    std::uint8_t preset = 4;
    /// NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, else LOW_LATENCY.
    bool ultra_low_latency = true;
    /// Hand BGRX dmabufs to NVENC as ARGB and let it convert (with the VUI's
    /// matrix and range, see above) instead of running farland's kernel.
    /// Only for measuring what NVENC does.
    bool nvenc_rgb_conversion = false;

    friend bool operator==(const Tuning&, const Tuning&) = default;
};

class NvencEncoder final : public H264Encoder {
public:
    struct Impl;
    explicit NvencEncoder(std::unique_ptr<Impl> impl);
    NvencEncoder(const NvencEncoder&) = delete;
    NvencEncoder& operator=(const NvencEncoder&) = delete;
    NvencEncoder(NvencEncoder&&) = delete;
    NvencEncoder& operator=(NvencEncoder&&) = delete;
    ~NvencEncoder() override;

    [[nodiscard]] Backend backend() const noexcept override { return Backend::nvenc; }
    [[nodiscard]] const EncoderConfig& config() const noexcept override;
    [[nodiscard]] Result<void> configure(const EncoderConfig& config) override;
    [[nodiscard]] Result<void> set_rate_control(const RateControl& rate) override;
    void request_idr() noexcept override;
    [[nodiscard]] Result<EncodedFrame> encode(const codec::Yuv420View& picture, const FrameOptions& options) override;
    [[nodiscard]] bool accepts_dmabuf() const noexcept override;
    [[nodiscard]] Result<EncodedFrame> encode_dmabuf(const DmabufFrame& frame, const FrameOptions& options) override;

    /// Only the GPU colour conversion of encode_dmabuf(), read back as I420
    /// of the configured size. Slow; for tests and diagnostics.
    [[nodiscard]] Result<codec::Yuv420Frame> convert(const DmabufFrame& frame);
    /// The same conversion of B, G, R, X pixels uploaded from the CPU; works
    /// without the EGL/OpenGL route.
    [[nodiscard]] Result<codec::Yuv420Frame> convert(const codec::ImageView& image);

    [[nodiscard]] const DeviceInfo& device() const noexcept;

    /// Releases the imported dmabufs. Imports are cached (by device and inode
    /// of the descriptor, a handful at a time) because a capture cycles
    /// through the same few buffers; a capture that renegotiates its buffers
    /// should call this, so the old ones are freed.
    void forget_dmabufs() noexcept override;

private:
    std::unique_ptr<Impl> impl_;
};

/// Opens the GPU (BackendOptions::render_node) and configures an encoder.
/// Errc::unsupported without the NVIDIA libraries, without an NVIDIA GPU at
/// that node, if all NVENC sessions are taken, or for pictures smaller or
/// larger than NVENC encodes.
[[nodiscard]] Result<std::unique_ptr<H264Encoder>>
create(const EncoderConfig& config, const BackendOptions& options = {}, const Tuning& tuning = {});

}  // namespace farland::video::nvenc
