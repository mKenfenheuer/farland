// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/codec/image.hpp>
#include <farland/video/dmabuf_frame.hpp>

#include <cstdint>
#include <memory>
#include <string>

/// BGRX dmabufs for the VA-API tests and benchmark, standing in for a
/// compositor's buffers: allocated on their own device handle, filled from
/// the CPU, and handed to the encoder as DmabufFrame.
namespace farland::test {

class DmabufSource {
public:
    DmabufSource() = default;
    DmabufSource(const DmabufSource&) = delete;
    DmabufSource& operator=(const DmabufSource&) = delete;
    DmabufSource(DmabufSource&&) = delete;
    DmabufSource& operator=(DmabufSource&&) = delete;
    virtual ~DmabufSource() = default;

    /// Copies `image` (the buffer's size, BGRX) into the buffer.
    [[nodiscard]] virtual bool write(const codec::ImageView& image) = 0;
    [[nodiscard]] virtual const video::DmabufFrame& frame() const noexcept = 0;
};

/// A VA surface (VA_RT_FORMAT_RGB32, BGRX) exported with
/// vaExportSurfaceHandle: the driver's own layout and modifier. nullptr if
/// the device cannot do it.
[[nodiscard]] std::unique_ptr<DmabufSource> make_va_source(const std::string& render_node, std::uint32_t width,
                                                           std::uint32_t height);

/// A GBM buffer object, XRGB8888, linear or with the layout the driver picks.
/// nullptr without GBM (FARLAND_TEST_HAVE_GBM) or if allocation fails.
[[nodiscard]] std::unique_ptr<DmabufSource> make_gbm_source(const std::string& render_node, std::uint32_t width,
                                                            std::uint32_t height, bool linear);

}  // namespace farland::test
