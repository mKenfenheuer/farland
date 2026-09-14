// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "nvenc_test_source.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#if defined(FARLAND_TEST_HAVE_GBM)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fcntl.h>
#include <gbm.h>
#include <span>
#include <string_view>
#include <unistd.h>
#include <vector>
#endif

namespace farland::test {

namespace {
std::atomic<bool> wrote_through_gl{false};
}  // namespace

bool linear_writes_reliable() noexcept
{
    return !wrote_through_gl.load();
}

#if defined(FARLAND_TEST_HAVE_GBM)

namespace {

constexpr unsigned gl_texture_2d = 0x0DE1;
constexpr unsigned gl_bgra = 0x80E1;
constexpr unsigned gl_unsigned_byte = 0x1401;
constexpr unsigned gl_unpack_row_length = 0x0CF2;

using GenTextures = void (*)(int, unsigned*);
using DeleteTextures = void (*)(int, const unsigned*);
using BindTexture = void (*)(unsigned, unsigned);
using TexSubImage2D = void (*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
using PixelStorei = void (*)(unsigned, int);
using Finish = void (*)();
using GetError = unsigned (*)();
using ImageTargetTexture2D = void (*)(unsigned, void*);

template <class Fn>
Fn proc(const char* name)
{
    return reinterpret_cast<Fn>(eglGetProcAddress(name));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

}  // namespace

struct GbmDmabuf::State {
    int node = -1;
    gbm_device* device = nullptr;
    gbm_bo* bo = nullptr;
    video::DmabufFrame frame;
    bool linear = false;
    // Tiled buffers: written through OpenGL.
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    unsigned texture = 0;

    ~State()
    {
        if (context != EGL_NO_CONTEXT) {
            if (texture != 0 && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) == EGL_TRUE) {
                proc<DeleteTextures>("glDeleteTextures")(1, &texture);
            }
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (image != EGL_NO_IMAGE_KHR) {
                proc<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR")(display, image);
            }
            eglDestroyContext(display, context);
        }
        for (std::uint32_t i = 0; i < frame.plane_count; ++i) {
            if (frame.planes.at(i).fd >= 0) {
                close(frame.planes.at(i).fd);
            }
        }
        if (bo != nullptr) {
            gbm_bo_destroy(bo);
        }
        if (device != nullptr) {
            gbm_device_destroy(device);
        }
        if (node >= 0) {
            close(node);
        }
    }

    /// An EGL context on the NVIDIA device of `render_node`, with the buffer as texture.
    bool open_gl(const std::string& render_node)
    {
        const auto query_devices = proc<PFNEGLQUERYDEVICESEXTPROC>("eglQueryDevicesEXT");
        const auto query_string = proc<PFNEGLQUERYDEVICESTRINGEXTPROC>("eglQueryDeviceStringEXT");
        const auto platform_display = proc<PFNEGLGETPLATFORMDISPLAYEXTPROC>("eglGetPlatformDisplayEXT");
        const auto create_image = proc<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR");
        if (query_devices == nullptr || query_string == nullptr || platform_display == nullptr ||
            create_image == nullptr) {
            return false;
        }
        std::array<EGLDeviceEXT, 16> devices{};
        EGLint count = 0;
        query_devices(static_cast<EGLint>(devices.size()), devices.data(), &count);
        for (const EGLDeviceEXT candidate : std::span(devices).first(static_cast<std::size_t>(std::max(count, 0)))) {
            const char* extensions = query_string(candidate, EGL_EXTENSIONS);
            const char* node_name = query_string(candidate, EGL_DRM_RENDER_NODE_FILE_EXT);
            if (extensions != nullptr &&
                std::string_view(extensions).find("EGL_NV_device_cuda") != std::string_view::npos &&
                (node_name == nullptr || render_node == node_name)) {
                display = platform_display(EGL_PLATFORM_DEVICE_EXT, candidate, nullptr);
                break;
            }
        }
        EGLint major = 0;
        EGLint minor = 0;
        if (display == EGL_NO_DISPLAY || eglInitialize(display, &major, &minor) != EGL_TRUE ||
            eglBindAPI(EGL_OPENGL_API) != EGL_TRUE) {
            return false;
        }
        const std::array<EGLint, 1> none{EGL_NONE};
        context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, none.data());
        if (context == EGL_NO_CONTEXT || eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context) != EGL_TRUE) {
            return false;
        }
        const auto& plane = frame.planes[0];
        const std::array<EGLint, 17> attributes{
            EGL_WIDTH,
            static_cast<EGLint>(frame.width),
            EGL_HEIGHT,
            static_cast<EGLint>(frame.height),
            EGL_LINUX_DRM_FOURCC_EXT,
            static_cast<EGLint>(frame.fourcc),
            EGL_DMA_BUF_PLANE0_FD_EXT,
            plane.fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,
            static_cast<EGLint>(plane.offset),
            EGL_DMA_BUF_PLANE0_PITCH_EXT,
            static_cast<EGLint>(plane.pitch),
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
            static_cast<EGLint>(frame.modifier & 0xFFFF'FFFFU),
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
            static_cast<EGLint>(frame.modifier >> 32U),
            EGL_NONE,
        };
        image = create_image(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes.data());
        if (image == EGL_NO_IMAGE_KHR) {
            return false;
        }
        proc<GenTextures>("glGenTextures")(1, &texture);
        proc<BindTexture>("glBindTexture")(gl_texture_2d, texture);
        proc<ImageTargetTexture2D>("glEGLImageTargetTexture2DOES")(gl_texture_2d, image);
        const bool ok = proc<GetError>("glGetError")() == 0;
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return ok;
    }
};

GbmDmabuf::GbmDmabuf(std::unique_ptr<State> state) : state_(std::move(state)) {}

GbmDmabuf::~GbmDmabuf() = default;

const video::DmabufFrame& GbmDmabuf::frame() const noexcept
{
    return state_->frame;
}

bool GbmDmabuf::write(const codec::ImageView& image)
{
    State& s = *state_;
    if (image.width != s.frame.width || image.height != s.frame.height) {
        return false;
    }
    const std::size_t row = std::size_t{image.width} * 4U;
    if (s.linear) {
        std::uint32_t stride = 0;
        void* map_data = nullptr;
        void* pixels = gbm_bo_map(s.bo, 0, 0, image.width, image.height, GBM_BO_TRANSFER_WRITE, &stride, &map_data);
        if (pixels == nullptr) {
            return false;
        }
        const std::span<std::byte> dst(static_cast<std::byte*>(pixels), std::size_t{stride} * image.height);
        for (std::uint32_t y = 0; y < image.height; ++y) {
            std::ranges::copy(image.data.subspan(y * image.stride, row), dst.subspan(y * std::size_t{stride}).begin());
        }
        gbm_bo_unmap(s.bo, map_data);
        return true;
    }
    if (eglMakeCurrent(s.display, EGL_NO_SURFACE, EGL_NO_SURFACE, s.context) != EGL_TRUE) {
        return false;
    }
    wrote_through_gl = true;
    proc<GetError>("glGetError")();
    proc<BindTexture>("glBindTexture")(gl_texture_2d, s.texture);
    proc<PixelStorei>("glPixelStorei")(gl_unpack_row_length, static_cast<int>(image.stride / 4U));
    proc<TexSubImage2D>("glTexSubImage2D")(gl_texture_2d, 0, 0, 0, static_cast<int>(image.width),
                                           static_cast<int>(image.height), gl_bgra, gl_unsigned_byte,
                                           image.data.data());
    proc<PixelStorei>("glPixelStorei")(gl_unpack_row_length, 0);
    proc<Finish>("glFinish")();
    const bool ok = proc<GetError>("glGetError")() == 0;
    eglMakeCurrent(s.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return ok;
}

std::unique_ptr<GbmDmabuf> make_gbm_dmabuf(const std::string& render_node, std::uint32_t width, std::uint32_t height,
                                           bool linear)
{
    auto state = std::make_unique<GbmDmabuf::State>();
    state->linear = linear;
    state->node = open(render_node.c_str(), O_RDWR | O_CLOEXEC);
    if (state->node < 0 || (state->device = gbm_create_device(state->node)) == nullptr) {
        std::cerr << "GBM: cannot open " << render_node << '\n';
        return nullptr;
    }
    state->bo = gbm_bo_create(state->device, width, height, GBM_FORMAT_XRGB8888,
                              linear ? GBM_BO_USE_LINEAR : GBM_BO_USE_RENDERING);
    if (state->bo == nullptr) {
        std::cerr << "GBM: cannot allocate a " << (linear ? "LINEAR" : "tiled") << " buffer\n";
        return nullptr;
    }
    auto& frame = state->frame;
    frame.fourcc = video::drm_fourcc::xrgb8888;
    frame.modifier = gbm_bo_get_modifier(state->bo);
    frame.width = width;
    frame.height = height;
    const int planes = gbm_bo_get_plane_count(state->bo);
    frame.plane_count = static_cast<std::uint32_t>(std::clamp(planes, 1, 4));
    for (std::uint32_t i = 0; i < frame.plane_count; ++i) {
        const auto plane = static_cast<int>(i);
        frame.planes.at(i) = {.fd = gbm_bo_get_fd_for_plane(state->bo, plane),
                              .offset = gbm_bo_get_offset(state->bo, plane),
                              .pitch = gbm_bo_get_stride_for_plane(state->bo, plane)};
    }
    if (!linear && !state->open_gl(render_node)) {
        std::cerr << "EGL/OpenGL: cannot write the tiled buffer\n";
        return nullptr;
    }
    return std::make_unique<GbmDmabuf>(std::move(state));
}

#else

struct GbmDmabuf::State {};

GbmDmabuf::GbmDmabuf(std::unique_ptr<State> state) : state_(std::move(state)) {}

GbmDmabuf::~GbmDmabuf() = default;

bool GbmDmabuf::write(const codec::ImageView& /*image*/)
{
    return false;
}

const video::DmabufFrame& GbmDmabuf::frame() const noexcept
{
    static const video::DmabufFrame none{};
    return none;
}

std::unique_ptr<GbmDmabuf> make_gbm_dmabuf(const std::string& /*render_node*/, std::uint32_t /*width*/,
                                           std::uint32_t /*height*/, bool /*linear*/)
{
    std::cerr << "built without GBM and EGL (libgbm-dev, libegl-dev)\n";
    return nullptr;
}

#endif

}  // namespace farland::test
