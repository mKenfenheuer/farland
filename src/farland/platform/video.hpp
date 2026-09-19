// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

/// The camera half of a platform backend (docs/PLAN.md §3.3): a local camera
/// fed by the client's, so that a video call inside the session sees the
/// person in front of the client. It runs a thread of its own; the session
/// thread writes whole frames to it.
namespace farland::platform {

/// Raw frame layouts a camera can publish. These are the uncompressed formats
/// of [MS-RDPECAM] 2.2.3.10.1, which is what the client sends after its own
/// conversion, so no decoding happens anywhere in farland.
enum class VideoFormat : std::uint8_t {
    yuy2,   ///< packed 4:2:2, two bytes per pixel
    nv12,   ///< planar 4:2:0, a luma plane then interleaved chroma
    i420,   ///< planar 4:2:0, three planes
    rgb24,  ///< packed BGR in memory order, three bytes per pixel
    rgb32,  ///< packed BGRX in memory order, four bytes per pixel
};

struct VideoMode {
    VideoFormat format = VideoFormat::nv12;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// Nominal frames per second, for what the consumer is told to expect.
    std::uint32_t fps = 30;
    /// The rows arrive bottom-up, as Windows RGB frames often do.
    bool bottom_up = false;

    /// Bytes one frame takes.
    [[nodiscard]] constexpr std::size_t frame_size() const noexcept
    {
        const std::size_t pixels = std::size_t{width} * height;
        switch (format) {
        case VideoFormat::yuy2:
            return pixels * 2;
        case VideoFormat::nv12:
        case VideoFormat::i420:
            return pixels + (2 * (((std::size_t{width} + 1) / 2) * ((std::size_t{height} + 1) / 2)));
        case VideoFormat::rgb24:
            return pixels * 3;
        case VideoFormat::rgb32:
            return pixels * 4;
        }
        return 0;
    }

    friend bool operator==(const VideoMode&, const VideoMode&) = default;
};

/// A camera that desktop applications capture from.
class VideoSink {
public:
    VideoSink() = default;
    VideoSink(const VideoSink&) = delete;
    VideoSink& operator=(const VideoSink&) = delete;
    VideoSink(VideoSink&&) = delete;
    VideoSink& operator=(VideoSink&&) = delete;
    virtual ~VideoSink() = default;

    /// Offers one whole frame in the mode the camera was created with. A
    /// frame that arrives while the last one is still unread replaces it: a
    /// camera consumer wants the newest picture, never a backlog.
    virtual void write(std::span<const std::byte> frame) = 0;
    [[nodiscard]] virtual bool closed() const = 0;
    /// Why it closed; empty while open.
    [[nodiscard]] virtual std::string error() const = 0;
};

}  // namespace farland::platform
