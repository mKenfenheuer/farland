// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/base/log.hpp>
#include <farland/codec/avc420.hpp>
#include <farland/codec/planar.hpp>
#include <farland/server/graphics_pipeline.hpp>

#include <algorithm>

namespace farland::server {

namespace {

namespace gfx = channels::rdpgfx;
namespace dvc = channels::dvc_event;
constexpr std::string_view log_component = "server.gfx";
constexpr std::size_t bytes_per_pixel = 4;

}  // namespace

GraphicsPipeline::GraphicsPipeline(DynamicChannels& channels, std::uint16_t width, std::uint16_t height,
                                   channels::rdpgfx::GfxServerConfig config, TileCodec codec, H264Factory make_h264)
    : channels_(&channels), gfx_(config), width_(width), height_(height), tiles_x_((width + tile_size - 1) / tile_size),
      tiles_y_((height + tile_size - 1) / tile_size), requested_codec_(codec), make_h264_(std::move(make_h264)),
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
    if (requested_codec_ == TileCodec::avc420 && negotiated && negotiated->allows(gfx::codec::avc420) && make_h264_) {
        video::EncoderConfig config;
        config.width = codec::avc::coded_size(width_);
        config.height = codec::avc::coded_size(height_);
        if (auto encoder = make_h264_(config)) {
            h264_ = std::move(*encoder);
            yuv_.emplace(config.width, config.height);
        } else {
            log::warn(log_component, "no H.264 encoder ({}), using Progressive", encoder.error().message());
        }
    }
    if (!h264_ && requested_codec_ != TileCodec::planar && negotiated && negotiated->allows(gfx::codec::progressive)) {
        progressive_.emplace(width_, height_);
    }
    invalidate_all();
}

void GraphicsPipeline::invalidate_all()
{
    std::ranges::fill(dirty_, true);
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
    const std::uint16_t surface = *surface_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> tiles;
    for (std::uint32_t ty = 0; ty < tiles_y_; ++ty) {
        for (std::uint32_t tx = 0; tx < tiles_x_; ++tx) {
            if (dirty_[(std::size_t{ty} * tiles_x_) + tx] || tile_changed(frame, tx, ty)) {
                tiles.emplace_back(tx, ty);
            }
        }
    }
    if (tiles.empty()) {
        return std::nullopt;
    }
    if (h264_) {
        return send_h264_frame(frame, surface, tiles, timestamp);
    }
    const std::uint32_t frame_id = gfx_.start_frame(timestamp);
    if (progressive_) {
        // One or more RFX_PROGRESSIVE streams of at most 16 KB each, every one
        // its own WireToSurface2 (docs/PLAN.md §4.1).
        std::vector<codec::progressive::Rect> damage;
        damage.reserve(tiles.size());
        for (const auto& [tx, ty] : tiles) {
            damage.push_back(tile_rect(tx, ty));
        }
        for (const auto& stream : progressive_->encode(frame, damage)) {
            gfx_.wire_to_surface_2(surface, gfx::codec::progressive, progressive_context, gfx::pixel_format::xrgb_8888,
                                   stream);
        }
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

std::optional<std::uint32_t>
GraphicsPipeline::send_h264_frame(const codec::ImageView& frame, std::uint16_t surface,
                                  const std::vector<std::pair<std::uint32_t, std::uint32_t>>& tiles,
                                  std::uint32_t timestamp)
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
    if (encoded->bitstream.empty()) {
        return std::nullopt;  // the encoder dropped the picture; the damage stays for the next frame
    }
    // The client decodes the whole picture but only copies the regions to the
    // surface, so an IDR lists everything and later frames what changed.
    const codec::avc::QuantQuality quant{
        .qp = encoded->qp, .progressive = false, .quality = codec::avc::quality_from_qp(encoded->qp)};
    std::vector<codec::avc::Region> regions;
    const auto add = [&](const codec::progressive::Rect& r) {
        regions.push_back({{static_cast<std::uint16_t>(r.x), static_cast<std::uint16_t>(r.y),
                            static_cast<std::uint16_t>(r.x + r.width), static_cast<std::uint16_t>(r.y + r.height)},
                           quant});
    };
    if (encoded->idr) {
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
                           codec::avc::encode_avc420(regions, encoded->bitstream));
    for (const auto& [tx, ty] : tiles) {
        remember_tile(frame, tx, ty);
    }
    gfx_.end_frame();
    flush();
    return frame_id;
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
