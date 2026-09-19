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

/// Per-tile motion (GraphicsPipeline::Screen::motion): a frame that changes a
/// tile adds `motion_rise`, one that leaves it alone takes `motion_fall`
/// away, and the count stops at `motion_max`. From `motion_hot` on the tile
/// counts as moving picture: four frames in a row raise it there, while a
/// caret that blinks once a second falls back to zero in between.
constexpr std::uint8_t motion_rise = 2;
constexpr std::uint8_t motion_fall = 1;
constexpr std::uint8_t motion_max = 12;
constexpr std::uint8_t motion_hot = 8;
/// Frames since a tile last changed, capped where nothing looks any further.
constexpr std::uint8_t still_max = 250;

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

gfx::Rect16 to_rect16(const codec::progressive::Rect& r)
{
    return {static_cast<std::uint16_t>(r.x), static_cast<std::uint16_t>(r.y), static_cast<std::uint16_t>(r.x + r.width),
            static_cast<std::uint16_t>(r.y + r.height)};
}

OutputLayout single_screen(std::uint16_t width, std::uint16_t height)
{
    FARLAND_ASSERT(width > 0 && height > 0);
    const std::array size{std::pair<std::uint32_t, std::uint32_t>{width, height}};
    return DisplayLayout::single(width, height).place(size);
}

}  // namespace

GraphicsPipeline::GraphicsPipeline(DynamicChannels& channels, OutputLayout layout,
                                   channels::rdpgfx::GfxServerConfig config, TileCodec codec, H264Factory make_h264,
                                   PipelineOptions options)
    : channels_(&channels), gfx_(config), layout_(std::move(layout)), requested_codec_(codec), options_(options),
      make_h264_(std::move(make_h264))
{
    FARLAND_ASSERT(layout_.width > 0 && layout_.height > 0);
    channel_id_ = channels_->open(std::string(channel_name));
}

GraphicsPipeline::GraphicsPipeline(DynamicChannels& channels, std::uint16_t width, std::uint16_t height,
                                   channels::rdpgfx::GfxServerConfig config, TileCodec codec, H264Factory make_h264,
                                   PipelineOptions options)
    : GraphicsPipeline(channels, single_screen(width, height), config, codec, std::move(make_h264), options)
{
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
                    laid_out_ = false;
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
                    lay_out();
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

void GraphicsPipeline::set_layout(OutputLayout layout)
{
    FARLAND_ASSERT(layout.width > 0 && layout.height > 0);
    FARLAND_ASSERT(!frame_id_);
    if (layout == layout_) {
        return;
    }
    layout_ = std::move(layout);
    if (ready()) {
        lay_out();
        flush();
    }
}

void GraphicsPipeline::lay_out()
{
    // ResetGraphics keeps the client's surfaces (FreeRDP only clears them),
    // so the old ones go first.
    for (const auto& s : screens_) {
        if (gfx_.has_surface(s->surface)) {
            gfx_.delete_surface(s->surface);
        }
    }
    for (const auto id : fill_surfaces_) {
        if (gfx_.has_surface(id)) {
            gfx_.delete_surface(id);
        }
    }
    screens_.clear();
    fill_surfaces_.clear();
    // mstsc needs ResetGraphics before the first CreateSurface (docs/PLAN.md §4.1).
    gfx_.reset_graphics(layout_.width, layout_.height, layout_.monitors);
    const auto& negotiated = gfx_.negotiated();
    const bool client_scales = options_.scaled_output && negotiated && negotiated->scaled_output;
    for (const auto& placement : layout_.screens) {
        auto s = std::make_unique<Screen>();
        const auto& target = placement.target;
        // A picture larger than its place goes to a surface of its own size
        // that the client scales, or else the caller scales it to fit.
        const bool scaled = placement.scaled() && client_scales;
        s->width = static_cast<std::uint16_t>(scaled ? placement.width : target.width);
        s->height = static_cast<std::uint16_t>(scaled ? placement.height : target.height);
        s->tiles_x = (s->width + tile_size - 1) / tile_size;
        s->tiles_y = (s->height + tile_size - 1) / tile_size;
        s->previous.assign(std::size_t{s->width} * s->height * bytes_per_pixel, std::byte{0});
        const std::size_t tiles = std::size_t{s->tiles_x} * s->tiles_y;
        s->dirty.assign(tiles, true);
        s->motion.assign(tiles, 0);
        s->still.assign(tiles, 0);
        s->exact.assign(tiles, false);
        s->from_video.assign(tiles, false);
        s->surface = gfx_.create_surface(s->width, s->height);
        if (scaled) {
            gfx_.map_surface_to_scaled_output(s->surface, target.x, target.y, target.width, target.height);
        } else {
            gfx_.map_surface_to_output(s->surface, target.x, target.y);
        }
        // A fresh surface means a fresh codec context on the client: start
        // over with SYNC and CONTEXT, and H.264 with an IDR frame.
        create_encoders(*s);
        screens_.push_back(std::move(s));
    }
    if (!layout_.fills.empty()) {
        // Black surfaces for the borders, filled once: the client's output
        // buffer outside surfaces holds whatever it held.
        for (const auto& fill : layout_.fills) {
            const auto id =
                gfx_.create_surface(static_cast<std::uint16_t>(fill.width), static_cast<std::uint16_t>(fill.height));
            gfx_.map_surface_to_output(id, fill.x, fill.y);
            fill_surfaces_.push_back(id);
        }
        static_cast<void>(gfx_.start_frame());
        for (std::size_t i = 0; i < fill_surfaces_.size(); ++i) {
            const auto& fill = layout_.fills[i];
            const std::array rect{
                gfx::Rect16{0, 0, static_cast<std::uint16_t>(fill.width), static_cast<std::uint16_t>(fill.height)}};
            gfx_.solid_fill(fill_surfaces_[i], gfx::Color32{0, 0, 0, 0xFF}, rect);
        }
        gfx_.end_frame();
    }
    laid_out_ = true;
    if (screens_.size() > 1 || !fill_surfaces_.empty()) {
        log::info(log_component, "output {}x{}: {} surfaces for pictures, {} for borders", layout_.width,
                  layout_.height, screens_.size(), fill_surfaces_.size());
    }
}

void GraphicsPipeline::create_encoders(Screen& s)
{
    const auto& negotiated = gfx_.negotiated();
    const bool h264_requested = requested_codec_ == TileCodec::avc420 || requested_codec_ == TileCodec::avc444;
    if (h264_requested && negotiated && make_h264_) {
        // AVC444 falls back to AVC420 (8.1 clients have only that).
        const auto avc444 = requested_codec_ == TileCodec::avc444 ? avc444_version(*negotiated, s.width) : std::nullopt;
        if (avc444 || negotiated->allows(gfx::codec::avc420)) {
            video::EncoderConfig config;
            config.width = codec::avc::coded_size(s.width);
            config.height = codec::avc::coded_size(s.height);
            if (h264_rate_) {
                config.rate = *h264_rate_;
            }
            if (avc444) {
                config.reference_frames = 2;  // each view predicts from its own last picture
            }
            if (auto encoder = make_h264_(config)) {
                if (avc444) {
                    s.avc444.emplace(std::move(*encoder), s.width, s.height, *avc444, avc444_policy_);
                    log::info(log_component, "H.264 4:4:4 as {}",
                              *avc444 == codec::Avc444Version::v2 ? "AVC444v2" : "AVC444");
                } else {
                    s.h264 = std::move(*encoder);
                    s.yuv.emplace(config.width, config.height);
                }
            } else {
                log::warn(log_component, "no H.264 encoder ({}), using Progressive", encoder.error().message());
            }
        }
    }
    if (!s.h264 && !s.avc444 && requested_codec_ != TileCodec::planar && negotiated &&
        negotiated->allows(gfx::codec::progressive)) {
        // Refinement needs the reduce-extrapolate DWT (codec/progressive.hpp).
        s.progressive.emplace(
            s.width, s.height,
            codec::progressive::EncoderOptions{.reduce_extrapolate = options_.refine, .refine = options_.refine});
        if (progressive_quant_) {
            s.progressive->set_quant(*progressive_quant_);
        }
        // A new encoder starts the client's caches over with its first
        // stream (CLEARCODEC_FLAG_CACHE_RESET).
        if (options_.clearcodec && negotiated->allows(gfx::codec::clearcodec)) {
            s.clear.emplace();
        }
    }
}

std::pair<std::uint32_t, std::uint32_t> GraphicsPipeline::surface_size(std::size_t index) const
{
    FARLAND_ASSERT(index < screens_.size());
    return {screens_[index]->width, screens_[index]->height};
}

TileCodec GraphicsPipeline::codec() const noexcept
{
    if (screens_.empty()) {
        return requested_codec_ == TileCodec::planar ? TileCodec::planar : TileCodec::progressive;
    }
    const auto& s = *screens_.front();
    if (s.avc444) {
        return TileCodec::avc444;
    }
    if (s.h264) {
        return TileCodec::avc420;
    }
    return s.progressive ? TileCodec::progressive : TileCodec::planar;
}

bool GraphicsPipeline::has_pending_refinement() const noexcept
{
    return std::ranges::any_of(screens_, [this](const auto& s) {
        return (s->avc444.has_value() && s->avc444->chroma_pending()) ||
               (s->progressive.has_value() && s->progressive->pending_tiles() > 0) || exact_pending(*s);
    });
}

bool GraphicsPipeline::exact_pending(const Screen& s) const noexcept
{
    if (!options_.lossless_still || !s.progressive) {
        return false;
    }
    for (std::uint32_t ty = 0; ty < s.tiles_y; ++ty) {
        for (std::uint32_t tx = 0; tx < s.tiles_x; ++tx) {
            const std::size_t i = (std::size_t{ty} * s.tiles_x) + tx;
            if (!s.exact[i] && !s.dirty[i] && s.motion[i] < motion_hot &&
                s.progressive->tile_stage(tx, ty) == codec::progressive::full_quality_stage) {
                return true;
            }
        }
    }
    return false;
}

bool GraphicsPipeline::accepts_dmabuf(std::size_t index) const noexcept
{
    return index < screens_.size() && screens_[index]->h264 != nullptr && screens_[index]->h264->accepts_dmabuf();
}

void GraphicsPipeline::invalidate_all()
{
    for (const auto& s : screens_) {
        std::ranges::fill(s->dirty, true);
        std::ranges::fill(s->exact, false);
        std::ranges::fill(s->from_video, false);
    }
}

void GraphicsPipeline::set_quality(const codec::rfx::Quant& progressive_quant, const video::RateControl& h264_rate,
                                   bool defer_chroma)
{
    progressive_quant_ = progressive_quant;
    h264_rate_ = h264_rate;
    avc444_policy_.defer_chroma = defer_chroma;
    for (const auto& s : screens_) {
        if (s->avc444) {
            s->avc444->set_policy(avc444_policy_);
        }
        if (s->progressive) {
            s->progressive->set_quant(progressive_quant);
        }
        const auto retune = [&](video::H264Encoder* h264) {
            if (h264 != nullptr && h264->config().rate != h264_rate) {
                if (auto set = h264->set_rate_control(h264_rate); !set) {
                    log::warn(log_component, "cannot change the H.264 rate control: {}", set.error().message());
                }
            }
        };
        retune(s->avc444 ? &s->avc444->h264() : s->h264.get());
        retune(s->video.get());
    }
}

void GraphicsPipeline::close(std::string reason)
{
    if (closed_) {
        return;
    }
    log::warn(log_component, "graphics pipeline closed: {}", reason);
    closed_ = true;
    laid_out_ = false;
    channels_->close(channel_id_);
    events_.emplace_back(pipeline_event::Closed{std::move(reason)});
}

void GraphicsPipeline::begin_frame(std::uint32_t timestamp)
{
    FARLAND_ASSERT(ready());
    FARLAND_ASSERT(!frame_id_);
    frame_timestamp_ = timestamp;
    for (const auto& s : screens_) {
        s->in_frame = false;
    }
}

void GraphicsPipeline::ensure_frame()
{
    if (!frame_id_) {
        frame_id_ = gfx_.start_frame(frame_timestamp_);
    }
}

std::optional<std::uint32_t> GraphicsPipeline::end_frame()
{
    // A screen nothing was added for got no refinement from add_frame(): a
    // desktop that stands still delivers no frames at all, and its tiles
    // would never leave the quality of their first pass.
    if (!closed_) {
        for (const auto& s : screens_) {
            if (!s->in_frame) {
                refine_still_screen(*s);
            }
        }
    }
    const auto id = std::exchange(frame_id_, std::nullopt);
    if (!id || closed_) {
        return std::nullopt;
    }
    gfx_.end_frame();
    flush();
    return id;
}

std::optional<std::uint32_t> GraphicsPipeline::send_frame(const codec::ImageView& frame, std::uint32_t timestamp)
{
    begin_frame(timestamp);
    if (!screens_.empty()) {
        add_frame(0, frame);
    }
    return end_frame();
}

Result<std::optional<std::uint32_t>> GraphicsPipeline::send_dmabuf_frame(const video::DmabufFrame& frame,
                                                                         std::span<const PixelRect> damage,
                                                                         std::uint32_t timestamp)
{
    begin_frame(timestamp);
    if (screens_.empty()) {
        return end_frame();
    }
    const auto sent = add_dmabuf_frame(0, frame, damage);
    const auto id = end_frame();  // nothing went into the frame on an error
    if (!sent) {
        return std::unexpected(sent.error());
    }
    return id;
}

void GraphicsPipeline::add_frame(std::size_t index, const codec::ImageView& frame)
{
    FARLAND_ASSERT(ready() && index < screens_.size());
    Screen& s = *screens_[index];
    FARLAND_ASSERT(frame.width == s.width && frame.height == s.height);
    s.in_frame = true;
    if (s.previous_stale) {
        // Dmabuf pictures went out that `previous` never saw: diffing against
        // it could miss a change back to what it holds.
        std::ranges::fill(s.dirty, true);
        s.previous_stale = false;
    }
    const auto changed_tiles = [&] {
        TileList changed;
        for (std::uint32_t ty = 0; ty < s.tiles_y; ++ty) {
            for (std::uint32_t tx = 0; tx < s.tiles_x; ++tx) {
                if (s.dirty[(std::size_t{ty} * s.tiles_x) + tx] || tile_changed(s, frame, tx, ty)) {
                    changed.emplace_back(tx, ty);
                }
            }
        }
        return changed;
    };
    TileList tiles = changed_tiles();
    if (s.avc444) {
        // Even without damage when chroma is still owed (LC 2).
        if (!tiles.empty() || s.avc444->chroma_pending()) {
            send_avc444_frame(s, frame, tiles);
        }
        return;
    }
    if (s.h264) {
        // An AVC420 surface has neither refinement nor a lossless pass, so
        // without damage there is nothing to encode (and no region to list).
        if (!tiles.empty()) {
            send_h264_frame(s, frame, tiles);
        }
        return;
    }
    // The per-tile counters age once per frame, before anything decides to
    // skip the frame: a tile that stops moving has to cool down even while
    // nothing else happens, or the H.264 regions would never be given back.
    age_tiles(s, tiles);
    if (reclaim_video_tiles(s, false)) {
        tiles = changed_tiles();
    }
    // Without changes a frame still goes out while Progressive tiles wait
    // for refinement or for their lossless copy.
    const bool refining = s.progressive.has_value() && s.progressive->pending_tiles() > 0;
    if (tiles.empty() && !refining && !exact_pending(s)) {
        return;
    }
    ensure_frame();
    // A scroll: the client moves what it already has; what is left to encode
    // is what the move did not cover.
    if (move_scrolled(s, frame, tiles)) {
        tiles = changed_tiles();
    }
    if (s.progressive) {
        // Every changed tile goes to the codec its behaviour asks for: tiles
        // that keep changing and hold many colours (a video, an animation) to
        // H.264; runs of adjacent few-colour tiles in a row (text, UI) to one
        // ClearCodec region each; the rest to RFX_PROGRESSIVE streams of at
        // most 16 KB, every one its own WireToSurface2 (docs/PLAN.md §4.1).
        const auto at = [&](std::uint32_t tx, std::uint32_t ty) { return (std::size_t{ty} * s.tiles_x) + tx; };
        // Counting a tile's colours walks its pixels, so it is done once per
        // changed tile and kept, indexed like `tiles`.
        std::vector<char> few(tiles.size(), 0);
        const bool classify = s.clear.has_value() || (options_.video_regions && !s.video_unavailable);
        if (classify) {
            for (std::size_t i = 0; i < tiles.size(); ++i) {
                few[i] = static_cast<char>(few_colours(s, frame, tiles[i].first, tiles[i].second));
            }
        }
        const TileList moving = video_tiles(s, tiles, few);
        std::vector<bool> to_video(s.dirty.size(), false);
        for (const auto& [tx, ty] : moving) {
            to_video[at(tx, ty)] = true;
        }
        std::vector<codec::progressive::Rect> clear_area;
        std::vector<codec::progressive::Rect> direct;
        std::vector<codec::progressive::Rect> ladder;
        const auto for_clear = [&](std::size_t i) {
            return s.clear.has_value() && few[i] != 0 && !to_video[at(tiles[i].first, tiles[i].second)];
        };
        const bool small = tiles.size() <= std::size_t{options_.direct_tiles};
        for (std::size_t i = 0; i < tiles.size();) {
            const auto [tx, ty] = tiles[i];
            if (to_video[at(tx, ty)]) {
                ++i;
                continue;
            }
            if (!for_clear(i)) {
                // Small damage goes out sharp at once; a tile that keeps
                // changing takes the coarse-first ladder, which costs less
                // per frame, and refinement catches up when it settles.
                const bool hot = s.motion[at(tx, ty)] >= motion_hot;
                (small && !hot ? direct : ladder).push_back(tile_rect(s, tx, ty));
                ++i;
                continue;
            }
            std::size_t stop = i + 1;
            while (stop < tiles.size() && tiles[stop].second == ty && tiles[stop].first == tiles[stop - 1].first + 1 &&
                   for_clear(stop)) {
                ++stop;
            }
            const auto first = tile_rect(s, tx, ty);
            const auto last = tile_rect(s, tiles[stop - 1].first, ty);
            clear_area.push_back({first.x, first.y, last.x + last.width - first.x, first.height});
            i = stop;
        }
        std::size_t bytes = send_video_tiles(s, frame, moving);
        if (!clear_area.empty()) {
            // These pixels come from ClearCodec now: no refinement may paint over them.
            s.progressive->discard(clear_area);
            for (const auto& area : clear_area) {
                bytes += send_clear(s, frame, area);
            }
        }
        const auto send = [&](std::span<const codec::progressive::Rect> area, codec::progressive::Encoder::Pass pass) {
            for (const auto& stream : s.progressive->encode(frame, area, pass)) {
                bytes += stream.size();
                gfx_.wire_to_surface_2(s.surface, gfx::codec::progressive, progressive_context,
                                       gfx::pixel_format::xrgb_8888, stream);
            }
        };
        send(direct, codec::progressive::Encoder::Pass::direct);
        send(ladder, codec::progressive::Encoder::Pass::refined);
        bytes += send_upgrades(s, tiles, bytes);
        // ClearCodec, planar and the direct pass leave the client the exact
        // pixels; the ladder leaves it a tile that still owes one.
        for (const auto& [tx, ty] : tiles) {
            s.exact[at(tx, ty)] = false;
        }
        const auto mark = [&](std::span<const codec::progressive::Rect> area) {
            for (const auto& r : area) {
                for (std::uint32_t ty = r.y / tile_size; ty * tile_size < r.y + r.height; ++ty) {
                    for (std::uint32_t tx = r.x / tile_size; tx * tile_size < r.x + r.width; ++tx) {
                        s.exact[at(tx, ty)] = true;
                    }
                }
            }
        };
        mark(clear_area);
        if (options_.lossless_still && bytes < options_.upgrade_budget + options_.lossless_budget) {
            send_exact(s, frame, options_.upgrade_budget + options_.lossless_budget - bytes);
        }
    } else {
        for (const auto& [tx, ty] : tiles) {
            static_cast<void>(send_planar_tile(s, frame, tx, ty));
            s.exact[(std::size_t{ty} * s.tiles_x) + tx] = true;
        }
    }
    for (const auto& [tx, ty] : tiles) {
        remember_tile(s, frame, tx, ty);
    }
}

bool GraphicsPipeline::tile_changed(const Screen& s, const codec::ImageView& frame, std::uint32_t tx, std::uint32_t ty)
{
    const std::uint32_t x = tx * tile_size;
    const std::uint32_t y = ty * tile_size;
    const std::size_t row_bytes = std::size_t{std::min<std::uint32_t>(tile_size, s.width - x)} * bytes_per_pixel;
    const std::uint32_t rows = std::min<std::uint32_t>(tile_size, s.height - y);
    const std::span previous(s.previous);
    for (std::uint32_t row = y; row < y + rows; ++row) {
        const auto current = frame.data.subspan((row * frame.stride) + (std::size_t{x} * bytes_per_pixel), row_bytes);
        const auto before = previous.subspan(((std::size_t{row} * s.width) + x) * bytes_per_pixel, row_bytes);
        if (!std::ranges::equal(current, before)) {
            return true;
        }
    }
    return false;
}

void GraphicsPipeline::send_h264_frame(Screen& s, const codec::ImageView& frame, const TileList& tiles)
{
    if (!s.yuv) {
        return;  // not reached: an encoder always comes with its picture
    }
    codec::bgrx_to_yuv420(frame, *s.yuv);
    const auto encoded = s.h264->encode(s.yuv->view());
    if (!encoded) {
        close("H.264 encoder failed: " + encoded.error().message());
        return;
    }
    if (send_avc420(s, *encoded, tiles)) {
        for (const auto& [tx, ty] : tiles) {
            remember_tile(s, frame, tx, ty);
        }
    }
}

Result<bool> GraphicsPipeline::add_dmabuf_frame(std::size_t index, const video::DmabufFrame& frame,
                                                std::span<const PixelRect> damage)
{
    FARLAND_ASSERT(ready() && accepts_dmabuf(index));
    Screen& s = *screens_[index];
    FARLAND_ASSERT(frame.width == s.width && frame.height == s.height);
    s.in_frame = true;
    // The damage marks tiles like an invalidation: they stay marked until a
    // frame carries them, whichever path it takes.
    for (const PixelRect& rect : damage) {
        if (rect.width == 0 || rect.height == 0 || rect.x >= s.width || rect.y >= s.height) {
            continue;
        }
        const std::uint32_t right = std::min<std::uint32_t>(s.width, rect.x + std::min(rect.width, 0xFFFFU));
        const std::uint32_t bottom = std::min<std::uint32_t>(s.height, rect.y + std::min(rect.height, 0xFFFFU));
        for (std::uint32_t ty = rect.y / tile_size; ty <= (bottom - 1) / tile_size; ++ty) {
            for (std::uint32_t tx = rect.x / tile_size; tx <= (right - 1) / tile_size; ++tx) {
                s.dirty[(std::size_t{ty} * s.tiles_x) + tx] = true;
            }
        }
    }
    const TileList tiles = dirty_tiles(s);
    if (tiles.empty()) {
        return false;
    }
    auto encoded = s.h264->encode_dmabuf(frame);
    if (!encoded) {
        if (encoded.error().code == Errc::unsupported) {
            return std::unexpected(encoded.error());
        }
        close("H.264 encoder failed: " + encoded.error().message());
        return false;
    }
    if (!send_avc420(s, *encoded, tiles)) {
        return false;
    }
    for (const auto& [tx, ty] : tiles) {
        s.dirty[(std::size_t{ty} * s.tiles_x) + tx] = false;
    }
    s.previous_stale = true;
    return true;
}

void GraphicsPipeline::forget_dmabufs(std::size_t index) noexcept
{
    if (index < screens_.size() && screens_[index]->h264) {
        screens_[index]->h264->forget_dmabufs();
    }
}

void GraphicsPipeline::forget_dmabufs() noexcept
{
    for (std::size_t i = 0; i < screens_.size(); ++i) {
        forget_dmabufs(i);
    }
}

bool GraphicsPipeline::send_avc420(Screen& s, const video::EncodedFrame& encoded, const TileList& tiles)
{
    if (encoded.bitstream.empty()) {
        return false;  // the encoder dropped the picture; the damage stays for the next frame
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
        add({0, 0, s.width, s.height});
    } else {
        for (const auto& [tx, ty] : tiles) {
            add(tile_rect(s, tx, ty));
        }
    }
    gfx::Rect16 bounds{s.width, s.height, 0, 0};
    for (const auto& region : regions) {
        bounds.left = std::min(bounds.left, region.rect.left);
        bounds.top = std::min(bounds.top, region.rect.top);
        bounds.right = std::max(bounds.right, region.rect.right);
        bounds.bottom = std::max(bounds.bottom, region.rect.bottom);
    }
    ensure_frame();
    gfx_.wire_to_surface_1(s.surface, gfx::codec::avc420, gfx::pixel_format::xrgb_8888, bounds,
                           codec::avc::encode_avc420(regions, encoded.bitstream));
    return true;
}

GraphicsPipeline::TileList GraphicsPipeline::dirty_tiles(const Screen& s)
{
    TileList tiles;
    for (std::uint32_t ty = 0; ty < s.tiles_y; ++ty) {
        for (std::uint32_t tx = 0; tx < s.tiles_x; ++tx) {
            if (s.dirty[(std::size_t{ty} * s.tiles_x) + tx]) {
                tiles.emplace_back(tx, ty);
            }
        }
    }
    return tiles;
}

void GraphicsPipeline::send_avc444_frame(Screen& s, const codec::ImageView& frame, const TileList& tiles)
{
    if (!s.avc444) {
        return;  // not reached: add_frame() only calls it with an AVC444 encoder
    }
    std::vector<codec::avc::Rect16> damage;
    damage.reserve(tiles.size());
    for (const auto& [tx, ty] : tiles) {
        const auto r = tile_rect(s, tx, ty);
        damage.push_back({static_cast<std::uint16_t>(r.x), static_cast<std::uint16_t>(r.y),
                          static_cast<std::uint16_t>(r.x + r.width), static_cast<std::uint16_t>(r.y + r.height)});
    }
    const auto encoded = s.avc444->encode(frame, damage);
    if (!encoded) {
        close("H.264 encoder failed: " + encoded.error().message());
        return;
    }
    const auto& picture = *encoded;
    if (!picture) {
        return;  // the encoder dropped the picture; the damage stays for the next frame
    }
    const video::Avc444Frame& out = *picture;
    const std::uint16_t codec_id =
        s.avc444->version() == codec::Avc444Version::v2 ? gfx::codec::avc444v2 : gfx::codec::avc444;
    ensure_frame();
    gfx_.wire_to_surface_1(
        s.surface, codec_id, gfx::pixel_format::xrgb_8888,
        gfx::Rect16{out.dest_rect.left, out.dest_rect.top, out.dest_rect.right, out.dest_rect.bottom},
        out.bitmap_stream);
    for (const auto& [tx, ty] : tiles) {
        remember_tile(s, frame, tx, ty);
    }
}

bool GraphicsPipeline::move_scrolled(Screen& s, const codec::ImageView& frame, const TileList& tiles)
{
    // The client's surface equals `previous` only while no tile is
    // invalidated; moving anything else would move pixels it does not have.
    if (tiles.size() < min_scroll_tiles || std::ranges::any_of(s.dirty, std::identity{})) {
        return false;
    }
    std::uint32_t tx0 = s.tiles_x;
    std::uint32_t ty0 = s.tiles_y;
    std::uint32_t tx1 = 0;
    std::uint32_t ty1 = 0;
    for (const auto& [tx, ty] : tiles) {
        tx0 = std::min(tx0, tx);
        ty0 = std::min(ty0, ty);
        tx1 = std::max(tx1, tx + 1);
        ty1 = std::max(ty1, ty + 1);
    }
    const auto first = tile_rect(s, tx0, ty0);
    const auto last = tile_rect(s, tx1 - 1, ty1 - 1);
    const PixelRect area{first.x, first.y, last.x + last.width - first.x, last.y + last.height - first.y};
    const codec::ImageView before{s.previous, s.width, s.height, std::size_t{s.width} * bytes_per_pixel};
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
    gfx_.surface_to_surface(s.surface, s.surface, source_rect, points);
    apply_scroll(s.previous, std::size_t{s.width} * bytes_per_pixel, *move);
    if (s.progressive) {
        // The client's refinement state of the destination no longer matches
        // its pixels.
        const std::array moved{
            codec::progressive::Rect{destination.x, destination.y, destination.width, destination.height}};
        s.progressive->discard(moved);
    }
    return true;
}

bool GraphicsPipeline::few_colours(const Screen& s, const codec::ImageView& frame, std::uint32_t tx,
                                   std::uint32_t ty) const
{
    // Open addressing on the BGR value, stopping at the first colour too many.
    constexpr std::size_t slots = 128;             // a power of two, twice the most colours counted
    constexpr std::uint32_t empty = 0xFFFF'FFFFU;  // no BGR value
    const std::uint32_t limit = std::min(options_.max_clear_colours, std::uint32_t{slots / 2});
    std::array<std::uint32_t, slots> table{};
    table.fill(empty);
    std::uint32_t count = 0;
    std::uint32_t last = empty;
    const auto r = tile_rect(s, tx, ty);
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

std::size_t GraphicsPipeline::send_clear(Screen& s, const codec::ImageView& frame, const codec::progressive::Rect& area)
{
    if (!s.clear) {
        return 0;  // not reached: add_frame() only calls it with an encoder
    }
    const std::size_t offset = (area.y * frame.stride) + (std::size_t{area.x} * bytes_per_pixel);
    const codec::ImageView region{
        frame.data.subspan(offset, ((area.height - 1) * frame.stride) + (std::size_t{area.width} * bytes_per_pixel)),
        area.width, area.height, frame.stride};
    const auto stream = s.clear->encode(region);
    gfx_.wire_to_surface_1(s.surface, gfx::codec::clearcodec, gfx::pixel_format::xrgb_8888, to_rect16(area), stream);
    return stream.size();
}

void GraphicsPipeline::refine_still_screen(Screen& s)
{
    if (s.avc444) {
        // The chroma view is encoded from the picture the luma came from, and
        // no other: `previous` still holds it, tile for tile, because every
        // tile that went out was remembered. A stale `previous` is not that
        // picture, so the chroma waits for the next real frame instead.
        if (s.avc444->chroma_pending() && !s.previous_stale) {
            const codec::ImageView last{s.previous, s.width, s.height, std::size_t{s.width} * bytes_per_pixel};
            send_avc444_frame(s, last, {});
        }
        return;
    }
    if (!s.progressive) {
        return;
    }
    // Nothing was captured for this screen, so nothing changed on it: the
    // tiles grow older, and what the client has is exactly `previous`.
    age_tiles(s, {});
    const codec::ImageView last{s.previous, s.width, s.height, std::size_t{s.width} * bytes_per_pixel};
    static_cast<void>(send_video_tiles(s, last, {}));  // ages the idle counter
    static_cast<void>(reclaim_video_tiles(s, false));
    const std::size_t used = send_upgrades(s, {}, 0);
    if (options_.lossless_still && !s.previous_stale && used < options_.lossless_budget) {
        send_exact(s, last, options_.lossless_budget - used);
    }
}

std::size_t GraphicsPipeline::send_upgrades(Screen& s, const TileList& changed, std::size_t used)
{
    if (!s.progressive || s.progressive->pending_tiles() == 0 || used >= options_.upgrade_budget) {
        return 0;
    }
    const std::size_t budget = options_.upgrade_budget - used;
    std::vector<std::vector<std::byte>> streams;
    if (changed.empty()) {
        streams = s.progressive->upgrade(budget);
    } else {
        // What changed in this frame just started over; refine the rest.
        std::vector<bool> skip(s.dirty.size());
        for (const auto& [tx, ty] : changed) {
            skip[(std::size_t{ty} * s.tiles_x) + tx] = true;
        }
        std::vector<codec::progressive::Rect> area;
        for (std::uint32_t ty = 0; ty < s.tiles_y; ++ty) {
            for (std::uint32_t tx = 0; tx < s.tiles_x; ++tx) {
                if (!skip[(std::size_t{ty} * s.tiles_x) + tx]) {
                    area.push_back(tile_rect(s, tx, ty));
                }
            }
        }
        streams = s.progressive->upgrade(area, budget);
    }
    std::size_t bytes = 0;
    for (const auto& stream : streams) {
        ensure_frame();  // refinement alone opens the frame too
        bytes += stream.size();
        gfx_.wire_to_surface_2(s.surface, gfx::codec::progressive, progressive_context, gfx::pixel_format::xrgb_8888,
                               stream);
    }
    return bytes;
}

void GraphicsPipeline::age_tiles(Screen& s, const TileList& changed)
{
    std::vector<bool> moved(s.motion.size(), false);
    for (const auto& [tx, ty] : changed) {
        moved[(std::size_t{ty} * s.tiles_x) + tx] = true;
    }
    for (std::size_t i = 0; i < s.motion.size(); ++i) {
        const auto rise = static_cast<std::uint8_t>(std::min<unsigned>(motion_max, s.motion[i] + motion_rise));
        if (moved[i]) {
            s.motion[i] = rise;
            s.still[i] = 0;
        } else {
            s.motion[i] =
                s.motion[i] > motion_fall ? static_cast<std::uint8_t>(s.motion[i] - motion_fall) : std::uint8_t{0};
            s.still[i] = static_cast<std::uint8_t>(std::min<unsigned>(still_max, s.still[i] + 1U));
        }
    }
}

bool GraphicsPipeline::ensure_video_encoder(Screen& s)
{
    if (s.video) {
        return true;
    }
    if (s.video_unavailable || !make_h264_) {
        return false;
    }
    const auto& negotiated = gfx_.negotiated();
    if (!negotiated || !negotiated->allows(gfx::codec::avc420)) {
        s.video_unavailable = true;
        return false;
    }
    video::EncoderConfig config;
    config.width = codec::avc::coded_size(s.width);
    config.height = codec::avc::coded_size(s.height);
    if (h264_rate_) {
        config.rate = *h264_rate_;
    }
    auto encoder = make_h264_(config);
    if (!encoder) {
        log::info(log_component, "no H.264 encoder for video regions ({}), staying on Progressive",
                  encoder.error().message());
        s.video_unavailable = true;
        return false;
    }
    s.video = std::move(*encoder);
    s.video_yuv.emplace(config.width, config.height);
    log::info(log_component, "video regions of surface {} go through H.264 ({})", s.surface,
              video::to_string(s.video->backend()));
    return true;
}

GraphicsPipeline::TileList GraphicsPipeline::video_tiles(Screen& s, const TileList& changed,
                                                         std::span<const char> few) const
{
    if (!options_.video_regions || !s.progressive || s.video_unavailable) {
        return {};
    }
    TileList moving;
    for (std::size_t i = 0; i < changed.size(); ++i) {
        const auto [tx, ty] = changed[i];
        if (s.motion[(std::size_t{ty} * s.tiles_x) + tx] >= motion_hot && few[i] == 0) {
            moving.emplace_back(tx, ty);
        }
    }
    // Below a handful of tiles the direct full-quality pass is both sharper
    // and smaller than a whole H.264 picture; an encoder already running
    // keeps its tiles, so a shrinking video does not flip back and forth.
    if (moving.size() < options_.min_video_tiles && !s.video) {
        return {};
    }
    return moving;
}

std::size_t GraphicsPipeline::send_video_tiles(Screen& s, const codec::ImageView& frame, const TileList& tiles)
{
    if (tiles.empty()) {
        // The video stopped: drop the encoder after a while and let
        // Progressive and ClearCodec take those tiles back, sharp.
        if (s.video && ++s.video_idle >= options_.video_idle_frames) {
            log::info(log_component, "surface {}: no video regions left, back to Progressive", s.surface);
            s.video.reset();
            s.video_yuv.reset();
            s.video_idle = 0;
            reclaim_video_tiles(s, true);
        }
        return 0;
    }
    s.video_idle = 0;
    if (!ensure_video_encoder(s)) {
        return 0;
    }
    codec::bgrx_to_yuv420(frame, *s.video_yuv);
    const auto encoded = s.video->encode(s.video_yuv->view());
    if (!encoded) {
        log::warn(log_component, "H.264 video region failed ({}), staying on Progressive", encoded.error().message());
        s.video.reset();
        s.video_yuv.reset();
        s.video_unavailable = true;
        return 0;
    }
    if (encoded->bitstream.empty()) {
        return 0;  // the encoder dropped the picture; the tiles stay changed
    }
    // The picture covers the surface, but only these regions are copied, so
    // ClearCodec and Progressive pixels elsewhere stay untouched -- an IDR
    // included ([MS-RDPEGFX] 2.2.4.4: the client copies regionRects only).
    const codec::avc::QuantQuality quant{
        .qp = encoded->qp, .progressive = false, .quality = codec::avc::quality_from_qp(encoded->qp)};
    std::vector<codec::avc::Region> regions;
    regions.reserve(tiles.size());
    std::vector<codec::progressive::Rect> covered;
    covered.reserve(tiles.size());
    gfx::Rect16 bounds{s.width, s.height, 0, 0};
    for (const auto& [tx, ty] : tiles) {
        const auto r = tile_rect(s, tx, ty);
        const codec::avc::Rect16 rect{static_cast<std::uint16_t>(r.x), static_cast<std::uint16_t>(r.y),
                                      static_cast<std::uint16_t>(r.x + r.width),
                                      static_cast<std::uint16_t>(r.y + r.height)};
        regions.push_back({rect, quant});
        covered.push_back(r);
        s.from_video[(std::size_t{ty} * s.tiles_x) + tx] = true;
        bounds.left = std::min(bounds.left, rect.left);
        bounds.top = std::min(bounds.top, rect.top);
        bounds.right = std::max(bounds.right, rect.right);
        bounds.bottom = std::max(bounds.bottom, rect.bottom);
    }
    // H.264 paints these pixels now: a pending upgrade would paint over them.
    s.progressive->discard(covered);
    const auto stream = codec::avc::encode_avc420(regions, encoded->bitstream);
    ensure_frame();
    gfx_.wire_to_surface_1(s.surface, gfx::codec::avc420, gfx::pixel_format::xrgb_8888, bounds, stream);
    return stream.size();
}

bool GraphicsPipeline::reclaim_video_tiles(Screen& s, bool all)
{
    bool any = false;
    for (std::size_t i = 0; i < s.from_video.size(); ++i) {
        if (s.from_video[i] && (all || s.motion[i] < motion_hot)) {
            s.from_video[i] = false;
            s.dirty[i] = true;  // H.264 pixels: send them again, sharp
            s.exact[i] = false;
            any = true;
        }
    }
    return any;
}

void GraphicsPipeline::send_exact(Screen& s, const codec::ImageView& frame, std::size_t budget)
{
    if (!s.progressive || budget == 0) {
        return;
    }
    const std::size_t n = s.exact.size();
    std::size_t spent = 0;
    for (std::size_t k = 0; k < n && spent < budget; ++k) {
        const std::size_t i = (s.exact_cursor + k) % n;
        if (s.exact[i] || s.dirty[i] || s.motion[i] >= motion_hot ||
            std::uint32_t{s.still[i]} < options_.still_frames) {
            continue;
        }
        const auto tx = static_cast<std::uint32_t>(i % s.tiles_x);
        const auto ty = static_cast<std::uint32_t>(i / s.tiles_x);
        if (s.progressive->tile_stage(tx, ty) != codec::progressive::full_quality_stage) {
            continue;  // still climbing the ladder; upgrades come first
        }
        const auto r = tile_rect(s, tx, ty);
        const std::array area{r};
        // The client's pixels come from planar now, not from the Progressive
        // tile state, so no upgrade may follow for it.
        s.progressive->discard(area);
        ensure_frame();
        spent += send_planar_tile(s, frame, tx, ty);
        s.exact[i] = true;
        s.exact_cursor = (i + 1) % n;
    }
}

codec::progressive::Rect GraphicsPipeline::tile_rect(const Screen& s, std::uint32_t tx, std::uint32_t ty) noexcept
{
    const std::uint32_t x = tx * tile_size;
    const std::uint32_t y = ty * tile_size;
    return {x, y, std::min<std::uint32_t>(tile_size, s.width - x), std::min<std::uint32_t>(tile_size, s.height - y)};
}

std::size_t GraphicsPipeline::send_planar_tile(Screen& s, const codec::ImageView& frame, std::uint32_t tx,
                                               std::uint32_t ty)
{
    const auto r = tile_rect(s, tx, ty);
    const std::size_t offset = (r.y * frame.stride) + (std::size_t{r.x} * bytes_per_pixel);
    const codec::ImageView tile{
        frame.data.subspan(offset, ((r.height - 1) * frame.stride) + (std::size_t{r.width} * bytes_per_pixel)), r.width,
        r.height, frame.stride};

    // RDPGFX planar is top-down ([MS-RDPEGFX] 2.2.4.3; FreeRDP decodes it without vFlip).
    const auto data =
        codec::planar::encode(tile, {codec::planar::Mode::automatic, codec::planar::Orientation::top_down});
    gfx_.wire_to_surface_1(s.surface, gfx::codec::planar, gfx::pixel_format::xrgb_8888, to_rect16(r), data);
    return data.size();
}

void GraphicsPipeline::remember_tile(Screen& s, const codec::ImageView& frame, std::uint32_t tx, std::uint32_t ty)
{
    const auto r = tile_rect(s, tx, ty);
    const std::size_t row_bytes = std::size_t{r.width} * bytes_per_pixel;
    const std::span previous(s.previous);
    for (std::uint32_t row = r.y; row < r.y + r.height; ++row) {
        const auto source = frame.data.subspan((row * frame.stride) + (std::size_t{r.x} * bytes_per_pixel), row_bytes);
        std::ranges::copy(source, previous.subspan(((std::size_t{row} * s.width) + r.x) * bytes_per_pixel).begin());
    }
    s.dirty[(std::size_t{ty} * s.tiles_x) + tx] = false;
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
