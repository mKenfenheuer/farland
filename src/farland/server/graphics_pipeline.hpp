// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/channels/rdpgfx_server.hpp>
#include <farland/codec/clear.hpp>
#include <farland/codec/image.hpp>
#include <farland/codec/progressive.hpp>
#include <farland/codec/yuv.hpp>
#include <farland/codec/zgfx.hpp>
#include <farland/server/display_layout.hpp>
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
#include <utility>
#include <variant>
#include <vector>

/// The Graphics Pipeline output of one session ([MS-RDPEGFX], docs/PLAN.md
/// §3.4) over a dynamic virtual channel. It opens the channel, answers the
/// capability exchange through GfxServer, lays out the output (an
/// OutputLayout: one surface per screen, mapped where the screen shows, and
/// black surfaces around letterboxed pictures), and sends each frame as the
/// 64x64 tiles that changed. Every RDPGFX PDU goes out ZGFX-compressed
/// ([MS-RDPEGFX] 2.2.5), several PDUs to a message.
///
/// Frames are encoded with RemoteFX Progressive by default, with planar
/// (lossless) on request or for clients that cannot take Progressive (thin
/// clients), or as H.264 when an encoder is available and the client allows
/// it: AVC420, or AVC444 for full chroma. Each screen has its own encoders.
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
    /// Let clients that allow it scale down pictures larger than their place
    /// (MapSurfaceToScaledOutput, [MS-RDPEGFX] 2.2.2.22); otherwise the
    /// caller scales them.
    bool scaled_output = true;
};

namespace pipeline_event {
/// The surfaces are laid out and frames may be sent. Comes again after the
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
    /// ready) for `layout`. `channels` must outlive the pipeline.
    GraphicsPipeline(DynamicChannels& channels, OutputLayout layout, channels::rdpgfx::GfxServerConfig config = {},
                     TileCodec codec = TileCodec::progressive, H264Factory make_h264 = {},
                     PipelineOptions options = {});
    /// One screen covering a desktop of `width` x `height`.
    GraphicsPipeline(DynamicChannels& channels, std::uint16_t width, std::uint16_t height,
                     channels::rdpgfx::GfxServerConfig config = {}, TileCodec codec = TileCodec::progressive,
                     H264Factory make_h264 = {}, PipelineOptions options = {});

    /// Handles `event` if it concerns the graphics channel. False otherwise.
    bool handle(const channels::DvcEvent& event);
    [[nodiscard]] std::optional<PipelineEvent> poll_event();

    [[nodiscard]] bool ready() const noexcept { return laid_out_ && !closed_; }
    [[nodiscard]] bool closed() const noexcept { return closed_; }

    /// A new layout (the client's monitors or the screens changed). Once
    /// ready, it applies at once: ResetGraphics, new surfaces, new encoders,
    /// everything sent again. Not inside a frame.
    void set_layout(OutputLayout layout);
    [[nodiscard]] const OutputLayout& layout() const noexcept { return layout_; }
    /// Surfaces for pictures, one per layout().screens entry, once ready.
    [[nodiscard]] std::size_t screen_count() const noexcept { return screens_.size(); }
    /// The picture size screen `index` takes: the picture's own size, or its
    /// target size when the client cannot scale it (scale it first then).
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> surface_size(std::size_t index) const;

    /// The codec frames use once ready (the requested one, or a fallback
    /// when the client or the encoders cannot take it), for the first screen.
    [[nodiscard]] TileCodec codec() const noexcept;
    /// Part of a surface waits for a refinement (AVC444 chroma held back,
    /// Progressive tiles below full quality): send a frame even when nothing
    /// changed.
    [[nodiscard]] bool has_pending_refinement() const noexcept;

    /// A frame of several screens: begin_frame(), then add_frame() or
    /// add_dmabuf_frame() for each screen that has a new picture, then
    /// end_frame(). Only when ready.
    void begin_frame(std::uint32_t timestamp = 0);
    /// Encodes the tiles of screen `index` that changed (or were invalidated)
    /// into the frame; `frame` has surface_size(index).
    void add_frame(std::size_t index, const codec::ImageView& frame);
    /// The zero-copy path for screen `index`: a picture in GPU memory as
    /// AVC420, without reading its pixels. The pipeline cannot diff what it
    /// does not see, so `damage` says what changed since the picture sent
    /// before (by either add_*; one rectangle of the surface's size for
    /// everything). The regions are the tiles the damage or an invalidation
    /// touched; an IDR lists the whole surface. Returns whether a picture
    /// went out: not when nothing changed, or the encoder dropped the picture
    /// (the damage stays for the next frame) or failed (the pipeline
    /// closes). Errc::unsupported: the encoder cannot import this buffer;
    /// nothing was added, and the picture's pixels should go to add_frame(),
    /// which then covers the damage too. Only for accepts_dmabuf(index);
    /// `frame` has surface_size(index).
    [[nodiscard]] Result<bool> add_dmabuf_frame(std::size_t index, const video::DmabufFrame& frame,
                                                std::span<const PixelRect> damage);
    /// Closes the frame and returns its ID; nullopt when it stayed empty.
    /// A screen that got no picture is refined here (has_pending_refinement()):
    /// a still desktop sends no frames of its own, so its Progressive tiles
    /// would stay at the quality their TILE_FIRST pass had.
    [[nodiscard]] std::optional<std::uint32_t> end_frame();

    /// A frame of the first screen alone: add_frame(0, frame).
    [[nodiscard]] std::optional<std::uint32_t> send_frame(const codec::ImageView& frame, std::uint32_t timestamp = 0);
    /// A frame of the first screen alone: add_dmabuf_frame(0, ...). Returns
    /// the frame ID, or nullopt when nothing went out.
    [[nodiscard]] Result<std::optional<std::uint32_t>>
    send_dmabuf_frame(const video::DmabufFrame& frame, std::span<const PixelRect> damage, std::uint32_t timestamp = 0);

    /// True when add_dmabuf_frame() works for screen `index`: the surface is
    /// AVC420 and its encoder takes dmabufs (H264Encoder::accepts_dmabuf()).
    [[nodiscard]] bool accepts_dmabuf(std::size_t index) const noexcept;
    [[nodiscard]] bool accepts_dmabuf() const noexcept { return accepts_dmabuf(0); }
    /// The capture of screen `index` replaced its buffers: the encoder drops
    /// what it imported.
    void forget_dmabufs(std::size_t index) noexcept;
    /// forget_dmabufs() for every screen.
    void forget_dmabufs() noexcept;
    /// The next frame sends every tile of every screen.
    void invalidate_all();
    /// Encoder settings of the current quality tier (QualityController): the
    /// Progressive quantization, the H.264 rate control, and whether AVC444
    /// holds chroma back while the picture changes. They apply from the next
    /// frame on, and to encoders the pipeline creates later.
    void set_quality(const codec::rfx::Quant& progressive_quant, const video::RateControl& h264_rate,
                     bool defer_chroma = false);

private:
    using TileList = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

    /// One surface showing one screen's picture, with its encoders.
    struct Screen {
        std::uint16_t surface = 0;
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        std::uint32_t tiles_x = 0;
        std::uint32_t tiles_y = 0;
        std::optional<codec::progressive::Encoder> progressive;
        /// ClearCodec for text and UI tiles of a Progressive surface.
        std::optional<codec::clear::Encoder> clear;
        std::unique_ptr<video::H264Encoder> h264;
        std::optional<codec::Yuv420Frame> yuv;
        std::optional<video::Avc444Encoder> avc444;
        std::vector<std::byte> previous;  ///< Last picture sent, tightly packed BGRX
        /// A dmabuf picture went out since: `previous` is not what the client has.
        bool previous_stale = false;
        /// A picture of this screen went into the open frame, which refined it
        /// already; end_frame() refines the others.
        bool in_frame = false;
        std::vector<bool> dirty;
    };

    void drain_gfx();
    void lay_out();
    void create_encoders(Screen& screen);
    /// Opens the frame for the first command that goes into it.
    void ensure_frame();
    void flush();
    void close(std::string reason);
    [[nodiscard]] static bool tile_changed(const Screen& s, const codec::ImageView& frame, std::uint32_t tx,
                                           std::uint32_t ty);
    /// One AVC420 WireToSurface1 for the whole surface, listing the changed
    /// tiles as regions ([MS-RDPEGFX] 2.2.4.4).
    void send_h264_frame(Screen& s, const codec::ImageView& frame, const TileList& tiles);
    /// The WireToSurface1 of an encoded AVC420 picture; false (nothing sent)
    /// when the encoder dropped the picture.
    bool send_avc420(Screen& s, const video::EncodedFrame& encoded, const TileList& tiles);
    /// The tiles marked dirty, row by row.
    [[nodiscard]] static TileList dirty_tiles(const Screen& s);
    /// One AVC444 or AVC444v2 WireToSurface1 ([MS-RDPEGFX] 2.2.4.5, 2.2.4.6);
    /// the encoder picks the views and regions.
    void send_avc444_frame(Screen& s, const codec::ImageView& frame, const TileList& tiles);
    void send_planar_tile(Screen& s, const codec::ImageView& frame, std::uint32_t tx, std::uint32_t ty);
    /// A tile with at most options_.max_clear_colours colours: text or UI.
    [[nodiscard]] bool few_colours(const Screen& s, const codec::ImageView& frame, std::uint32_t tx,
                                   std::uint32_t ty) const;
    /// `area` of `frame` as one ClearCodec WireToSurface1; returns its size.
    std::size_t send_clear(Screen& s, const codec::ImageView& frame, const codec::progressive::Rect& area);
    /// TILE_UPGRADE passes for the tiles not in `changed`, within what is left
    /// of options_.upgrade_budget after `used` bytes.
    void send_upgrades(Screen& s, const TileList& changed, std::size_t used);
    /// What a screen that got no picture in the open frame still owes the
    /// client: Progressive upgrades, or the AVC444 chroma held back earlier.
    void refine_still_screen(Screen& s);
    /// Looks for a vertical scroll among `tiles` (the changed ones). When one
    /// is found, sends it as a SurfaceToSurface in the open frame, moves the
    /// same pixels in `previous` and returns true: the tiles must then be
    /// diffed again.
    [[nodiscard]] bool move_scrolled(Screen& s, const codec::ImageView& frame, const TileList& tiles);
    /// Remembers the tile's pixels as sent and clears its dirty flag.
    static void remember_tile(Screen& s, const codec::ImageView& frame, std::uint32_t tx, std::uint32_t ty);
    /// The tile's rectangle, clipped to the surface.
    [[nodiscard]] static codec::progressive::Rect tile_rect(const Screen& s, std::uint32_t tx,
                                                            std::uint32_t ty) noexcept;

    /// The one Progressive codec context of each surface.
    static constexpr std::uint32_t progressive_context = 1;

    DynamicChannels* channels_;  ///< never null
    std::uint32_t channel_id_ = 0;
    channels::rdpgfx::GfxServer gfx_;
    codec::ZgfxCompressor zgfx_;
    OutputLayout layout_;
    TileCodec requested_codec_;
    PipelineOptions options_;
    bool laid_out_ = false;
    std::vector<std::unique_ptr<Screen>> screens_;
    /// Black surfaces: borders and monitors without a picture.
    std::vector<std::uint16_t> fill_surfaces_;
    H264Factory make_h264_;
    std::optional<codec::rfx::Quant> progressive_quant_;  ///< set_quality(); the encoder's default before
    std::optional<video::RateControl> h264_rate_;
    video::Avc444Policy avc444_policy_;
    std::uint32_t frame_timestamp_ = 0;
    /// The frame begin_frame() started, once a command went into it.
    std::optional<std::uint32_t> frame_id_;
    std::deque<PipelineEvent> events_;
    bool closed_ = false;
};

}  // namespace farland::server
