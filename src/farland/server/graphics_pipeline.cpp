// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/base/log.hpp>
#include <farland/codec/avc420.hpp>
#include <farland/codec/planar.hpp>
#include <farland/codec/yuv444.hpp>
#include <farland/server/graphics_pipeline.hpp>
#include <farland/server/scroll_detector.hpp>

#include <algorithm>
#include <array>
#include <functional>

namespace farland::server {

namespace {

namespace gfx = channels::rdpgfx;
namespace dvc = channels::dvc_event;
constexpr std::string_view log_component = "server.gfx";
constexpr std::size_t bytes_per_pixel = 4;
/// Scroll detection costs a few milliseconds at 1080p, so it only runs when
/// at least this many tiles changed.
constexpr std::size_t min_scroll_tiles = 4;

/// The AVC444 version for a surface: v2 when the client takes it and the
/// surface width is a multiple of 32, where every reading of the v2 layout
/// agrees (codec/yuv444.hpp); otherwise v1, which has no such ambiguity.
std::optional<codec::Avc444Version> avc444_version(const gfx::Negotiated& negotiated, std::uint16_t width)
{
    if (negotiated.allows(gfx::codec::avc444v2) && width % 32 == 0) {
        return codec::Avc444Version::v2;
    }
    if (negotiated.allows(gfx::codec::avc444)) {
        return codec::Avc444Version::v1;
    }
    return std::nullopt;
}

}  // namespace

GraphicsPipeline::GraphicsPipeline(DynamicChannels& channels, std::uint16_t width, std::uint16_t height,
                                   channels::rdpgfx::GfxServerConfig config, TileCodec codec, H264Factory make_h264,
                                   PipelineOptions options)
    : channels_(&channels), gfx_(config), width_(width), height_(height), tiles_x_((width + tile_size - 1) / tile_size),
      tiles_y_((height + tile_size - 1) / tile_size), requested_codec_(codec), options_(options),
      make_h264_(std::move(make_h264)),
      previous_(std::size_t{width} * height * bytes_per_pixel), dirty_(std::size_t{tiles_x_} * tiles_y_, true)
{
    FARLAND_ASSERT(width > 0 && height > 0);
    channel_id_ = channels_->open(std::string(channel_name));
}

bool GraphicsPipeline::handle(const channels::DvcEvent& event)
{
    return std::visit(
        [this](const auto& e) -> bool {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, dvc::CapabilitiesReady>) {
                return false;
            } else {
                if (e.id != channel_id_ || closed_) {
                    return false;
                }
                if constexpr (std::is_same_v<T, dvc::ChannelOpenFailed>) {
                    close("the client refused the graphics channel");
                } else if constexpr (std::is_same_v<T, dvc::ChannelData>) {
                    gfx_.receive(e.data);
                    drain_gfx();
                    flush();
                } else if constexpr (std::is_same_v<T, dvc::ChannelClosed>) {
                    closed_ = true;
                    surface_.reset();
                    events_.emplace_back(pipeline_event::Closed{"the client closed the graphics channel"});
                }
                // ChannelOpened: the client sends its Caps Advertise next.
                return true;
            }
        },
        event);
}

std::optional<PipelineEvent> GraphicsPipeline::poll_event()
{
    if (events_.empty()) {
        return std::nullopt;
    }
    PipelineEvent event = std::move(events_.front());
    events_.pop_front();
    return event;
}

void GraphicsPipeline::drain_gfx()
{
    while (auto event = gfx_.poll_event()) {
        std::visit(
            [this](auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, gfx::event::Ready>) {
                    log::info(log_component, "RDPGFX {}{}, flags {:#x}", gfx::version_name(e.negotiated.version),
                              e.reset ? " (client restarted the pipeline)" : "", e.negotiated.flags);
                    layout();
                    events_.emplace_back(pipeline_event::Ready{e.negotiated});
                } else if constexpr (std::is_same_v<T, gfx::event::FrameAcked>) {
                    events_.emplace_back(pipeline_event::FrameAcked{e.frame_id, e.queue_depth, e.known});
                } else if constexpr (std::is_same_v<T, gfx::event::Failed>) {
                    close("RDPGFX protocol error: " + e.reason);
                }
            },
            *event);
        if (closed_) {
            return;
        }
    }
}

void GraphicsPipeline::layout()
{
    // mstsc needs ResetGraphics before the first CreateSurface (docs/PLAN.md §4.1).
    gfx_.reset_graphics(width_, height_);
    surface_ = gfx_.create_surface(width_, height_);
    gfx_.map_surface_to_output(*surface_, 0, 0);
    // A fresh surface means a fresh codec context on the client: start over
    // with SYNC and CONTEXT.
    // A new encoder also starts H.264 over with an IDR frame.
    const auto& negotiated = gfx_.negotiated();
    progressive_.reset();
    h264_.reset();
    yuv_.reset();
    avc444_.reset();
    clear_.reset();
    const bool h264_requested = requested_codec_ == TileCodec::avc420 || requested_codec_ == TileCodec::avc444;
    if (h264_requested && negotiated && make_h264_) {
        // AVC444 falls back to AVC420 (8.1 clients have only that).
        const auto avc444 = requested_codec_ == TileCodec::avc444 ? avc444_version(*negotiated, width_) : std::nullopt;
        if (avc444 || negotiated->allows(gfx::codec::avc420)) {
            video::EncoderConfig config;
            config.width = codec::avc::coded_size(width_);
            config.height = codec::avc::coded_size(height_);
            if (h264_rate_) {
                config.rate = *h264_rate_;
            }
            if (avc444) {
                config.reference_frames = 2;  // each view predicts from its own last picture
            }
            if (auto encoder = make_h264_(config)) {
                if (avc444) {
                    avc444_.emplace(std::move(*encoder), width_, height_, *avc444, avc444_policy_);
                    log::info(log_component, "H.264 4:4:4 as {}",
                              *avc444 == codec::Avc444Version::v2 ? "AVC444v2" : "AVC444");
                } else {
                    h264_ = std::move(*encoder);
                    yuv_.emplace(config.width, config.height);
                }
            } else {
                log::warn(log_component, "no H.264 encoder ({}), using Progressive", encoder.error().message());
            }
        }
    }
    if (!h264_ && !avc444_ && requested_codec_ != TileCodec::planar && negotiated &&
        negotiated->allows(gfx::codec::progressive)) {
        // Refinement needs the reduce-extrapolate DWT (codec/progressive.hpp).
        progressive_.emplace(width_, height_,
                             codec::progressive::EncoderOptions{.reduce_extrapolate = options_.refine,
                                                                .refine = options_.refine});
        if (progressive_quant_) {
            progressive_->set_quant(*progressive_quant_);
        }
        // A new encoder starts the client's caches over with its first
        // stream (CLEARCODEC_FLAG_CACHE_RESET).
        if (options_.clearcodec && negotiated->allows(gfx::codec::clearcodec)) {
            clear_.emplace();
        }
    }
    invalidate_all();
}

void GraphicsPipeline::invalidate_all()
{
    std::ranges::fill(dirty_, true);
}

void GraphicsPipeline::set_quality(const codec::rfx::Quant& progressive_quant, const video::RateControl& h264_rate,
                                   bool defer_chroma)
{
    progressive_quant_ = progressive_quant;
    h264_rate_ = h264_rate;
    avc444_policy_.defer_chroma = defer_chroma;
    if (avc444_) {
        avc444_->set_policy(avc444_policy_);
    }
    if (progressive_) {
        progressive_->set_quant(progressive_quant);
    }
    video::H264Encoder* h264 = h264_.get();
    if (avc444_) {
        h264 = &avc444_->h264();
    }
    if (h264 != nullptr && h264->config().rate != h264_rate) {
        if (auto set = h264->set_rate_control(h264_rate); !set) {
            log::warn(log_component, "cannot change the H.264 rate control: {}", set.error().message());
        }
    }
}

void GraphicsPipeline::close(std::string reason)
{
    if (closed_) {
        return;
    }
    log::warn(log_component, "graphics pipeline closed: {}", reason);
    closed_ = true;
    surface_.reset();
    channels_->close(channel_id_);
    events_.emplace_back(pipeline_event::Closed{std::move(reason)});
}

std::optional<std::uint32_t> GraphicsPipeline::send_frame(const codec::ImageView& frame, std::uint32_t timestamp)
{
    FARLAND_ASSERT(ready());
    FARLAND_ASSERT(frame.width == width_ && frame.height == height_);
    if (!surface_) {
        return std::nullopt;  // not reached: ready() implies a surface
    }
    if (previous_stale_) {
        // Dmabuf frames went out that previous_ never saw: diffing against
        // it could miss a change back to what it holds.
        invalidate_all();
        previous_stale_ = false;
    }
    const std::uint16_t surface = *surface_;
    const auto changed_tiles = [&] {
        TileList changed;
        for (std::uint32_t ty = 0; ty < tiles_y_; ++ty) {
            for (std::uint32_t tx = 0; tx < tiles_x_; ++tx) {
                if (dirty_[(std::size_t{ty} * tiles_x_) + tx] || tile_changed(frame, tx, ty)) {
                    changed.emplace_back(tx, ty);
                }
            }
        }
        return changed;
    };
    TileList tiles = changed_tiles();
    if (avc444_) {
        // Even without damage when chroma is still owed (LC 2).
        return tiles.empty() && !avc444_->chroma_pending() ? std::nullopt
                                                           : send_avc444_frame(frame, surface, tiles, timestamp);
    }
    // Without changes a frame still goes out while Progressive tiles wait
    // for refinement.
    const bool refining = progressive_.has_value() && progressive_->pending_tiles() > 0;
    if (tiles.empty() && !refining) {
        return std::nullopt;
    }
    if (h264_) {
        return send_h264_frame(frame, surface, tiles, timestamp);
    }
    const std::uint32_t frame_id = gfx_.start_frame(timestamp);
    // A scroll: the client moves what it already has; what is left to encode
    // is what the move did not cover.
    if (move_scrolled(frame, surface, tiles)) {
        tiles = changed_tiles();
    }
    if (progressive_) {
        // Runs of adjacent few-colour tiles in a row (text, UI) go out as one
        // ClearCodec region each; the rest as RFX_PROGRESSIVE streams of at
        // most 16 KB, every one its own WireToSurface2 (docs/PLAN.md §4.1).
        std::vector<codec::progressive::Rect> clear_area;
        std::vector<codec::progressive::Rect> damage;
        for (std::size_t i = 0; i < tiles.size();) {
            const auto [tx, ty] = tiles[i];
            if (!clear_ || !few_colours(frame, tx, ty)) {
                damage.push_back(tile_rect(tx, ty));
                ++i;
                continue;
            }
            std::size_t stop = i + 1;
            while (stop < tiles.size() && tiles[stop].second == ty && tiles[stop].first == tiles[stop - 1].first + 1 &&
                   few_colours(frame, tiles[stop].first, ty)) {
                ++stop;
            }
            const auto first = tile_rect(tx, ty);
            const auto last = tile_rect(tiles[stop - 1].first, ty);
            clear_area.push_back({first.x, first.y, last.x + last.width - first.x, first.height});
            i = stop;
        }
        std::size_t bytes = 0;
        if (!clear_area.empty()) {
            // These pixels come from ClearCodec now: no refinement may paint over them.
            progressive_->discard(clear_area);
            for (const auto& area : clear_area) {
                bytes += send_clear(frame, surface, area);
            }
        }
        for (const auto& stream : progressive_->encode(frame, damage)) {
            bytes += stream.size();
            gfx_.wire_to_surface_2(surface, gfx::codec::progressive, progressive_context, gfx::pixel_format::xrgb_8888,
                                   stream);
        }
        send_upgrades(surface, tiles, bytes);
    } else {
        for (const auto& [tx, ty] : tiles) {
            send_planar_tile(frame, surface, tx, ty);
        }
    }
    for (const auto& [tx, ty] : tiles) {
        remember_tile(frame, tx, ty);
    }
    gfx_.end_frame();
    flush();
    return frame_id;
}

bool GraphicsPipeline::tile_changed(const codec::ImageView& frame, std::uint32_t tx, std::uint32_t ty) const
{
    const std::uint32_t x = tx * tile_size;
    const std::uint32_t y = ty * tile_size;
    const std::size_t row_bytes = std::size_t{std::min(tile_size, width_ - x)} * bytes_per_pixel;
    const std::uint32_t rows = std::min(tile_size, height_ - y);
    const std::span previous(previous_);
    for (std::uint32_t row = y; row < y + rows; ++row) {
        const auto current = frame.data.subspan((row * frame.stride) + (std::size_t{x} * bytes_per_pixel), row_bytes);
        const auto before = previous.subspan(((std::size_t{row} * width_) + x) * bytes_per_pixel, row_bytes);
        if (!std::ranges::equal(current, before)) {
            return true;
        }
    }
    return false;
}

std::optional<std::uint32_t> GraphicsPipeline::send_h264_frame(const codec::ImageView& frame, std::uint16_t surface,
                                                               const TileList& tiles, std::uint32_t timestamp)
{
    if (!yuv_) {
        return std::nullopt;  // not reached: an encoder always comes with its picture
    }
    codec::bgrx_to_yuv420(frame, *yuv_);
    const auto encoded = h264_->encode(yuv_->view());
    if (!encoded) {
        close("H.264 encoder failed: " + encoded.error().message());
        return std::nullopt;
    }
    const auto frame_id = send_avc420(*encoded, surface, tiles, timestamp);
    if (frame_id) {
        for (const auto& [tx, ty] : tiles) {
            remember_tile(frame, tx, ty);
        }
    }
    return frame_id;
}

Result<std::optional<std::uint32_t>> GraphicsPipeline::send_dmabuf_frame(const video::DmabufFrame& frame,
                                                                         std::span<const PixelRect> damage,
                                                                         std::uint32_t timestamp)
{
    FARLAND_ASSERT(ready() && accepts_dmabuf());
    FARLAND_ASSERT(frame.width == width_ && frame.height == height_);
    if (!surface_) {
        return std::optional<std::uint32_t>{};  // not reached: ready() implies a surface
    }
    // The damage marks tiles like an invalidation: they stay marked until a
    // frame carries them, whichever path it takes.
    for (const PixelRect& rect : damage) {
        if (rect.width == 0 || rect.height == 0 || rect.x >= width_ || rect.y >= height_) {
            continue;
        }
        const std::uint32_t right = std::min<std::uint32_t>(width_, rect.x + std::min(rect.width, 0xFFFFU));
        const std::uint32_t bottom = std::min<std::uint32_t>(height_, rect.y + std::min(rect.height, 0xFFFFU));
        for (std::uint32_t ty = rect.y / tile_size; ty <= (bottom - 1) / tile_size; ++ty) {
            for (std::uint32_t tx = rect.x / tile_size; tx <= (right - 1) / tile_size; ++tx) {
                dirty_[(std::size_t{ty} * tiles_x_) + tx] = true;
            }
        }
    }
    const TileList tiles = dirty_tiles();
    if (tiles.empty()) {
        return std::optional<std::uint32_t>{};
    }
    auto encoded = h264_->encode_dmabuf(frame);
    if (!encoded) {
        if (encoded.error().code == Errc::unsupported) {
            return std::unexpected(encoded.error());
        }
        close("H.264 encoder failed: " + encoded.error().message());
        return std::optional<std::uint32_t>{};
    }
    const auto frame_id = send_avc420(*encoded, *surface_, tiles, timestamp);
    if (frame_id) {
        for (const auto& [tx, ty] : tiles) {
            dirty_[(std::size_t{ty} * tiles_x_) + tx] = false;
        }
        previous_stale_ = true;
    }
    return frame_id;
}

void GraphicsPipeline::forget_dmabufs() noexcept
{
    if (h264_) {
        h264_->forget_dmabufs();
    }
}

std::optional<std::uint32_t> GraphicsPipeline::send_avc420(const video::EncodedFrame& encoded, std::uint16_t surface,
                                                           const TileList& tiles, std::uint32_t timestamp)
{
    if (encoded.bitstream.empty()) {
        return std::nullopt;  // the encoder dropped the picture; the damage stays for the next frame
    }
    // The client decodes the whole picture but only copies the regions to the
    // surface, so an IDR lists everything and later frames what changed.
    const codec::avc::QuantQuality quant{
        .qp = encoded.qp, .progressive = false, .quality = codec::avc::quality_from_qp(encoded.qp)};
    std::vector<codec::avc::Region> regions;
    const auto add = [&](const codec::progressive::Rect& r) {
        regions.push_back({{static_cast<std::uint16_t>(r.x), static_cast<std::uint16_t>(r.y),
                            static_cast<std::uint16_t>(r.x + r.width), static_cast<std::uint16_t>(r.y + r.height)},
                           quant});
    };
    if (encoded.idr) {
        add({0, 0, width_, height_});
    } else {
        for (const auto& [tx, ty] : tiles) {
            add(tile_rect(tx, ty));
        }
    }
    gfx::Rect16 bounds{width_, height_, 0, 0};
    for (const auto& region : regions) {
        bounds.left = std::min(bounds.left, region.rect.left);
        bounds.top = std::min(bounds.top, region.rect.top);
        bounds.right = std::max(bounds.right, region.rect.right);
        bounds.bottom = std::max(bounds.bottom, region.rect.bottom);
    }
    const std::uint32_t frame_id = gfx_.start_frame(timestamp);
    gfx_.wire_to_surface_1(surface, gfx::codec::avc420, gfx::pixel_format::xrgb_8888, bounds,
                           codec::avc::encode_avc420(regions, encoded.bitstream));
    gfx_.end_frame();
    flush();
    return frame_id;
}

GraphicsPipeline::TileList GraphicsPipeline::dirty_tiles() const
{
    TileList tiles;
    for (std::uint32_t ty = 0; ty < tiles_y_; ++ty) {
        for (std::uint32_t tx = 0; tx < tiles_x_; ++tx) {
            if (dirty_[(std::size_t{ty} * tiles_x_) + tx]) {
                tiles.emplace_back(tx, ty);
            }
        }
    }
    return tiles;
}

std::optional<std::uint32_t> GraphicsPipeline::send_avc444_frame(const codec::ImageView& frame, std::uint16_t surface,
                                                                 const TileList& tiles, std::uint32_t timestamp)
{
    if (!avc444_) {
        return std::nullopt;  // not reached: send_frame() only calls it with an AVC444 encoder
    }
    std::vector<codec::avc::Rect16> damage;
    damage.reserve(tiles.size());
    for (const auto& [tx, ty] : tiles) {
        const auto r = tile_rect(tx, ty);
        damage.push_back({static_cast<std::uint16_t>(r.x), static_cast<std::uint16_t>(r.y),
                          static_cast<std::uint16_t>(r.x + r.width), static_cast<std::uint16_t>(r.y + r.height)});
    }
    const auto encoded = avc444_->encode(frame, damage);
    if (!encoded) {
        close("H.264 encoder failed: " + encoded.error().message());
        return std::nullopt;
    }
    const auto& picture = *encoded;
    if (!picture) {
        return std::nullopt;  // the encoder dropped the picture; the damage stays for the next frame
    }
    const video::Avc444Frame& out = *picture;
    const std::uint16_t codec_id =
        avc444_->version() == codec::Avc444Version::v2 ? gfx::codec::avc444v2 : gfx::codec::avc444;
    const std::uint32_t frame_id = gfx_.start_frame(timestamp);
    gfx_.wire_to_surface_1(
        surface, codec_id, gfx::pixel_format::xrgb_8888,
        gfx::Rect16{out.dest_rect.left, out.dest_rect.top, out.dest_rect.right, out.dest_rect.bottom},
        out.bitmap_stream);
    for (const auto& [tx, ty] : tiles) {
        remember_tile(frame, tx, ty);
    }
    gfx_.end_frame();
    flush();
    return frame_id;
}

bool GraphicsPipeline::move_scrolled(const codec::ImageView& frame, std::uint16_t surface, const TileList& tiles)
{
    // The client's surface equals previous_ only while no tile is
    // invalidated; moving anything else would move pixels it does not have.
    if (tiles.size() < min_scroll_tiles || std::ranges::any_of(dirty_, std::identity{})) {
        return false;
    }
    std::uint32_t tx0 = tiles_x_;
    std::uint32_t ty0 = tiles_y_;
    std::uint32_t tx1 = 0;
    std::uint32_t ty1 = 0;
    for (const auto& [tx, ty] : tiles) {
        tx0 = std::min(tx0, tx);
        ty0 = std::min(ty0, ty);
        tx1 = std::max(tx1, tx + 1);
        ty1 = std::max(ty1, ty + 1);
    }
    const auto first = tile_rect(tx0, ty0);
    const auto last = tile_rect(tx1 - 1, ty1 - 1);
    const PixelRect area{first.x, first.y, last.x + last.width - first.x, last.y + last.height - first.y};
    const codec::ImageView before{previous_, width_, height_, std::size_t{width_} * bytes_per_pixel};
    const auto move = detect_vertical_scroll(before, frame, area);
    if (!move) {
        return false;
    }
    const auto& source = move->source;
    const auto destination = move->destination();
    const gfx::Rect16 source_rect{static_cast<std::uint16_t>(source.x), static_cast<std::uint16_t>(source.y),
                                  static_cast<std::uint16_t>(source.x + source.width),
                                  static_cast<std::uint16_t>(source.y + source.height)};
    const std::array points{
        gfx::Point16{static_cast<std::int16_t>(destination.x), static_cast<std::int16_t>(destination.y)}};
    gfx_.surface_to_surface(surface, surface, source_rect, points);
    apply_scroll(previous_, std::size_t{width_} * bytes_per_pixel, *move);
    if (progressive_) {
        // The client's refinement state of the destination no longer matches
        // its pixels.
        const std::array moved{
            codec::progressive::Rect{destination.x, destination.y, destination.width, destination.height}};
        progressive_->discard(moved);
    }
    return true;
}

bool GraphicsPipeline::few_colours(const codec::ImageView& frame, std::uint32_t tx, std::uint32_t ty) const
{
    // Open addressing on the BGR value, stopping at the first colour too many.
    constexpr std::size_t slots = 128;  // a power of two, twice the most colours counted
    constexpr std::uint32_t empty = 0xFFFF'FFFFU;  // no BGR value
    const std::uint32_t limit = std::min(options_.max_clear_colours, std::uint32_t{slots / 2});
    std::array<std::uint32_t, slots> table{};
    table.fill(empty);
    std::uint32_t count = 0;
    std::uint32_t last = empty;
    const auto r = tile_rect(tx, ty);
    for (std::uint32_t row = r.y; row < r.y + r.height; ++row) {
        const auto line = frame.data.subspan((row * frame.stride) + (std::size_t{r.x} * bytes_per_pixel),
                                             std::size_t{r.width} * bytes_per_pixel);
        for (std::size_t i = 0; i < line.size(); i += bytes_per_pixel) {
            const std::uint32_t colour = std::to_integer<std::uint32_t>(line[i]) |
                                         (std::to_integer<std::uint32_t>(line[i + 1]) << 8U) |
                                         (std::to_integer<std::uint32_t>(line[i + 2]) << 16U);
            if (colour == last) {
                continue;  // runs are the common case in text and UI
            }
            last = colour;
            std::size_t slot = (colour * 0x9E37'79B1U) >> 25U;  // the top 7 bits
            while (table[slot] != empty && table[slot] != colour) {
                slot = (slot + 1) & (slots - 1);
            }
            if (table[slot] == empty) {
                table[slot] = colour;
                if (++count > limit) {
                    return false;
                }
            }
        }
    }
    return true;
}

std::size_t GraphicsPipeline::send_clear(const codec::ImageView& frame, std::uint16_t surface,
                                         const codec::progressive::Rect& area)
{
    if (!clear_) {
        return 0;  // not reached: send_frame() only calls it with an encoder
    }
    const std::size_t offset = (area.y * frame.stride) + (std::size_t{area.x} * bytes_per_pixel);
    const codec::ImageView region{
        frame.data.subspan(offset, ((area.height - 1) * frame.stride) + (std::size_t{area.width} * bytes_per_pixel)),
        area.width, area.height, frame.stride};
    const auto stream = clear_->encode(region);
    const gfx::Rect16 rect{static_cast<std::uint16_t>(area.x), static_cast<std::uint16_t>(area.y),
                           static_cast<std::uint16_t>(area.x + area.width),
                           static_cast<std::uint16_t>(area.y + area.height)};
    gfx_.wire_to_surface_1(surface, gfx::codec::clearcodec, gfx::pixel_format::xrgb_8888, rect, stream);
    return stream.size();
}

void GraphicsPipeline::send_upgrades(std::uint16_t surface, const TileList& changed, std::size_t used)
{
    if (!progressive_ || progressive_->pending_tiles() == 0 || used >= options_.upgrade_budget) {
        return;
    }
    const std::size_t budget = options_.upgrade_budget - used;
    std::vector<std::vector<std::byte>> streams;
    if (changed.empty()) {
        streams = progressive_->upgrade(budget);
    } else {
        // What changed in this frame just started over; refine the rest.
        std::vector<bool> skip(dirty_.size());
        for (const auto& [tx, ty] : changed) {
            skip[(std::size_t{ty} * tiles_x_) + tx] = true;
        }
        std::vector<codec::progressive::Rect> area;
        for (std::uint32_t ty = 0; ty < tiles_y_; ++ty) {
            for (std::uint32_t tx = 0; tx < tiles_x_; ++tx) {
                if (!skip[(std::size_t{ty} * tiles_x_) + tx]) {
                    area.push_back(tile_rect(tx, ty));
                }
            }
        }
        streams = progressive_->upgrade(area, budget);
    }
    for (const auto& stream : streams) {
        gfx_.wire_to_surface_2(surface, gfx::codec::progressive, progressive_context, gfx::pixel_format::xrgb_8888,
                               stream);
    }
}

codec::progressive::Rect GraphicsPipeline::tile_rect(std::uint32_t tx, std::uint32_t ty) const noexcept
{
    const std::uint32_t x = tx * tile_size;
    const std::uint32_t y = ty * tile_size;
    return {x, y, std::min(tile_size, width_ - x), std::min(tile_size, height_ - y)};
}

void GraphicsPipeline::send_planar_tile(const codec::ImageView& frame, std::uint16_t surface, std::uint32_t tx,
                                        std::uint32_t ty)
{
    const auto r = tile_rect(tx, ty);
    const std::size_t offset = (r.y * frame.stride) + (std::size_t{r.x} * bytes_per_pixel);
    const codec::ImageView tile{
        frame.data.subspan(offset, ((r.height - 1) * frame.stride) + (std::size_t{r.width} * bytes_per_pixel)), r.width,
        r.height, frame.stride};

    // RDPGFX planar is top-down ([MS-RDPEGFX] 2.2.4.3; FreeRDP decodes it without vFlip).
    const auto data =
        codec::planar::encode(tile, {codec::planar::Mode::automatic, codec::planar::Orientation::top_down});
    const gfx::Rect16 rect{static_cast<std::uint16_t>(r.x), static_cast<std::uint16_t>(r.y),
                           static_cast<std::uint16_t>(r.x + r.width), static_cast<std::uint16_t>(r.y + r.height)};
    gfx_.wire_to_surface_1(surface, gfx::codec::planar, gfx::pixel_format::xrgb_8888, rect, data);
}

void GraphicsPipeline::remember_tile(const codec::ImageView& frame, std::uint32_t tx, std::uint32_t ty)
{
    const auto r = tile_rect(tx, ty);
    const std::size_t row_bytes = std::size_t{r.width} * bytes_per_pixel;
    const std::span previous(previous_);
    for (std::uint32_t row = r.y; row < r.y + r.height; ++row) {
        const auto source = frame.data.subspan((row * frame.stride) + (std::size_t{r.x} * bytes_per_pixel), row_bytes);
        std::ranges::copy(source, previous.subspan(((std::size_t{row} * width_) + r.x) * bytes_per_pixel).begin());
    }
    dirty_[(std::size_t{ty} * tiles_x_) + tx] = false;
}

void GraphicsPipeline::flush()
{
    std::vector<std::byte> batch;
    const auto send_batch = [&] {
        if (!batch.empty()) {
            channels_->send(channel_id_, zgfx_.compress(batch));
            batch.clear();
        }
    };
    for (const auto& pdu : gfx_.take_output()) {
        if (!batch.empty() && batch.size() + pdu.size() > max_batch_size) {
            send_batch();
        }
        batch.insert(batch.end(), pdu.begin(), pdu.end());
    }
    send_batch();
}

}  // namespace farland::server
