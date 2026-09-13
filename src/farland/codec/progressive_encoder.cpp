// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// RemoteFX Progressive encoder, [MS-RDPEGFX] 2.2.4.2 and 3.2.8.1.
//
// Ported from macRDP's RfxProgressiveEncoder.swift (same author), which is
// itself a port of FreeRDP's encoder path (rfx_encode.c, rfx_dwt.c,
// rfx_quantization.c, rfx_rlgr.c, rfx.c rfx_write_message_progressive_simple)
// and is known to work with mstsc. Differences from macRDP: damage comes from
// the caller instead of tile hashing, tiles are always sent at full
// progressive quality (no coarse-first stages yet; TILE_UPGRADE is M5), and a
// tile too large for one stream falls back to coarser quantization instead of
// overrunning the stream budget.

#include <farland/base/assert.hpp>
#include <farland/base/writer.hpp>
#include <farland/codec/progressive.hpp>
#include <farland/codec/rfx_common.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace farland::codec::progressive {

namespace {

// Block types, [MS-RDPEGFX] 2.2.4.2.1.
constexpr std::uint16_t wbt_sync = 0xCCC0;
constexpr std::uint16_t wbt_frame_begin = 0xCCC1;
constexpr std::uint16_t wbt_frame_end = 0xCCC2;
constexpr std::uint16_t wbt_context = 0xCCC3;
constexpr std::uint16_t wbt_region = 0xCCC4;
constexpr std::uint16_t wbt_tile_first = 0xCCC6;

// Block sizes, [MS-RDPEGFX] 2.2.4.2.1.1 to 2.2.4.2.1.5.4.
constexpr std::size_t sync_size = 12;
constexpr std::size_t context_size = 10;
constexpr std::size_t frame_begin_size = 12;
constexpr std::size_t frame_end_size = 6;
constexpr std::size_t region_header_size = 18;
constexpr std::size_t rect_size = 8;         // TS_RFX_RECT, [MS-RDPRFX] 2.2.2.1.6
constexpr std::size_t prog_quant_size = 16;  // RFX_PROGRESSIVE_CODEC_QUANT
constexpr std::size_t tile_first_header_size = 23;

constexpr std::uint32_t tile = rfx::tile_size;

/// Quantization levels a tile falls back to when it does not fit a stream:
/// the configured table, then 2, 4, 6 and 8 steps coarser, then all 15. A
/// region holds at most 7 tables ([MS-RDPEGFX] 2.2.4.2.1.5).
constexpr std::size_t ladder_steps = 6;
static_assert(ladder_steps <= 7);

[[nodiscard]] bool valid_quant(const rfx::Quant& q)
{
    return std::ranges::all_of(q.bands, [](std::uint8_t v) { return v >= 6 && v <= 15; });
}

[[nodiscard]] std::vector<rfx::Quant> make_ladder(const rfx::Quant& base)
{
    std::vector<rfx::Quant> ladder;
    for (std::size_t step = 0; step + 1 < ladder_steps; ++step) {
        rfx::Quant q = base;
        for (auto& v : q.bands) {
            v = static_cast<std::uint8_t>(std::min<std::size_t>(v + (2 * step), 15));
        }
        ladder.push_back(q);
    }
    ladder.push_back(rfx::uniform_quant(15));
    return ladder;
}

}  // namespace

struct Encoder::EncodedTile {
    std::uint16_t x_idx = 0;
    std::uint16_t y_idx = 0;
    std::uint8_t level = 0;  // index into ladder_
    std::array<std::uint16_t, 3> lengths{};
    std::vector<std::byte> data;  // Y, Cb, Cr RLGR1 data back to back

    /// Rect plus TILE_FIRST block.
    [[nodiscard]] std::size_t cost() const noexcept { return rect_size + tile_first_header_size + data.size(); }
};

Encoder::Encoder(std::uint32_t width, std::uint32_t height, const EncoderOptions& options)
    : width_(width), height_(height), grid_width_((width + tile - 1) / tile), grid_height_((height + tile - 1) / tile),
      options_(options), ladder_(make_ladder(options.quant)),
      selected_(static_cast<std::size_t>(grid_width_) * grid_height_), planes_(std::make_unique<rfx::Planes>()),
      transformed_(std::make_unique<std::array<rfx::Coefficients, 3>>()),
      scratch_(std::make_unique<rfx::Coefficients>())
{
    FARLAND_ASSERT(width >= 1 && width <= max_dimension);
    FARLAND_ASSERT(height >= 1 && height <= max_dimension);
    FARLAND_ASSERT(options.max_bytes >= min_max_bytes && options.max_bytes <= max_max_bytes);
    FARLAND_ASSERT(valid_quant(options.quant));
}

void Encoder::set_quant(const rfx::Quant& quant)
{
    FARLAND_ASSERT(valid_quant(quant));
    options_.quant = quant;
    ladder_ = make_ladder(quant);
}

std::size_t Encoder::stream_overhead(std::size_t quant_tables, bool sync) noexcept
{
    return (sync ? sync_size : 0) + context_size + frame_begin_size + region_header_size +
           (quant_tables * rfx::component_quant_size) + prog_quant_size + frame_end_size;
}

void Encoder::transform(const rfx::Planes& planes)
{
    const std::array<const rfx::Coefficients*, 3> sources{&planes.y, &planes.cb, &planes.cr};
    for (std::size_t c = 0; c < 3; ++c) {
        transformed_->at(c) = *sources.at(c);
        rfx::dwt_encode(transformed_->at(c), *scratch_);
    }
}

void Encoder::code_components(std::size_t level, EncodedTile& out)
{
    out.level = static_cast<std::uint8_t>(level);
    out.data.clear();
    for (std::size_t c = 0; c < 3; ++c) {
        rfx::Coefficients& coeffs = *scratch_;
        coeffs = transformed_->at(c);
        rfx::quantize(coeffs, ladder_.at(level));
        const auto& ll3 = rfx::standard_layout.at(static_cast<std::size_t>(rfx::Band::ll3));
        rfx::differential_encode(std::span(coeffs).subspan(ll3.offset, ll3.size));
        const std::size_t before = out.data.size();
        rfx::rlgr_encode(rfx::RlgrMode::rlgr1, coeffs, out.data);
        const std::size_t length = out.data.size() - before;
        out.lengths.at(c) = static_cast<std::uint16_t>(std::min<std::size_t>(length, UINT16_MAX));
    }
}

Encoder::EncodedTile Encoder::encode_tile(const ImageView& image, std::uint32_t tx, std::uint32_t ty)
{
    EncodedTile out;
    out.x_idx = static_cast<std::uint16_t>(tx);
    out.y_idx = static_cast<std::uint16_t>(ty);
    rfx::load_tile(image, tx * tile, ty * tile, *planes_);

    // The largest tile a stream can carry on its own, with room for every
    // quantization table of the ladder.
    const std::size_t budget = options_.max_bytes - stream_overhead(ladder_.size(), true);
    const auto fits = [&](const EncodedTile& t) {
        return t.cost() <= budget && std::ranges::all_of(t.lengths, [](std::uint16_t l) { return l < UINT16_MAX; });
    };
    transform(*planes_);
    for (std::size_t level = 0; level < ladder_.size(); ++level) {
        code_components(level, out);
        if (fits(out)) {
            return out;
        }
    }

    // Last resort: the tile's average colour, which codes to a few bytes.
    const std::uint32_t x0 = tx * tile;
    const std::uint32_t y0 = ty * tile;
    const std::size_t w = std::min(tile, image.width - x0);
    const std::size_t h = std::min(tile, image.height - y0);
    FARLAND_ASSERT(w > 0 && h > 0);  // load_tile checked (x0, y0)
    std::array<std::uint64_t, 3> sum{};
    for (std::size_t row = 0; row < h; ++row) {
        const auto line = image.data.subspan(((y0 + row) * image.stride) + (std::size_t{x0} * 4), w * 4);
        for (std::size_t col = 0; col < w; ++col) {
            for (std::size_t c = 0; c < 3; ++c) {
                sum.at(c) += std::to_integer<std::uint64_t>(line[(col * 4) + c]);
            }
        }
    }
    const auto mean = [&](std::size_t c) { return static_cast<std::int32_t>(sum.at(c) / (w * h)); };
    const auto flat = rfx::rgb_to_ycbcr(mean(2), mean(1), mean(0));
    planes_->y.fill(flat.y);
    planes_->cb.fill(flat.cb);
    planes_->cr.fill(flat.cr);
    transform(*planes_);
    code_components(0, out);
    FARLAND_ASSERT(fits(out));
    return out;
}

std::vector<std::byte> Encoder::write_stream(std::span<const EncodedTile* const> tiles)
{
    // Quantization tables in this stream: the distinct ladder levels used.
    std::array<std::uint8_t, ladder_steps> table_of_level{};
    std::vector<std::uint8_t> levels;
    for (const EncodedTile* t : tiles) {
        if (std::ranges::find(levels, t->level) == levels.end()) {
            levels.push_back(t->level);
        }
    }
    std::ranges::sort(levels);
    for (std::size_t i = 0; i < levels.size(); ++i) {
        table_of_level.at(levels[i]) = static_cast<std::uint8_t>(i);
    }

    const bool sync = options_.sync_every_stream || !synced_;
    synced_ = true;
    std::size_t tiles_size = 0;
    for (const EncodedTile* t : tiles) {
        tiles_size += tile_first_header_size + t->data.size();
    }
    const std::size_t region_size = region_header_size + (tiles.size() * rect_size) +
                                    (levels.size() * rfx::component_quant_size) + prog_quant_size + tiles_size;

    Writer w(stream_overhead(levels.size(), sync) + (tiles.size() * rect_size) + tiles_size);
    if (sync) {
        // RFX_PROGRESSIVE_SYNC, [MS-RDPEGFX] 2.2.4.2.1.1.
        w.u16le(wbt_sync);
        w.u32le(sync_size);
        w.u32le(0xCACCACCA);  // magic
        w.u16le(0x0100);      // version 1.0
    }
    // RFX_PROGRESSIVE_CONTEXT, [MS-RDPEGFX] 2.2.4.2.1.4: ctxId 0, tileSize 64,
    // no RFX_SUBBAND_DIFFING.
    w.u16le(wbt_context);
    w.u32le(context_size);
    w.u8(0);
    w.u16le(tile);
    w.u8(0);
    // RFX_PROGRESSIVE_FRAME_BEGIN, [MS-RDPEGFX] 2.2.4.2.1.2: one region.
    w.u16le(wbt_frame_begin);
    w.u32le(frame_begin_size);
    w.u32le(frame_index_);
    w.u16le(1);
    // RFX_PROGRESSIVE_REGION, [MS-RDPEGFX] 2.2.4.2.1.5.
    w.u16le(wbt_region);
    w.u32le(static_cast<std::uint32_t>(region_size));
    w.u8(static_cast<std::uint8_t>(tile));              // tileSize
    w.u16le(static_cast<std::uint16_t>(tiles.size()));  // numRects
    w.u8(static_cast<std::uint8_t>(levels.size()));     // numQuant
    w.u8(1);                                            // numProgQuant
    w.u8(0);                                            // flags: classic DWT
    w.u16le(static_cast<std::uint16_t>(tiles.size()));  // numTiles
    w.u32le(static_cast<std::uint32_t>(tiles_size));    // tileDataSize
    for (const EncodedTile* t : tiles) {
        // One TS_RFX_RECT per tile, clipped to the surface.
        const std::uint32_t x = std::uint32_t{t->x_idx} * tile;
        const std::uint32_t y = std::uint32_t{t->y_idx} * tile;
        w.u16le(static_cast<std::uint16_t>(x));
        w.u16le(static_cast<std::uint16_t>(y));
        w.u16le(static_cast<std::uint16_t>(std::min(tile, width_ - x)));
        w.u16le(static_cast<std::uint16_t>(std::min(tile, height_ - y)));
    }
    for (const std::uint8_t level : levels) {
        rfx::write_component_quant(w, ladder_.at(level));
    }
    // One RFX_PROGRESSIVE_CODEC_QUANT ([MS-RDPEGFX] 2.2.4.2.1.5.1): quality
    // 100, no extra quantization for Y, Cb and Cr.
    w.u8(100);
    for (int c = 0; c < 3; ++c) {
        write_component_quant(w, rfx::uniform_quant(0));
    }
    for (const EncodedTile* t : tiles) {
        // RFX_PROGRESSIVE_TILE_FIRST, [MS-RDPEGFX] 2.2.4.2.1.5.4.
        const std::uint8_t quant_idx = table_of_level.at(t->level);
        w.u16le(wbt_tile_first);
        w.u32le(static_cast<std::uint32_t>(tile_first_header_size + t->data.size()));
        w.u8(quant_idx);  // quantIdxY
        w.u8(quant_idx);  // quantIdxCb
        w.u8(quant_idx);  // quantIdxCr
        w.u16le(t->x_idx);
        w.u16le(t->y_idx);
        w.u8(0);  // flags: not a difference tile
        w.u8(0);  // progressiveQuality: quantProgVals[0]
        for (const std::uint16_t length : t->lengths) {
            w.u16le(length);
        }
        w.u16le(0);  // tailLen
        w.bytes(t->data);
    }
    // RFX_PROGRESSIVE_FRAME_END, [MS-RDPEGFX] 2.2.4.2.1.3.
    w.u16le(wbt_frame_end);
    w.u32le(frame_end_size);

    FARLAND_ASSERT(w.size() <= options_.max_bytes);
    return std::move(w).take();
}

std::vector<std::vector<std::byte>> Encoder::encode(const ImageView& image, std::span<const Rect> damage)
{
    FARLAND_ASSERT(image.width == width_ && image.height == height_);
    FARLAND_ASSERT(image.stride >= std::size_t{width_} * 4);
    FARLAND_ASSERT(image.data.size() >= (image.stride * (height_ - 1)) + (std::size_t{width_} * 4));

    // Tiles touched by the damage.
    std::ranges::fill(selected_, false);
    std::size_t count = 0;
    for (const Rect& r : damage) {
        if (r.x >= width_ || r.y >= height_ || r.width == 0 || r.height == 0) {
            continue;
        }
        const std::uint32_t right = r.x + std::min(r.width, width_ - r.x);
        const std::uint32_t bottom = r.y + std::min(r.height, height_ - r.y);
        for (std::uint32_t ty = r.y / tile; ty < (bottom + tile - 1) / tile; ++ty) {
            for (std::uint32_t tx = r.x / tile; tx < (right + tile - 1) / tile; ++tx) {
                const std::size_t i = (std::size_t{ty} * grid_width_) + tx;
                if (!selected_[i]) {
                    selected_[i] = true;
                    ++count;
                }
            }
        }
    }
    if (count == 0) {
        return {};
    }

    std::vector<EncodedTile> tiles;
    tiles.reserve(count);
    for (std::uint32_t ty = 0; ty < grid_height_; ++ty) {
        for (std::uint32_t tx = 0; tx < grid_width_; ++tx) {
            if (selected_[(std::size_t{ty} * grid_width_) + tx]) {
                tiles.push_back(encode_tile(image, tx, ty));
            }
        }
    }
    ++frame_index_;

    // Deal the tiles round-robin over the expected number of streams, so each
    // stream spans the whole damaged area and a large update does not paint
    // top to bottom (macRDP).
    std::size_t total = stream_overhead(1, options_.sync_every_stream);
    for (const auto& t : tiles) {
        total += t.cost();
    }
    const std::size_t buckets = std::max<std::size_t>(1, (total + options_.max_bytes - 1) / options_.max_bytes);
    std::vector<const EncodedTile*> ordered;
    ordered.reserve(tiles.size());
    for (std::size_t b = 0; b < buckets; ++b) {
        for (std::size_t i = b; i < tiles.size(); i += buckets) {
            ordered.push_back(&tiles[i]);
        }
    }

    // Greedy packing with the exact stream size.
    std::vector<std::vector<std::byte>> streams;
    std::vector<const EncodedTile*> chunk;
    std::array<bool, ladder_steps> used{};
    std::size_t chunk_tables = 0;
    std::size_t chunk_bytes = 0;
    const auto flush = [&] {
        streams.push_back(write_stream(chunk));
        chunk.clear();
        used.fill(false);
        chunk_tables = 0;
        chunk_bytes = 0;
    };
    for (const EncodedTile* t : ordered) {
        const bool new_table = !used.at(t->level);
        const bool sync = options_.sync_every_stream || (!synced_ && streams.empty());
        const std::size_t size = stream_overhead(chunk_tables + (new_table ? 1 : 0), sync) + chunk_bytes + t->cost();
        if (!chunk.empty() && size > options_.max_bytes) {
            flush();
        }
        if (!used.at(t->level)) {
            used.at(t->level) = true;
            ++chunk_tables;
        }
        chunk.push_back(t);
        chunk_bytes += t->cost();
    }
    flush();
    return streams;
}

}  // namespace farland::codec::progressive
