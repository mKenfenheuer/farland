// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/channels/rdpgfx_server.hpp>
#include <farland/codec/clear.hpp>
#include <farland/codec/image.hpp>
#include <farland/codec/progressive.hpp>
#include <farland/codec/yuv.hpp>
#include <farland/codec/zgfx.hpp>
#include <farland/server/dynamic_channels.hpp>
#include <farland/server/scroll_detector.hpp>
#include <farland/video/avc444_encoder.hpp>
#include <farland/video/dmabuf_frame.hpp>
#include <farland/video/h264_encoder.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
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
/// clients), or as H.264 when an encoder is available and the client allows
/// it: AVC420, or AVC444 for full chroma.
namespace farland::server {

enum class TileCodec : std::uint8_t {
    progressive,  ///< RFX Progressive, lossy, much smaller; planar for thin clients
    planar,       ///< RDP 6.0 planar, lossless
    avc420,       ///< H.264 4:2:0 of the whole surface; Progressive when unavailable
    /// H.264 4:4:4 of the whole surface: AVC444v2 where it is safe, else
    /// AVC444 (v1); AVC420, then Progressive, when unavailable
    avc444,
};

/// Creates the H.264 encoder for a surface (the configuration has the coded
/// size filled in).
using H264Factory = std::function<Result<std::unique_ptr<video::H264Encoder>>(const video::EncoderConfig&)>;

/// How a Progressive surface is encoded (docs/PLAN.md §3.4).
struct PipelineOptions {
    /// Tiles with few colours (text, UI) go through ClearCodec, which keeps
    /// them sharp in far fewer bytes than Progressive.
    bool clearcodec = true;
    /// Progressive tiles go out coarse first and are refined with
    /// TILE_UPGRADE passes in the bytes a frame leaves over.
    bool refine = true;
    /// Most bytes of ClearCodec, Progressive and refinement streams in a
    /// frame before refinement stops for that frame.
    std::size_t upgrade_budget = std::size_t{16} * 1024;
    /// A tile with at most this many colours counts as text or UI (at most 64).
    std::uint32_t max_clear_colours = 48;
};

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
                     H264Factory make_h264 = {}, PipelineOptions options = {});

    /// Handles `event` if it concerns the graphics channel. False otherwise.
    bool handle(const channels::DvcEvent& event);
    [[nodiscard]] std::optional<PipelineEvent> poll_event();

    [[nodiscard]] bool ready() const noexcept { return surface_.has_value() && !closed_; }
    [[nodiscard]] bool closed() const noexcept { return closed_; }
    /// The codec frames use once ready (the requested one, or a fallback
    /// when the client or the encoders cannot take it).
    [[nodiscard]] TileCodec codec() const noexcept
    {
        if (avc444_) {
            return TileCodec::avc444;
        }
        if (h264_) {
            return TileCodec::avc420;
        }
        return progressive_ ? TileCodec::progressive : TileCodec::planar;
    }
    /// Part of the surface waits for a refinement (AVC444 chroma held back,
    /// Progressive tiles below full quality): send a frame even when nothing
    /// changed.
    [[nodiscard]] bool has_pending_refinement() const noexcept
    {
        return (avc444_.has_value() && avc444_->chroma_pending()) ||
               (progressive_.has_value() && progressive_->pending_tiles() > 0);
    }

    /// Sends the tiles of `frame` that changed (or were invalidated) as one
    /// frame and returns its ID; nullopt when nothing changed. Only when
    /// ready(); `frame` has the desktop's size.
    [[nodiscard]] std::optional<std::uint32_t> send_frame(const codec::ImageView& frame, std::uint32_t timestamp = 0);

    /// True when send_dmabuf_frame() works: the surface is AVC420 and its
    /// encoder takes dmabufs (H264Encoder::accepts_dmabuf()).
    [[nodiscard]] bool accepts_dmabuf() const noexcept { return h264_ != nullptr && h264_->accepts_dmabuf(); }
    /// Sends a frame in GPU memory as AVC420, without reading its pixels.
    /// The pipeline cannot diff what it does not see, so `damage` says what
    /// changed since the frame sent before (by either send_*; one rectangle
    /// of the desktop's size for everything). The regions are the tiles the
    /// damage or an invalidation touched; an IDR lists the whole surface.
    /// Returns the frame ID, or nullopt when nothing changed, the encoder
    /// dropped the picture (the damage stays for the next frame) or failed
    /// (the pipeline closes, as with send_frame). Errc::unsupported: the
    /// encoder cannot import this buffer; nothing was sent, and the frame's
    /// pixels should go to send_frame(), which then covers the damage too.
    /// Only when ready() and accepts_dmabuf(); `frame` has the desktop's size.
    [[nodiscard]] Result<std::optional<std::uint32_t>>
    send_dmabuf_frame(const video::DmabufFrame& frame, std::span<const PixelRect> damage, std::uint32_t timestamp = 0);
    /// The capture replaced its buffers: the encoder drops what it imported.
    void forget_dmabufs() noexcept;
    /// The next frame sends every tile.
    void invalidate_all();
    /// Encoder settings of the current quality tier (QualityController): the
    /// Progressive quantization, the H.264 rate control, and whether AVC444
    /// holds chroma back while the picture changes. They apply from the next
    /// frame on, and to encoders the pipeline creates later.
    void set_quality(const codec::rfx::Quant& progressive_quant, const video::RateControl& h264_rate,
                     bool defer_chroma = false);

private:
    using TileList = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

    void drain_gfx();
    void layout();
    void flush();
    void close(std::string reason);
    [[nodiscard]] bool tile_changed(const codec::ImageView& frame, std::uint32_t tx, std::uint32_t ty) const;
    /// One AVC420 WireToSurface1 for the whole surface, listing the changed
    /// tiles as regions ([MS-RDPEGFX] 2.2.4.4).
    [[nodiscard]] std::optional<std::uint32_t> send_h264_frame(const codec::ImageView& frame, std::uint16_t surface,
                                                               const TileList& tiles, std::uint32_t timestamp);
    /// The WireToSurface1 of an encoded AVC420 picture in a frame of its
    /// own; nullopt (nothing sent) when the encoder dropped the picture.
    [[nodiscard]] std::optional<std::uint32_t> send_avc420(const video::EncodedFrame& encoded, std::uint16_t surface,
                                                           const TileList& tiles, std::uint32_t timestamp);
    /// The tiles marked dirty, row by row.
    [[nodiscard]] TileList dirty_tiles() const;
    /// One AVC444 or AVC444v2 WireToSurface1 ([MS-RDPEGFX] 2.2.4.5, 2.2.4.6);
    /// the encoder picks the views and regions.
    [[nodiscard]] std::optional<std::uint32_t> send_avc444_frame(const codec::ImageView& frame, std::uint16_t surface,
                                                                 const TileList& tiles, std::uint32_t timestamp);
    void send_planar_tile(const codec::ImageView& frame, std::uint16_t surface, std::uint32_t tx, std::uint32_t ty);
    /// A tile with at most options_.max_clear_colours colours: text or UI.
    [[nodiscard]] bool few_colours(const codec::ImageView& frame, std::uint32_t tx, std::uint32_t ty) const;
    /// `area` of `frame` as one ClearCodec WireToSurface1; returns its size.
    std::size_t send_clear(const codec::ImageView& frame, std::uint16_t surface, const codec::progressive::Rect& area);
    /// TILE_UPGRADE passes for the tiles not in `changed`, within what is left
    /// of options_.upgrade_budget after `used` bytes.
    void send_upgrades(std::uint16_t surface, const TileList& changed, std::size_t used);
    /// Looks for a vertical scroll among `tiles` (the changed ones). When one
    /// is found, sends it as a SurfaceToSurface in the open frame, moves the
    /// same pixels in `previous_` and returns true: the tiles must then be
    /// diffed again.
    [[nodiscard]] bool move_scrolled(const codec::ImageView& frame, std::uint16_t surface, const TileList& tiles);
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
    PipelineOptions options_;
    std::optional<std::uint16_t> surface_;
    std::optional<codec::progressive::Encoder> progressive_;
    /// ClearCodec for text and UI tiles of a Progressive surface.
    std::optional<codec::clear::Encoder> clear_;
    H264Factory make_h264_;
    std::unique_ptr<video::H264Encoder> h264_;
    std::optional<codec::Yuv420Frame> yuv_;
    std::optional<video::Avc444Encoder> avc444_;
    std::optional<codec::rfx::Quant> progressive_quant_;  ///< set_quality(); the encoder's default before
    std::optional<video::RateControl> h264_rate_;
    video::Avc444Policy avc444_policy_;
    std::vector<std::byte> previous_;  ///< Last frame sent, tightly packed BGRX
    /// A dmabuf frame went out since: previous_ is not what the client has.
    bool previous_stale_ = false;
    std::vector<bool> dirty_;
    std::deque<PipelineEvent> events_;
    bool closed_ = false;
};

}  // namespace farland::server
