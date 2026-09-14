// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/video/avc444_encoder.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace farland::video {

namespace {

using codec::avc::Rect16;

[[nodiscard]] std::uint8_t at(std::span<const std::byte> bytes, std::size_t index)
{
    return std::to_integer<std::uint8_t>(bytes[index]);
}

[[nodiscard]] bool intersects(const Rect16& a, const Rect16& b) noexcept
{
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

/// Largest difference between a chroma sample of two YUV444 rows and the
/// main view's average of its 2x2 block.
[[nodiscard]] unsigned worst_deviation(std::span<const std::byte> row0, std::span<const std::byte> row1,
                                       std::span<const std::byte> averages)
{
    unsigned worst = 0;
    for (std::size_t x = 0; x < averages.size(); ++x) {
        const int m = at(averages, x);
        for (const int sample : {at(row0, 2 * x), at(row0, (2 * x) + 1U), at(row1, 2 * x), at(row1, (2 * x) + 1U)}) {
            worst = std::max(worst, static_cast<unsigned>(std::abs(sample - m)));
        }
    }
    return worst;
}

}  // namespace

Avc444Encoder::Avc444Encoder(std::unique_ptr<H264Encoder> encoder, std::uint16_t width, std::uint16_t height,
                             codec::Avc444Version version, Avc444Policy policy)
    : encoder_(std::move(encoder)), width_(width), height_(height), version_(version), policy_(policy),
      tiles_x_((width + tile_size - 1U) / tile_size), tiles_y_((height + tile_size - 1U) / tile_size),
      yuv_(codec::avc::coded_size(width), codec::avc::coded_size(height)),
      main_(codec::avc::coded_size(width), codec::avc::coded_size(height)),
      aux_(codec::avc::coded_size(width), codec::avc::coded_size(height)), pending_(std::size_t{tiles_x_} * tiles_y_)
{
    FARLAND_ASSERT(encoder_ != nullptr && width > 0 && height > 0);
    FARLAND_ASSERT(encoder_->config().width == codec::avc::coded_size(width));
    FARLAND_ASSERT(encoder_->config().height == codec::avc::coded_size(height));
}

bool Avc444Encoder::chroma_pending() const noexcept
{
    return std::ranges::any_of(pending_, [](const Pending& p) { return p.has_value(); });
}

Rect16 Avc444Encoder::tile_rect(std::size_t tile) const noexcept
{
    const auto left = static_cast<std::uint32_t>((tile % tiles_x_) * tile_size);
    const auto top = static_cast<std::uint32_t>((tile / tiles_x_) * tile_size);
    return {static_cast<std::uint16_t>(left), static_cast<std::uint16_t>(top),
            static_cast<std::uint16_t>(std::min<std::uint32_t>(left + tile_size, width_)),
            static_cast<std::uint16_t>(std::min<std::uint32_t>(top + tile_size, height_))};
}

bool Avc444Encoder::needs_chroma(std::size_t tile) const
{
    const Rect16 r = tile_rect(tile);
    const auto yuv = yuv_.view();
    const auto main = main_.view();
    // Whole 2x2 blocks: the picture sides are even and cover the surface.
    const std::size_t left = r.left;
    const std::size_t right = (std::size_t{r.right} + 1U) & ~std::size_t{1};
    const std::size_t bottom = (std::size_t{r.bottom} + 1U) & ~std::size_t{1};
    for (std::size_t y = r.top; y < bottom; y += 2) {
        const std::size_t line = y * yuv.stride;
        const std::size_t chroma = ((y / 2U) * main.uv_stride) + (left / 2U);
        for (const auto& [full, half] : {std::pair{yuv.u, main.u}, std::pair{yuv.v, main.v}}) {
            const unsigned worst = worst_deviation(full.subspan(line + left, right - left),
                                                   full.subspan(line + yuv.stride + left, right - left),
                                                   half.subspan(chroma, (right - left) / 2U));
            if (worst > policy_.chroma_tolerance) {
                return true;
            }
        }
    }
    return false;
}

std::vector<codec::avc::Region> Avc444Encoder::regions(std::span<const std::size_t> tiles, std::uint8_t qp) const
{
    const codec::avc::QuantQuality quant{.qp = qp, .progressive = false, .quality = codec::avc::quality_from_qp(qp)};
    std::vector<codec::avc::Region> out;
    out.reserve(tiles.size());
    for (const std::size_t tile : tiles) {
        out.push_back({tile_rect(tile), quant});
    }
    return out;
}

Result<std::optional<Avc444Frame>> Avc444Encoder::encode(const codec::ImageView& frame, std::span<const Rect16> damage,
                                                         const FrameOptions& options)
{
    FARLAND_ASSERT(frame.width == width_ && frame.height == height_);
    ++frame_;
    const std::size_t tile_count = pending_.size();
    const auto all_tiles = [tile_count] {
        std::vector<std::size_t> tiles(tile_count);
        for (std::size_t i = 0; i < tile_count; ++i) {
            tiles[i] = i;
        }
        return tiles;
    };

    std::vector<std::size_t> luma;
    if (restart_) {
        luma = all_tiles();
    } else {
        for (std::size_t tile = 0; tile < tile_count; ++tile) {
            const Rect16 r = tile_rect(tile);
            if (std::ranges::any_of(damage, [&](const Rect16& d) { return intersects(r, d); })) {
                luma.push_back(tile);
            }
        }
    }
    if (luma.empty() && !chroma_pending()) {
        return std::optional<Avc444Frame>{};
    }
    codec::bgrx_to_avc444(frame, version_, yuv_, main_, aux_);

    std::optional<EncodedFrame> main_picture;
    if (!luma.empty()) {
        FrameOptions main_options = options;
        main_options.force_idr = options.force_idr || restart_;
        FARLAND_TRY(auto encoded, encoder_->encode(main_.view(), main_options));
        if (encoded.bitstream.empty()) {
            return std::optional<Avc444Frame>{};  // dropped; the caller keeps the damage
        }
        restart_ = false;
        if (encoded.idr) {
            luma = all_tiles();  // an IDR lists the whole surface, as for AVC420
        }
        main_picture = std::move(encoded);
    }

    // Tiles whose main view goes out show 4:2:0 until a chroma view follows,
    // which they need unless their chroma is flat. A tile that was already
    // waiting keeps its age.
    std::vector<Pending> pending = pending_;
    for (const std::size_t tile : luma) {
        if (!needs_chroma(tile)) {
            pending[tile].reset();
        } else if (!pending[tile].has_value()) {
            pending[tile] = frame_;
        }
    }
    std::vector<std::size_t> chroma;
    bool overdue = false;
    for (std::size_t tile = 0; tile < tile_count; ++tile) {
        if (const auto since = pending[tile]) {
            chroma.push_back(tile);
            overdue = overdue || (policy_.max_chroma_delay != 0 && frame_ - *since >= policy_.max_chroma_delay);
        }
    }
    bool send_chroma = !chroma.empty();
    if (send_chroma && main_picture.has_value()) {
        const bool short_of_bandwidth =
            policy_.defer_chroma ||
            (policy_.luma_budget_bytes != 0 && main_picture->bitstream.size() >= policy_.luma_budget_bytes);
        send_chroma = !short_of_bandwidth || overdue;
    }

    std::optional<EncodedFrame> aux_picture;
    if (send_chroma) {
        FrameOptions aux_options;
        aux_options.force_idr = !main_picture.has_value() && options.force_idr;
        if (options.timestamp_us.has_value()) {
            aux_options.timestamp_us = *options.timestamp_us + 1U;  // a second picture in the same frame
        }
        FARLAND_TRY(auto encoded, encoder_->encode(aux_.view(), aux_options));
        if (!encoded.bitstream.empty()) {
            aux_picture = std::move(encoded);
            for (const std::size_t tile : chroma) {
                pending[tile].reset();
            }
        }
    }
    if (!main_picture.has_value() && !aux_picture.has_value()) {
        return std::optional<Avc444Frame>{};  // the chroma view was dropped; it stays pending
    }
    pending_ = std::move(pending);

    Avc444Frame out;
    std::vector<codec::avc::Region> luma_regions;
    std::vector<codec::avc::Region> chroma_regions;
    if (main_picture.has_value()) {
        out.idr = main_picture->idr;
        luma_regions = out.idr ? regions({}, main_picture->qp) : regions(luma, main_picture->qp);
        if (out.idr) {
            const codec::avc::QuantQuality quant{
                .qp = main_picture->qp, .progressive = false, .quality = codec::avc::quality_from_qp(main_picture->qp)};
            luma_regions.push_back({Rect16{0, 0, width_, height_}, quant});
        }
    }
    if (aux_picture.has_value()) {
        chroma_regions = regions(chroma, aux_picture->qp);
    }

    using codec::avc::Avc420Part;
    using codec::avc::Avc444Layout;
    if (main_picture.has_value() && aux_picture.has_value()) {
        out.layout = Avc444Layout::luma_and_chroma;
        out.bitmap_stream = codec::avc::encode_avc444(out.layout, Avc420Part{luma_regions, main_picture->bitstream},
                                                      Avc420Part{chroma_regions, aux_picture->bitstream});
    } else if (main_picture.has_value()) {
        out.layout = Avc444Layout::luma;
        out.bitmap_stream = codec::avc::encode_avc444(out.layout, Avc420Part{luma_regions, main_picture->bitstream});
    } else {
        out.layout = Avc444Layout::chroma;
        out.bitmap_stream = codec::avc::encode_avc444(out.layout, Avc420Part{chroma_regions, aux_picture->bitstream});
    }

    Rect16 bounds{width_, height_, 0, 0};
    for (const auto* list : {&luma_regions, &chroma_regions}) {
        for (const auto& region : *list) {
            bounds.left = std::min(bounds.left, region.rect.left);
            bounds.top = std::min(bounds.top, region.rect.top);
            bounds.right = std::max(bounds.right, region.rect.right);
            bounds.bottom = std::max(bounds.bottom, region.rect.bottom);
            (list == &luma_regions ? out.luma_regions : out.chroma_regions).push_back(region.rect);
        }
    }
    out.dest_rect = bounds;
    return std::optional<Avc444Frame>{std::move(out)};
}

}  // namespace farland::video
