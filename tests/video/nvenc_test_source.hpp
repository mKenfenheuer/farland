// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/codec/image.hpp>
#include <farland/video/dmabuf_frame.hpp>

#include <cstdint>
#include <memory>
#include <string>

/// BGRX dmabufs for the NVENC tests and benchmark, standing in for a
/// compositor's buffers: GBM buffer objects on the NVIDIA render node, in
/// the driver's tiled layout or LINEAR. NVIDIA's GBM maps only LINEAR buffers
/// for the CPU, so tiled ones are filled through their own EGL/OpenGL context
/// (an EGLImage of the buffer as texture, glTexSubImage2D). NVIDIA takes
/// LINEAR buffers neither as texture nor as renderbuffer, so those are
/// written through gbm_bo_map; with driver 595 such CPU writes reach the GPU
/// only partly once the process has written another buffer through OpenGL,
/// so create and use LINEAR buffers first (linear_writes_reliable()).
namespace farland::test {

class GbmDmabuf {
public:
    struct State;
    explicit GbmDmabuf(std::unique_ptr<State> state);
    GbmDmabuf(const GbmDmabuf&) = delete;
    GbmDmabuf& operator=(const GbmDmabuf&) = delete;
    GbmDmabuf(GbmDmabuf&&) = delete;
    GbmDmabuf& operator=(GbmDmabuf&&) = delete;
    ~GbmDmabuf();

    /// Copies `image` (the buffer's size, BGRX) into the buffer.
    [[nodiscard]] bool write(const codec::ImageView& image);
    [[nodiscard]] const video::DmabufFrame& frame() const noexcept;

private:
    std::unique_ptr<State> state_;
};

/// False once this process has written a tiled buffer through OpenGL; from
/// then on, writes to LINEAR buffers do not fully reach the GPU.
[[nodiscard]] bool linear_writes_reliable() noexcept;

/// An XRGB8888 buffer object on `render_node`, LINEAR or in the layout the
/// driver picks. nullptr without GBM and EGL at build time
/// (FARLAND_TEST_HAVE_GBM), without access to the node, or if the driver
/// refuses; the reason goes to stderr.
[[nodiscard]] std::unique_ptr<GbmDmabuf> make_gbm_dmabuf(const std::string& render_node, std::uint32_t width,
                                                         std::uint32_t height, bool linear);

}  // namespace farland::test
