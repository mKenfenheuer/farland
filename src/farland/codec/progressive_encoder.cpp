// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// RemoteFX Progressive encoder, [MS-RDPEGFX] 2.2.4.2 and 3.2.8.1.
//
// Ported from macRDP's RfxProgressiveEncoder.swift (same author), which is
// itself a port of FreeRDP's encoder path (rfx_encode.c, rfx_dwt.c,
// rfx_quantization.c, rfx_rlgr.c, rfx.c rfx_write_message_progressive_simple)
// and is known to work with mstsc. Differences from macRDP: damage comes from
// the caller instead of tile hashing, a tile too large for one stream falls
// back to coarser quantization instead of overrunning the stream budget, and
// there are the reduce-extrapolate DWT and progressive refinement (first pass
// plus TILE_UPGRADE passes, 3.2.8.1.5), which neither macRDP nor FreeRDP
// encode.

#include <farland/base/assert.hpp>
#include <farland/base/writer.hpp>
#include <farland/codec/progressive.hpp>
#include <farland/codec/rfx_common.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <utility>
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
constexpr std::uint16_t wbt_tile_upgrade = 0xCCC7;

// Block sizes, [MS-RDPEGFX] 2.2.4.2.1.1 to 2.2.4.2.1.5.5.
constexpr std::size_t sync_size = 12;
constexpr std::size_t context_size = 10;
constexpr std::size_t frame_begin_size = 12;
constexpr std::size_t frame_end_size = 6;
constexpr std::size_t region_header_size = 18;
constexpr std::size_t rect_size = 8;         // TS_RFX_RECT, [MS-RDPRFX] 2.2.2.1.6
constexpr std::size_t prog_quant_size = 16;  // RFX_PROGRESSIVE_CODEC_QUANT
constexpr std::size_t tile_first_header_size = 23;
constexpr std::size_t tile_upgrade_header_size = 26;

constexpr std::uint8_t subband_diffing = 0x01;     // RFX_SUBBAND_DIFFING, 2.2.4.2.1.4
constexpr std::uint8_t reduce_extrapolate = 0x01;  // RFX_DWT_REDUCE_EXTRAPOLATE, 2.2.4.2.1.5

constexpr std::uint32_t tile = rfx::tile_size;
constexpr std::size_t ll3 = std::to_underlying(rfx::Band::ll3);

/// Quantization levels a tile falls back to when it does not fit a stream:
/// the configured table, then 2, 4, 6 and 8 steps coarser, then all 15. A
/// region holds at most 7 tables ([MS-RDPEGFX] 2.2.4.2.1.5).
constexpr std::size_t ladder_steps = 6;
static_assert(ladder_steps <= 7);

/// The quality stages must only ever lower the extra quantization (a decoder
/// rejects an upgrade that raises a band's BitPos), stay within the 0..8 of
/// 2.2.4.2.1.5.2, end at zero, and never touch LL3: the first pass codes LL3
/// through RLGR1, whose encoder sends a trailing zero after a zero run as 1
/// (rfx_rlgr_encode); the decoder's LL3 can then be one step above what the
/// encoder assumes, and a later raw upgrade could not correct that.
constexpr bool stages_valid()
{
    for (std::size_t s = 0; s < quality_stages.size(); ++s) {
        for (std::size_t b = 0; b < rfx::band_count; ++b) {
            const std::uint8_t v = quality_stages[s].shift.bands[b];
            if (v > 8 || (b == ll3 && v != 0) || (s + 1 == quality_stages.size() && v != 0) ||
                (s > 0 && v > quality_stages[s - 1].shift.bands[b])) {
                return false;
            }
        }
    }
    return true;
}
static_assert(stages_valid());

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

[[nodiscard]] std::span<std::int16_t> band_of(rfx::Coefficients& c, const rfx::Layout& layout, std::size_t band)
{
    return std::span(c).subspan(layout.at(band).offset, layout.at(band).size);
}

[[nodiscard]] std::span<const std::int16_t> band_of(const rfx::Coefficients& c, const rfx::Layout& layout,
                                                    std::size_t band)
{
    return std::span(c).subspan(layout.at(band).offset, layout.at(band).size);
}

/// A region holds at most 7 quantization tables ([MS-RDPEGFX] 2.2.4.2.1.5).
constexpr std::size_t max_tables = 7;

/// The extra quantization of quality stage `stage` for a tile quantized with
/// `base`: at most 15 - base per band, so that BitPos (base + extra) stays at
/// 15 or below and the decoder's dequantization shift below 16.
[[nodiscard]] rfx::Quant extra_shift(const rfx::Quant& base, std::size_t stage)
{
    rfx::Quant out;
    for (std::size_t b = 0; b < rfx::band_count; ++b) {
        out.bands.at(b) = std::min<std::uint8_t>(quality_stages.at(stage).shift.bands.at(b),
                                                 static_cast<std::uint8_t>(15 - base.bands.at(b)));
    }
    return out;
}

}  // namespace

/// One tile block: TILE_FIRST (encode) or TILE_UPGRADE (upgrade).
struct Encoder::Block {
    std::uint16_t x_idx = 0;
    std::uint16_t y_idx = 0;
    rfx::Quant quant{};        // the quantization table
    std::uint8_t quality = 0;  // the quality stage (0 without refinement)
    bool upgrade = false;
    bool flat = false;  // the average-colour fallback
    // TILE_FIRST: yLen, cbLen, crLen. TILE_UPGRADE: ySrlLen, yRawLen,
    // cbSrlLen, cbRawLen, crSrlLen, crRawLen.
    std::array<std::uint16_t, 6> lengths{};
    std::vector<std::byte> data;  // the components' data back to back

    /// Rect plus tile block.
    [[nodiscard]] std::size_t cost() const noexcept
    {
        return rect_size + (upgrade ? tile_upgrade_header_size : tile_first_header_size) + data.size();
    }
};

/// A tile below full quality ([MS-RDPEGFX] 3.1.8.1.4): its quantized DWT
/// coefficients (SB of 3.2.8.1.5, before the extra quantization and the LL3
/// differences) and the stage the client has. What the client holds of each
/// coefficient (DAS) follows from these: trunc(SB / 2^BitPos) * 2^BitPos.
struct Encoder::TileState {
    std::array<rfx::Coefficients, 3> sb{};
    std::uint8_t stage = 0;
    rfx::Quant quant{};  // of the first pass; upgrades use the same table

    /// True when stage `s` already gives the client every bit.
    [[nodiscard]] bool complete_at(std::uint8_t s, const rfx::Layout& layout) const
    {
        for (const auto& component : sb) {
            for (std::size_t b = 0; b < rfx::band_count; ++b) {
                const std::uint32_t shift = extra_shift(quant, s).bands.at(b);
                if (shift == 0) {
                    continue;
                }
                const std::uint32_t mask = (1U << shift) - 1;
                for (const std::int16_t v : band_of(component, layout, b)) {
                    const auto magnitude = static_cast<std::uint32_t>(v < 0 ? -std::int32_t{v} : std::int32_t{v});
                    if ((magnitude & mask) != 0) {
                        return false;
                    }
                }
            }
        }
        return true;
    }
};

Encoder::Encoder(std::uint32_t width, std::uint32_t height, const EncoderOptions& options)
    : width_(width), height_(height), grid_width_((width + tile - 1) / tile), grid_height_((height + tile - 1) / tile),
      options_(options), ladder_(make_ladder(options.quant)),
      selected_(static_cast<std::size_t>(grid_width_) * grid_height_), planes_(std::make_unique<rfx::Planes>()),
      transformed_(std::make_unique<std::array<rfx::Coefficients, 3>>()),
      quantized_(std::make_unique<std::array<rfx::Coefficients, 3>>()), scratch_(std::make_unique<rfx::Coefficients>())
{
    FARLAND_ASSERT(width >= 1 && width <= max_dimension);
    FARLAND_ASSERT(height >= 1 && height <= max_dimension);
    FARLAND_ASSERT(options.max_bytes >= min_max_bytes && options.max_bytes <= max_max_bytes);
    FARLAND_ASSERT(valid_quant(options.quant));
    FARLAND_ASSERT(!options.refine || options.reduce_extrapolate);
    if (options.refine) {
        states_.resize(selected_.size());
    }
}

Encoder::Encoder(Encoder&&) noexcept = default;
Encoder& Encoder::operator=(Encoder&&) noexcept = default;
Encoder::~Encoder() = default;

void Encoder::set_quant(const rfx::Quant& quant)
{
    FARLAND_ASSERT(valid_quant(quant));
    options_.quant = quant;
    ladder_ = make_ladder(quant);
}

void Encoder::reset() noexcept
{
    synced_ = false;
    for (auto& state : states_) {
        state.reset();
    }
    pending_ = 0;
}

void Encoder::drop_state(std::size_t index) noexcept
{
    if (index < states_.size() && states_[index]) {
        states_[index].reset();
        --pending_;
    }
}

std::size_t Encoder::stream_overhead(std::size_t quant_tables, bool sync) const noexcept
{
    // With refinement, each quantization table has its own quality stages.
    const std::size_t prog_tables = options_.refine ? quality_stages.size() * quant_tables : 1;
    return (sync ? sync_size : 0) + context_size + frame_begin_size + region_header_size +
           (quant_tables * rfx::component_quant_size) + (prog_tables * prog_quant_size) + frame_end_size;
}

std::size_t Encoder::select(std::span<const Rect> rects)
{
    std::ranges::fill(selected_, false);
    std::size_t count = 0;
    for (const Rect& r : rects) {
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
    return count;
}

void Encoder::transform(const rfx::Planes& planes)
{
    const std::array<const rfx::Coefficients*, 3> sources{&planes.y, &planes.cb, &planes.cr};
    for (std::size_t c = 0; c < 3; ++c) {
        transformed_->at(c) = *sources.at(c);
        if (options_.reduce_extrapolate) {
            rfx::dwt_encode_extrapolate(transformed_->at(c), *scratch_);
        } else {
            rfx::dwt_encode(transformed_->at(c), *scratch_);
        }
    }
}

void Encoder::code_components(std::size_t level, std::uint8_t stage, Block& out)
{
    const rfx::Layout& layout = options_.reduce_extrapolate ? rfx::extrapolate_layout : rfx::standard_layout;
    out.quant = ladder_.at(level);
    out.quality = stage;  // the quality stage, or 0 for the only table
    out.data.clear();
    for (std::size_t c = 0; c < 3; ++c) {
        rfx::Coefficients& quantized = quantized_->at(c);
        quantized = transformed_->at(c);
        rfx::quantize(quantized, ladder_.at(level), layout);
        rfx::Coefficients& coeffs = *scratch_;
        coeffs = quantized;
        if (options_.refine) {
            // Extra quantization of the tile's stage ([MS-RDPEGFX] 3.1.8.1.3),
            // toward zero; LL3 has none (stages_valid), and the last stage
            // drops nothing, which is what a direct pass sends.
            const rfx::Quant extra = extra_shift(out.quant, stage);
            for (std::size_t b = 0; b < rfx::band_count; ++b) {
                const std::uint32_t shift = extra.bands.at(b);
                if (shift == 0) {
                    continue;
                }
                for (auto& v : band_of(coeffs, layout, b)) {
                    const std::int32_t magnitude = std::abs(std::int32_t{v}) >> shift;
                    v = static_cast<std::int16_t>(v < 0 ? -magnitude : magnitude);
                }
            }
        }
        rfx::differential_encode(band_of(coeffs, layout, ll3));
        const std::size_t before = out.data.size();
        rfx::rlgr_encode(rfx::RlgrMode::rlgr1, coeffs, out.data);
        const std::size_t length = out.data.size() - before;
        out.lengths.at(c) = static_cast<std::uint16_t>(std::min<std::size_t>(length, UINT16_MAX));
    }
}

Encoder::Block Encoder::encode_tile(const ImageView& image, std::uint32_t tx, std::uint32_t ty, std::uint8_t stage)
{
    Block out;
    out.x_idx = static_cast<std::uint16_t>(tx);
    out.y_idx = static_cast<std::uint16_t>(ty);
    rfx::load_tile(image, tx * tile, ty * tile, *planes_);

    // The largest tile a stream can carry on its own, with room for every
    // quantization table of the ladder.
    const std::size_t budget = options_.max_bytes - stream_overhead(ladder_.size(), true);
    const auto fits = [&](const Block& t) {
        return t.cost() <= budget &&
               std::ranges::all_of(std::span(t.lengths).first(3), [](std::uint16_t l) { return l < UINT16_MAX; });
    };
    transform(*planes_);
    for (std::size_t level = 0; level < ladder_.size(); ++level) {
        code_components(level, stage, out);
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
    code_components(0, stage, out);
    out.flat = true;
    FARLAND_ASSERT(fits(out));
    return out;
}

Encoder::Block Encoder::encode_upgrade(const TileState& state, std::uint32_t index, std::uint8_t to) const
{
    FARLAND_ASSERT(to > state.stage && to <= full_quality_stage);
    Block out;
    out.x_idx = static_cast<std::uint16_t>(index % grid_width_);
    out.y_idx = static_cast<std::uint16_t>(index / grid_width_);
    out.quant = state.quant;
    out.quality = to;
    out.upgrade = true;
    const rfx::Quant from = extra_shift(state.quant, state.stage);
    const rfx::Quant target = extra_shift(state.quant, to);
    for (std::size_t c = 0; c < 3; ++c) {
        // The decoder walks the bands in order with one SRL state per
        // component ([MS-RDPEGFX] 3.3.8.2.1.2); LL3 never changes.
        std::vector<std::byte> srl;
        std::vector<std::byte> raw;
        rfx::BitWriter srl_bits(srl);
        rfx::BitWriter raw_bits(raw);
        std::uint32_t kp = 8;
        std::uint32_t zeros = 0;
        for (std::size_t b = 0; b < rfx::band_count; ++b) {
            const std::uint8_t f = from.bands.at(b);
            const std::uint8_t t = target.bands.at(b);
            if (f > t) {
                rfx::upgrade_encode_band(band_of(state.sb.at(c), rfx::extrapolate_layout, b), f, t, srl_bits, raw_bits,
                                         kp, zeros);
            }
        }
        raw_bits.flush();
        rfx::upgrade_encode_finish(srl_bits, srl, kp, zeros);
        FARLAND_ASSERT(srl.size() < UINT16_MAX && raw.size() < UINT16_MAX);
        out.lengths.at(2 * c) = static_cast<std::uint16_t>(srl.size());
        out.lengths.at((2 * c) + 1) = static_cast<std::uint16_t>(raw.size());
        out.data.insert(out.data.end(), srl.begin(), srl.end());
        out.data.insert(out.data.end(), raw.begin(), raw.end());
    }
    return out;
}

namespace {

/// The distinct quantization tables of `blocks`, in order of appearance.
template <class Blocks>
std::vector<rfx::Quant> tables_of(const Blocks& blocks)
{
    std::vector<rfx::Quant> tables;
    for (const auto* b : blocks) {
        if (std::ranges::find(tables, b->quant) == tables.end()) {
            tables.push_back(b->quant);
        }
    }
    return tables;
}

}  // namespace

std::size_t Encoder::stream_size(const Group& group, bool sync) const noexcept
{
    std::size_t bytes = 0;
    for (const Block* b : group) {
        bytes += b->cost();
    }
    return stream_overhead(tables_of(group).size(), sync) + bytes;
}

std::vector<Encoder::Group> Encoder::group(std::span<const Block* const> blocks) const
{
    if (blocks.empty()) {
        return {};
    }
    // Deal the tiles round-robin over the expected number of streams, so each
    // stream spans the whole area and a large update does not paint top to
    // bottom (macRDP).
    std::size_t total = stream_overhead(1, options_.sync_every_stream);
    for (const Block* b : blocks) {
        total += b->cost();
    }
    const std::size_t buckets = std::max<std::size_t>(1, (total + options_.max_bytes - 1) / options_.max_bytes);
    Group ordered;
    ordered.reserve(blocks.size());
    for (std::size_t k = 0; k < buckets; ++k) {
        for (std::size_t i = k; i < blocks.size(); i += buckets) {
            ordered.push_back(blocks[i]);
        }
    }

    // Greedy packing with the exact stream size, at most max_tables
    // quantization tables per stream.
    std::vector<Group> groups;
    Group chunk;
    std::vector<rfx::Quant> tables;
    std::size_t chunk_bytes = 0;
    for (const Block* b : ordered) {
        const bool new_table = std::ranges::find(tables, b->quant) == tables.end();
        const bool sync = options_.sync_every_stream || (!synced_ && groups.empty());
        const std::size_t size = stream_overhead(tables.size() + (new_table ? 1 : 0), sync) + chunk_bytes + b->cost();
        if (!chunk.empty() && (size > options_.max_bytes || (new_table && tables.size() == max_tables))) {
            groups.push_back(std::move(chunk));
            chunk.clear();
            tables.clear();
            chunk_bytes = 0;
        }
        if (std::ranges::find(tables, b->quant) == tables.end()) {
            tables.push_back(b->quant);
        }
        chunk.push_back(b);
        chunk_bytes += b->cost();
    }
    groups.push_back(std::move(chunk));
    return groups;
}

bool Encoder::within(std::span<const Block* const> blocks, std::size_t budget) const
{
    // Greedy packing starts a new stream only when a block does not fit, so
    // every stream but the last carries more than max_bytes minus the largest
    // overhead and block. That bound settles most checks without packing.
    std::size_t sum = 0;
    std::size_t largest = 0;
    for (const Block* b : blocks) {
        sum += b->cost();
        largest = std::max(largest, b->cost());
    }
    const std::size_t overhead = stream_overhead(max_tables, true);
    if (options_.max_bytes > overhead + largest) {
        const std::size_t streams = 1 + (sum / (options_.max_bytes - overhead - largest));
        if (sum + (streams * overhead) <= budget) {
            return true;
        }
    }
    return total_size(group(blocks)) <= budget;
}

std::size_t Encoder::total_size(const std::vector<Group>& groups) const noexcept
{
    std::size_t total = 0;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        total += stream_size(groups[i], options_.sync_every_stream || (!synced_ && i == 0));
    }
    return total;
}

std::vector<std::byte> Encoder::write_stream(const Group& blocks)
{
    // Quantization tables in this stream: the distinct ones its tiles use.
    const std::vector<rfx::Quant> levels = tables_of(blocks);
    FARLAND_ASSERT(levels.size() <= max_tables);
    const auto table_of = [&](const Block& b) {
        return static_cast<std::uint8_t>(std::ranges::find(levels, b.quant) - levels.begin());
    };

    const bool sync = options_.sync_every_stream || !synced_;
    synced_ = true;
    const std::size_t prog_tables = options_.refine ? quality_stages.size() * levels.size() : 1;
    std::size_t tiles_size = 0;
    for (const Block* b : blocks) {
        tiles_size += b->cost() - rect_size;
    }
    const std::size_t region_size = region_header_size + (blocks.size() * rect_size) +
                                    (levels.size() * rfx::component_quant_size) + (prog_tables * prog_quant_size) +
                                    tiles_size;

    Writer w(stream_overhead(levels.size(), sync) + (blocks.size() * rect_size) + tiles_size);
    if (sync) {
        // RFX_PROGRESSIVE_SYNC, [MS-RDPEGFX] 2.2.4.2.1.1.
        w.u16le(wbt_sync);
        w.u32le(sync_size);
        w.u32le(0xCACCACCA);  // magic
        w.u16le(0x0100);      // version 1.0
    }
    // RFX_PROGRESSIVE_CONTEXT, [MS-RDPEGFX] 2.2.4.2.1.4: ctxId 0, tileSize 64;
    // RFX_SUBBAND_DIFFING when refining.
    w.u16le(wbt_context);
    w.u32le(context_size);
    w.u8(0);
    w.u16le(tile);
    w.u8(options_.refine ? subband_diffing : 0);
    // RFX_PROGRESSIVE_FRAME_BEGIN, [MS-RDPEGFX] 2.2.4.2.1.2: one region.
    w.u16le(wbt_frame_begin);
    w.u32le(frame_begin_size);
    w.u32le(frame_index_);
    w.u16le(1);
    // RFX_PROGRESSIVE_REGION, [MS-RDPEGFX] 2.2.4.2.1.5.
    w.u16le(wbt_region);
    w.u32le(static_cast<std::uint32_t>(region_size));
    w.u8(static_cast<std::uint8_t>(tile));                       // tileSize
    w.u16le(static_cast<std::uint16_t>(blocks.size()));          // numRects
    w.u8(static_cast<std::uint8_t>(levels.size()));              // numQuant
    w.u8(static_cast<std::uint8_t>(prog_tables));                // numProgQuant
    w.u8(options_.reduce_extrapolate ? reduce_extrapolate : 0);  // flags
    w.u16le(static_cast<std::uint16_t>(blocks.size()));          // numTiles
    w.u32le(static_cast<std::uint32_t>(tiles_size));             // tileDataSize
    for (const Block* b : blocks) {
        // One TS_RFX_RECT per tile, clipped to the surface.
        const std::uint32_t x = std::uint32_t{b->x_idx} * tile;
        const std::uint32_t y = std::uint32_t{b->y_idx} * tile;
        w.u16le(static_cast<std::uint16_t>(x));
        w.u16le(static_cast<std::uint16_t>(y));
        w.u16le(static_cast<std::uint16_t>(std::min(tile, width_ - x)));
        w.u16le(static_cast<std::uint16_t>(std::min(tile, height_ - y)));
    }
    for (const rfx::Quant& q : levels) {
        rfx::write_component_quant(w, q);
    }
    // RFX_PROGRESSIVE_CODEC_QUANT ([MS-RDPEGFX] 2.2.4.2.1.5.1): the quality
    // stages for each quantization table (table t, stage s at index
    // t * stages + s), or one table of quality 100 without extra
    // quantization.
    if (options_.refine) {
        for (const rfx::Quant& q : levels) {
            for (std::size_t s = 0; s < quality_stages.size(); ++s) {
                w.u8(quality_stages.at(s).quality);
                for (int c = 0; c < 3; ++c) {
                    rfx::write_component_quant(w, extra_shift(q, s));
                }
            }
        }
    } else {
        w.u8(100);
        for (int c = 0; c < 3; ++c) {
            rfx::write_component_quant(w, rfx::uniform_quant(0));
        }
    }
    for (const Block* b : blocks) {
        const std::uint8_t quant_idx = table_of(*b);
        const auto quality =
            static_cast<std::uint8_t>(options_.refine ? (quant_idx * quality_stages.size()) + b->quality : 0);
        if (b->upgrade) {
            // RFX_PROGRESSIVE_TILE_UPGRADE, [MS-RDPEGFX] 2.2.4.2.1.5.5.
            w.u16le(wbt_tile_upgrade);
            w.u32le(static_cast<std::uint32_t>(tile_upgrade_header_size + b->data.size()));
        } else {
            // RFX_PROGRESSIVE_TILE_FIRST, [MS-RDPEGFX] 2.2.4.2.1.5.4.
            w.u16le(wbt_tile_first);
            w.u32le(static_cast<std::uint32_t>(tile_first_header_size + b->data.size()));
        }
        w.u8(quant_idx);  // quantIdxY
        w.u8(quant_idx);  // quantIdxCb
        w.u8(quant_idx);  // quantIdxCr
        w.u16le(b->x_idx);
        w.u16le(b->y_idx);
        if (b->upgrade) {
            w.u8(quality);  // progressiveQuality
            for (const std::uint16_t length : b->lengths) {
                w.u16le(length);
            }
        } else {
            w.u8(0);        // flags: not a difference tile
            w.u8(quality);  // progressiveQuality
            for (const std::uint16_t length : std::span(b->lengths).first(3)) {
                w.u16le(length);
            }
            w.u16le(0);  // tailLen
        }
        w.bytes(b->data);
    }
    // RFX_PROGRESSIVE_FRAME_END, [MS-RDPEGFX] 2.2.4.2.1.3.
    w.u16le(wbt_frame_end);
    w.u32le(frame_end_size);

    FARLAND_ASSERT(w.size() <= options_.max_bytes);
    return std::move(w).take();
}

std::vector<std::vector<std::byte>> Encoder::write_streams(const std::vector<Group>& groups)
{
    std::vector<std::vector<std::byte>> streams;
    streams.reserve(groups.size());
    for (const Group& g : groups) {
        streams.push_back(write_stream(g));
    }
    return streams;
}

std::vector<std::vector<std::byte>> Encoder::encode(const ImageView& image, std::span<const Rect> damage, Pass pass)
{
    FARLAND_ASSERT(image.width == width_ && image.height == height_);
    FARLAND_ASSERT(image.stride >= std::size_t{width_} * 4);
    FARLAND_ASSERT(image.data.size() >= (image.stride * (height_ - 1)) + (std::size_t{width_} * 4));

    const std::size_t count = select(damage);
    if (count == 0) {
        return {};
    }
    const rfx::Layout& layout = rfx::extrapolate_layout;  // refine implies reduce-extrapolate
    std::vector<Block> blocks;
    blocks.reserve(count);
    for (std::uint32_t ty = 0; ty < grid_height_; ++ty) {
        for (std::uint32_t tx = 0; tx < grid_width_; ++tx) {
            const std::size_t i = (std::size_t{ty} * grid_width_) + tx;
            if (!selected_[i]) {
                continue;
            }
            // The last stage drops no bits, so a direct pass sends what a
            // single pass would and owes no upgrade.
            const std::uint8_t stage = pass == Pass::direct ? full_quality_stage : 0;
            blocks.push_back(encode_tile(image, tx, ty, stage));
            if (!options_.refine) {
                continue;
            }
            // A new first pass replaces whatever refinement was pending.
            const Block& b = blocks.back();
            if (pass == Pass::direct) {
                drop_state(i);
                continue;
            }
            if (b.flat) {
                drop_state(i);
                continue;
            }
            if (!states_[i]) {
                states_[i] = std::make_unique<TileState>();
                ++pending_;
            }
            TileState& state = *states_[i];
            state.sb = *quantized_;
            state.stage = 0;
            state.quant = b.quant;
            if (state.complete_at(0, layout)) {
                drop_state(i);
            }
        }
    }
    ++frame_index_;

    std::vector<const Block*> pointers;
    pointers.reserve(blocks.size());
    for (const Block& b : blocks) {
        pointers.push_back(&b);
    }
    return write_streams(group(pointers));
}

std::vector<std::vector<std::byte>> Encoder::upgrade(std::size_t budget)
{
    const std::array all{Rect{.x = 0, .y = 0, .width = width_, .height = height_}};
    return upgrade(all, budget);
}

std::vector<std::vector<std::byte>> Encoder::upgrade(std::span<const Rect> area, std::size_t budget)
{
    if (!options_.refine || pending_ == 0 || select(area) == 0) {
        return {};
    }
    // Candidates: lowest stage first, then from the cursor on, so that a
    // small budget serves every tile in turn.
    const std::size_t n = states_.size();
    std::vector<std::uint32_t> candidates;
    for (std::size_t i = 0; i < n; ++i) {
        if (selected_[i] && states_[i]) {
            candidates.push_back(static_cast<std::uint32_t>(i));
        }
    }
    if (candidates.empty()) {
        return {};
    }
    std::ranges::sort(candidates, [&](std::uint32_t a, std::uint32_t b) {
        const auto key = [&](std::uint32_t i) { return std::pair{states_[i]->stage, (i + n - cursor_) % n}; };
        return key(a) < key(b);
    });

    const std::size_t count = candidates.size();
    std::vector<Block> blocks(count);
    std::vector<bool> has(count, false);
    std::vector<bool> stuck(count, false);
    std::vector<bool> hopeless(count, false);
    std::vector<std::uint8_t> target(count);
    for (std::size_t k = 0; k < count; ++k) {
        target[k] = states_[candidates[k]]->stage;
    }
    const std::size_t single_limit = options_.max_bytes - stream_overhead(1, true);
    const auto current = [&] {
        std::vector<const Block*> pointers;
        for (std::size_t k = 0; k < count; ++k) {
            if (has[k]) {
                pointers.push_back(&blocks[k]);
            }
        }
        return pointers;
    };

    // When the budget is generous, every tile goes straight to full quality
    // in one block. Stops at the first block that would overrun it, having
    // encoded no more than about a budget's worth.
    bool all_full = true;
    for (std::size_t k = 0; k < count && all_full; ++k) {
        const std::uint32_t index = candidates[k];
        blocks[k] = encode_upgrade(*states_[index], index, full_quality_stage);
        has[k] = true;
        all_full = blocks[k].cost() <= single_limit && within(current(), budget);
    }
    if (all_full) {
        std::ranges::fill(target, full_quality_stage);
    } else {
        // Otherwise raise tiles one stage per round, the lowest first,
        // re-encoding a tile's block for the higher target, while all streams
        // fit the budget. After a few blocks that do not fit, the budget is
        // taken to be used up.
        std::ranges::fill(has, false);
        constexpr std::size_t max_misses = 8;
        std::size_t misses = 0;
        for (;;) {
            std::uint8_t lowest = full_quality_stage;
            for (std::size_t k = 0; k < count; ++k) {
                if (!stuck[k]) {
                    lowest = std::min(lowest, target[k]);
                }
            }
            if (lowest == full_quality_stage || misses >= max_misses) {
                break;
            }
            bool raised = false;
            for (std::size_t k = 0; k < count && misses < max_misses; ++k) {
                if (stuck[k] || target[k] != lowest) {
                    continue;
                }
                const std::uint32_t index = candidates[k];
                Block next = encode_upgrade(*states_[index], index, static_cast<std::uint8_t>(target[k] + 1));
                if (next.cost() > single_limit) {
                    // Cannot go out even alone in a stream (only with tiny
                    // max_bytes): give up refining this tile.
                    stuck[k] = true;
                    hopeless[k] = !has[k];
                    continue;
                }
                const bool had = has[k];
                std::swap(blocks[k], next);
                has[k] = true;
                if (within(current(), budget)) {
                    ++target[k];
                    raised = true;
                } else {
                    std::swap(blocks[k], next);
                    has[k] = had;
                    stuck[k] = true;
                    ++misses;
                }
            }
            if (!raised) {
                break;
            }
        }
    }

    for (std::size_t k = 0; k < count; ++k) {
        if (hopeless[k]) {
            drop_state(candidates[k]);
        }
    }
    const auto pointers = current();
    if (pointers.empty()) {
        return {};
    }
    for (std::size_t k = 0; k < count; ++k) {
        if (!has[k]) {
            continue;
        }
        const std::uint32_t index = candidates[k];
        TileState& state = *states_[index];
        state.stage = target[k];
        cursor_ = (index + 1) % n;
        if (state.stage == full_quality_stage || state.complete_at(state.stage, rfx::extrapolate_layout)) {
            drop_state(index);
        }
    }
    ++frame_index_;
    return write_streams(group(pointers));
}

void Encoder::discard(std::span<const Rect> area)
{
    if (states_.empty() || select(area) == 0) {
        return;
    }
    for (std::size_t i = 0; i < selected_.size(); ++i) {
        if (selected_[i]) {
            drop_state(i);
        }
    }
}

bool Encoder::pending(std::span<const Rect> area)
{
    if (pending_ == 0 || select(area) == 0) {
        return false;
    }
    for (std::size_t i = 0; i < selected_.size(); ++i) {
        if (selected_[i] && states_[i]) {
            return true;
        }
    }
    return false;
}

std::uint8_t Encoder::tile_stage(std::uint32_t tx, std::uint32_t ty) const noexcept
{
    const std::size_t i = (std::size_t{ty} * grid_width_) + tx;
    if (tx >= grid_width_ || ty >= grid_height_ || i >= states_.size() || !states_[i]) {
        return full_quality_stage;
    }
    return states_[i]->stage;
}

}  // namespace farland::codec::progressive
