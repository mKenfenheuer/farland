// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/channels/rdpgfx_server.hpp>
#include <farland/codec/image.hpp>
#include <farland/codec/progressive.hpp>
#include <farland/codec/yuv.hpp>
#include <farland/codec/zgfx.hpp>
#include <farland/server/dynamic_channels.hpp>
#include <farland/video/h264_encoder.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/// The Graphics Pipeline output of one session ([MS-RDPEGFX], docs/PLAN.md
/// §3.4) over a dynamic virtual channel. It opens the channel, answers the
/// capability exchange through GfxServer, lays out one surface covering the
/// desktop, and sends each frame as the 64x64 tiles that changed. Every
/// RDPGFX PDU goes out ZGFX-compressed ([MS-RDPEGFX] 2.2.5), several PDUs to
/// a message.
///
/// Frames are encoded with RemoteFX Progressive by default, with planar
/// (lossless) on request or for clients that cannot take Progressive (thin
/// clients), or as H.264 (AVC420) when an encoder is available and the
/// client allows it.
namespace farland::server {

enum class TileCodec : std::uint8_t {
    progressive,  ///< RFX Progressive, lossy, much smaller; planar for thin clients
    planar,       ///< RDP 6.0 planar, lossless
    avc420,       ///< H.264 4:2:0 of the whole surface; Progressive when unavailable
};

/// Creates the H.264 encoder for a surface (the configuration has the coded
/// size filled in).
using H264Factory = std::function<Result<std::unique_ptr<video::H264Encoder>>(const video::EncoderConfig&)>;

namespace pipeline_event {
/// The surface is laid out and frames may be sent. Comes again after the
/// client restarted the pipeline (a second Caps Advertise).
struct Ready {
    channels::rdpgfx::Negotiated negotiated;
};
/// The client acknowledged a frame; feed it to the FrameScheduler.
struct FrameAcked {
    std::uint32_t frame_id = 0;
    std::uint32_t queue_depth = 0;
    bool known = false;
};
/// The pipeline is gone: the client refused or closed the channel, or broke
/// the protocol. The session falls back to legacy output.
struct Closed {
    std::string reason;
};
}  // namespace pipeline_event

using PipelineEvent = std::variant<pipeline_event::Ready, pipeline_event::FrameAcked, pipeline_event::Closed>;

class GraphicsPipeline {
public:
    static constexpr std::string_view channel_name = "Microsoft::Windows::RDS::Graphics";
    static constexpr std::uint32_t tile_size = 64;
    /// Most bytes of RDPGFX PDUs compressed into one ZGFX message. mstsc
    /// failed on a message of about 60 KB (docs/PLAN.md §4.1).
    static constexpr std::size_t max_batch_size = std::size_t{16} * 1024;

    /// Opens the graphics channel on `channels` (whose capabilities must be
    /// ready) for a desktop of `width` x `height`. `channels` must outlive
    /// the pipeline.
    GraphicsPipeline(DynamicChannels& channels, std::uint16_t width, std::uint16_t height,
                     channels::rdpgfx::GfxServerConfig config = {}, TileCodec codec = TileCodec::progressive,
                     H264Factory make_h264 = {});

    /// Handles `event` if it concerns the graphics channel. False otherwise.
    bool handle(const channels::DvcEvent& event);
    [[nodiscard]] std::optional<PipelineEvent> poll_event();

    [[nodiscard]] bool ready() const noexcept { return surface_.has_value() && !closed_; }
    [[nodiscard]] bool closed() const noexcept { return closed_; }
    /// The codec frames use once ready (the requested one, or planar when
    /// the client cannot take it).
    [[nodiscard]] TileCodec codec() const noexcept
    {
        if (h264_) {
            return TileCodec::avc420;
        }
        return progressive_ ? TileCodec::progressive : TileCodec::planar;
    }

    /// Sends the tiles of `frame` that changed (or were invalidated) as one
    /// frame and returns its ID; nullopt when nothing changed. Only when
    /// ready(); `frame` has the desktop's size.
    [[nodiscard]] std::optional<std::uint32_t> send_frame(const codec::ImageView& frame, std::uint32_t timestamp = 0);
    /// The next frame sends every tile.
    void invalidate_all();

private:
    void drain_gfx();
    void layout();
    void flush();
    void close(std::string reason);
    [[nodiscard]] bool tile_changed(const codec::ImageView& frame, std::uint32_t tx, std::uint32_t ty) const;
    /// One AVC420 WireToSurface1 for the whole surface, listing the changed
    /// tiles as regions ([MS-RDPEGFX] 2.2.4.4).
    [[nodiscard]] std::optional<std::uint32_t>
    send_h264_frame(const codec::ImageView& frame, std::uint16_t surface,
                    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& tiles, std::uint32_t timestamp);
    void send_planar_tile(const codec::ImageView& frame, std::uint16_t surface, std::uint32_t tx, std::uint32_t ty);
    /// Remembers the tile's pixels as sent and clears its dirty flag.
    void remember_tile(const codec::ImageView& frame, std::uint32_t tx, std::uint32_t ty);
    /// The tile's rectangle, clipped to the desktop.
    [[nodiscard]] codec::progressive::Rect tile_rect(std::uint32_t tx, std::uint32_t ty) const noexcept;

    /// The one Progressive codec context of the surface.
    static constexpr std::uint32_t progressive_context = 1;

    DynamicChannels* channels_;  ///< never null
    std::uint32_t channel_id_ = 0;
    channels::rdpgfx::GfxServer gfx_;
    codec::ZgfxCompressor zgfx_;
    std::uint16_t width_;
    std::uint16_t height_;
    std::uint32_t tiles_x_;
    std::uint32_t tiles_y_;
    TileCodec requested_codec_;
    std::optional<std::uint16_t> surface_;
    std::optional<codec::progressive::Encoder> progressive_;
    H264Factory make_h264_;
    std::unique_ptr<video::H264Encoder> h264_;
    std::optional<codec::Yuv420Frame> yuv_;
    std::vector<std::byte> previous_;  ///< Last frame sent, tightly packed BGRX
    std::vector<bool> dirty_;
    std::deque<PipelineEvent> events_;
    bool closed_ = false;
};

}  // namespace farland::server
