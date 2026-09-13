// SPDX-FileCopyrightText: 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
// SPDX-FileCopyrightText: 2019 Armin Novak <armin.novak@thincast.com>
// SPDX-FileCopyrightText: 2019 Thincast Technologies GmbH
// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// RemoteFX Progressive decoder, [MS-RDPEGFX] 2.2.4.2 and 3.3.8.2.
//
// Translated from FreeRDP 3's libfreerdp/codec/progressive.c (Apache-2.0),
// cross-checked against ZeroVDI's port of it (same author as farland).
// Modified: all input goes through farland::Reader; the per-surface state is
// this class; tiles are allocated on first use; and some inputs that FreeRDP
// accepts or silently skips are errors here (listed in the report of the
// commit that added this file and in progressive.hpp):
//   - a tile index outside the surface grid (FreeRDP accepts one extra column
//     and aliases larger indices onto other tiles);
//   - a tile that fails to decode (bad quantIdx or progressiveQuality, empty
//     RLGR data, a dequantization shift of 16 or more, an upgrade that lowers
//     no bit position), which FreeRDP skips without failing the stream;
//   - more rect-by-tile work than max_blit_pairs when copying to the surface.

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/codec/progressive.hpp>
#include <farland/codec/rfx_common.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
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
constexpr std::uint16_t wbt_tile_simple = 0xCCC5;
constexpr std::uint16_t wbt_tile_first = 0xCCC6;
constexpr std::uint16_t wbt_tile_upgrade = 0xCCC7;

constexpr std::size_t block_header_size = 6;
constexpr std::uint32_t sync_magic = 0xCACCACCA;
constexpr std::uint16_t sync_version = 0x0100;

constexpr std::uint8_t tile_difference = 0x01;     // RFX_TILE_DIFFERENCE, 2.2.4.2.1.5.3
constexpr std::uint8_t reduce_extrapolate = 0x01;  // RFX_DWT_REDUCE_EXTRAPOLATE, 2.2.4.2.1.5
constexpr std::uint8_t full_quality = 0xFF;        // progressiveQuality, 2.2.4.2.1.5.4

// Which blocks a stream has shown so far (FreeRDP WBT_STATE_FLAG).
constexpr std::uint8_t seen_sync = 0x01;
constexpr std::uint8_t seen_frame_begin = 0x02;
constexpr std::uint8_t seen_frame_end = 0x04;
constexpr std::uint8_t seen_context = 0x08;

constexpr std::size_t tile = rfx::tile_size;
constexpr std::size_t tile_bytes = tile * tile * 4;

/// Bound on (region rect, overlapped tile) pairs examined when copying tiles
/// to the surface: 64 times a full 8192 x 8192 surface covered by one rect.
constexpr std::uint64_t max_blit_pairs = std::uint64_t{1} << 20;

using ComponentQuants = std::array<rfx::Quant, 3>;  // Y, Cb, Cr

/// The all-zero RFX_PROGRESSIVE_CODEC_QUANT that progressiveQuality 0xFF
/// selects (FreeRDP quantProgValFull).
constexpr ComponentQuants no_extra_quant{};

[[nodiscard]] constexpr std::int16_t saturating_add(std::int16_t a, std::int16_t b) noexcept
{
    return static_cast<std::int16_t>(std::clamp<std::int32_t>(std::int32_t{a} + b, INT16_MIN, INT16_MAX));
}

}  // namespace

struct Decoder::Tile {
    std::uint32_t x_idx = 0;
    std::uint32_t y_idx = 0;
    // Persistent state ([MS-RDPEGFX] 3.3.1.2, 3.3.1.3): dequantized DWT
    // coefficients (DecDwtQ), the tri-state Sign of each coefficient (FreeRDP
    // keeps the raw first-pass values), and BitPos per band.
    std::array<rfx::Coefficients, 3> current{};
    std::array<rfx::Coefficients, 3> sign{};
    ComponentQuants bit_pos{};
    std::array<std::byte, tile_bytes> pixels{};
    bool in_frame = false;

    // The block most recently parsed for this tile. As in FreeRDP, the fields
    // live in the tile, so a tile listed twice in a region decodes its last
    // block twice.
    std::uint16_t block_type = 0;
    std::array<std::uint8_t, 3> quant_idx{};
    std::uint8_t flags = 0;
    std::uint8_t quality = 0;
    std::array<std::span<const std::byte>, 3> data{};  // TILE_SIMPLE / TILE_FIRST
    std::array<std::span<const std::byte>, 3> srl{};   // TILE_UPGRADE
    std::array<std::span<const std::byte>, 3> raw{};   // TILE_UPGRADE

    Tile() { pixels.fill(std::byte{0xFF}); }  // FreeRDP starts tiles white
};

struct Decoder::Region {
    std::uint8_t flags = 0;
    std::uint16_t num_tiles = 0;
    std::vector<rfx::Quant> quants;
    std::vector<ComponentQuants> prog_quants;
    std::vector<std::uint32_t> tiles;  // tile indices in block order
};

struct Decoder::Frame {
    std::uint8_t seen = 0;
};

Decoder::Decoder(std::uint32_t width, std::uint32_t height)
    : width_(width), height_(height), grid_width_(static_cast<std::uint32_t>((width + tile - 1) / tile)),
      grid_height_(static_cast<std::uint32_t>((height + tile - 1) / tile)),
      pixels_(std::size_t{width} * height * 4, std::byte{0}),
      tiles_(static_cast<std::size_t>(grid_width_) * grid_height_), planes_(std::make_unique<rfx::Planes>()),
      scratch_(std::make_unique<rfx::Coefficients>())
{
    for (std::size_t i = 3; i < pixels_.size(); i += 4) {
        pixels_[i] = std::byte{0xFF};
    }
}

Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;
Decoder::~Decoder() = default;

Result<Decoder> Decoder::create(std::uint32_t width, std::uint32_t height)
{
    if (width == 0 || height == 0) {
        return fail(Errc::invalid_value, "progressive surface has no pixels");
    }
    if (width > max_dimension || height > max_dimension) {
        return fail(Errc::limit_exceeded, "progressive surface exceeds progressive::max_dimension");
    }
    return Decoder(width, height);
}

ImageView Decoder::image() const noexcept
{
    return {.data = pixels_, .width = width_, .height = height_, .stride = std::size_t{width_} * 4};
}

Decoder::Tile& Decoder::tile_at(std::size_t index)
{
    auto& slot = tiles_.at(index);
    if (!slot) {
        slot = std::make_unique<Tile>();
        slot->x_idx = static_cast<std::uint32_t>(index % grid_width_);
        slot->y_idx = static_cast<std::uint32_t>(index / grid_width_);
    }
    return *slot;
}

Result<void> Decoder::decode(std::span<const std::byte> stream, std::uint32_t frame_id)
{
    // Tiles of one RDPGFX frame accumulate across streams (FreeRDP
    // progressive_decompress, surface->frameId).
    if (frame_id != frame_id_) {
        frame_id_ = frame_id;
        for (const std::uint32_t index : frame_tiles_) {
            tiles_.at(index)->in_frame = false;
        }
        frame_tiles_.clear();
    }
    Reader r(stream);
    Frame frame;
    while (!r.empty()) {
        FARLAND_TRY_VOID(parse_block(r, frame));
    }
    return update_surface();
}

/// One RFX_PROGRESSIVE_DATABLOCK, [MS-RDPEGFX] 2.2.4.2.1.
Result<void> Decoder::parse_block(Reader& r, Frame& frame)
{
    const std::size_t at = r.offset();
    FARLAND_TRY(const auto block_type, r.u16le());
    FARLAND_TRY(const auto block_len, r.u32le());
    if (block_len < block_header_size) {
        return fail(Errc::invalid_length, "progressive blockLen is shorter than the block header", at);
    }
    FARLAND_TRY(auto body, r.sub(block_len - block_header_size));

    switch (block_type) {
    case wbt_sync: {
        // RFX_PROGRESSIVE_SYNC, 2.2.4.2.1.1.
        if (block_len != 12) {
            return fail(Errc::invalid_length, "RFX_PROGRESSIVE_SYNC blockLen is not 12", at);
        }
        FARLAND_TRY(const auto magic, body.u32le());
        FARLAND_TRY(const auto version, body.u16le());
        if (magic != sync_magic) {
            return fail(Errc::invalid_value, "RFX_PROGRESSIVE_SYNC magic is not 0xCACCACCA", at);
        }
        if (version != sync_version) {
            return fail(Errc::unsupported, "RFX_PROGRESSIVE_SYNC version is not 1.0", at);
        }
        frame.seen |= seen_sync;  // a duplicate is ignored
        break;
    }
    case wbt_frame_begin: {
        // RFX_PROGRESSIVE_FRAME_BEGIN, 2.2.4.2.1.2; frameIndex and
        // regionCount are not used.
        if (block_len != 12) {
            return fail(Errc::invalid_length, "RFX_PROGRESSIVE_FRAME_BEGIN blockLen is not 12", at);
        }
        FARLAND_TRY_VOID(body.skip(6));
        if ((frame.seen & (seen_frame_begin | seen_frame_end)) != 0) {
            return fail(Errc::invalid_value, "RFX_PROGRESSIVE_FRAME_BEGIN repeated or after FRAME_END", at);
        }
        frame.seen |= seen_frame_begin;
        break;
    }
    case wbt_frame_end:
        // RFX_PROGRESSIVE_FRAME_END, 2.2.4.2.1.3.
        if (block_len != 6) {
            return fail(Errc::invalid_length, "RFX_PROGRESSIVE_FRAME_END blockLen is not 6", at);
        }
        frame.seen |= seen_frame_end;
        break;
    case wbt_context: {
        // RFX_PROGRESSIVE_CONTEXT, 2.2.4.2.1.4; ctxId is ignored.
        if (block_len != 10) {
            return fail(Errc::invalid_length, "RFX_PROGRESSIVE_CONTEXT blockLen is not 10", at);
        }
        FARLAND_TRY_VOID(body.skip(1));
        FARLAND_TRY(const auto tile_size, body.u16le());
        FARLAND_TRY(const auto flags, body.u8());
        if (tile_size != tile) {
            return fail(Errc::invalid_value, "RFX_PROGRESSIVE_CONTEXT tileSize is not 64", at);
        }
        context_flags_ = flags;
        frame.seen |= seen_context;
        break;
    }
    case wbt_region: {
        // A region outside FRAME_BEGIN .. FRAME_END is checked and ignored.
        const bool inside = (frame.seen & seen_frame_begin) != 0 && (frame.seen & seen_frame_end) == 0;
        FARLAND_TRY_VOID(parse_region(body, !inside));
        break;
    }
    default:
        return fail(Errc::invalid_value, "unknown RFX_PROGRESSIVE block type", at);
    }
    return body.expect_end("RFX_PROGRESSIVE block has trailing data");
}

/// RFX_PROGRESSIVE_REGION, [MS-RDPEGFX] 2.2.4.2.1.5.
Result<void> Decoder::parse_region(Reader& r, bool skip)
{
    const std::size_t at = r.offset();
    FARLAND_TRY(const auto tile_size, r.u8());
    FARLAND_TRY(const auto num_rects, r.u16le());
    FARLAND_TRY(const auto num_quant, r.u8());
    FARLAND_TRY(const auto num_prog_quant, r.u8());
    FARLAND_TRY(const auto flags, r.u8());
    FARLAND_TRY(const auto num_tiles, r.u16le());
    FARLAND_TRY(const auto tile_data_size, r.u32le());
    if (tile_size != tile) {
        return fail(Errc::invalid_value, "RFX_PROGRESSIVE_REGION tileSize is not 64", at);
    }
    if (num_rects == 0) {
        return fail(Errc::invalid_value, "RFX_PROGRESSIVE_REGION has no rects", at);
    }
    if (num_quant > 7) {
        return fail(Errc::invalid_value, "RFX_PROGRESSIVE_REGION numQuant is above 7", at);
    }
    // rects, quantVals, quantProgVals and tiles fill the block exactly.
    const std::uint64_t expected = (std::uint64_t{num_rects} * 8) + (std::uint64_t{num_quant} * 5) +
                                   (std::uint64_t{num_prog_quant} * 16) + tile_data_size;
    if (expected != r.remaining()) {
        return fail(Errc::invalid_length, "RFX_PROGRESSIVE_REGION sizes disagree with blockLen", at);
    }
    if (skip) {
        return r.skip(r.remaining());
    }

    std::vector<Rect> rects(num_rects);
    for (Rect& rect : rects) {
        // TS_RFX_RECT, [MS-RDPRFX] 2.2.2.1.6.
        FARLAND_TRY(rect.x, r.u16le());
        FARLAND_TRY(rect.y, r.u16le());
        FARLAND_TRY(rect.width, r.u16le());
        FARLAND_TRY(rect.height, r.u16le());
    }
    Region region;
    region.flags = flags;
    region.num_tiles = num_tiles;
    region.quants.reserve(num_quant);
    for (std::size_t i = 0; i < num_quant; ++i) {
        const std::size_t quant_at = r.offset();
        FARLAND_TRY(const auto q, rfx::read_component_quant(r));
        if (!std::ranges::all_of(q.bands, [](std::uint8_t v) { return v >= 6 && v <= 15; })) {
            return fail(Errc::invalid_value, "RFX_COMPONENT_CODEC_QUANT factor outside 6..15", quant_at);
        }
        region.quants.push_back(q);
    }
    region.prog_quants.reserve(num_prog_quant);
    for (std::size_t i = 0; i < num_prog_quant; ++i) {
        // RFX_PROGRESSIVE_CODEC_QUANT, 2.2.4.2.1.5.1; quality is not used.
        FARLAND_TRY_VOID(r.skip(1));
        ComponentQuants q;
        for (auto& component : q) {
            FARLAND_TRY(component, rfx::read_component_quant(r));
        }
        region.prog_quants.push_back(q);
    }
    rects_ = std::move(rects);

    FARLAND_TRY(auto tiles, r.sub(tile_data_size));
    while (!tiles.empty()) {
        FARLAND_TRY_VOID(parse_tile(tiles, region));
    }
    if (region.tiles.size() != num_tiles) {
        return fail(Errc::invalid_length, "RFX_PROGRESSIVE_REGION numTiles disagrees with its tiles", at);
    }
    for (const std::uint32_t index : region.tiles) {
        Tile& t = *tiles_.at(index);
        if (t.block_type == wbt_tile_upgrade) {
            FARLAND_TRY_VOID(decode_upgrade(t, region));
        } else {
            FARLAND_TRY_VOID(decode_first(t, region));
        }
    }
    return {};
}

/// RFX_PROGRESSIVE_TILE_SIMPLE, _FIRST or _UPGRADE, [MS-RDPEGFX]
/// 2.2.4.2.1.5.3 to 2.2.4.2.1.5.5.
Result<void> Decoder::parse_tile(Reader& r, Region& region)
{
    const std::size_t at = r.offset();
    FARLAND_TRY(const auto block_type, r.u16le());
    FARLAND_TRY(const auto block_len, r.u32le());
    if (block_len < block_header_size) {
        return fail(Errc::invalid_length, "progressive tile blockLen is shorter than the block header", at);
    }
    FARLAND_TRY(auto b, r.sub(block_len - block_header_size));
    if (block_type != wbt_tile_simple && block_type != wbt_tile_first && block_type != wbt_tile_upgrade) {
        return fail(Errc::invalid_value, "RFX_PROGRESSIVE_REGION holds a block that is not a tile", at);
    }

    std::array<std::uint8_t, 3> quant_idx{};
    for (auto& idx : quant_idx) {
        FARLAND_TRY(idx, b.u8());
    }
    FARLAND_TRY(const auto x_idx, b.u16le());
    FARLAND_TRY(const auto y_idx, b.u16le());
    std::uint8_t flags = 0;
    std::uint8_t quality = full_quality;
    std::array<std::span<const std::byte>, 3> data{};
    std::array<std::span<const std::byte>, 3> srl{};
    std::array<std::span<const std::byte>, 3> raw{};
    if (block_type == wbt_tile_upgrade) {
        FARLAND_TRY(quality, b.u8());
        std::array<std::uint16_t, 6> lengths{};  // ySrlLen, yRawLen, cbSrlLen, ...
        for (auto& length : lengths) {
            FARLAND_TRY(length, b.u16le());
        }
        for (std::size_t c = 0; c < 3; ++c) {
            FARLAND_TRY(srl.at(c), b.bytes(lengths.at(2 * c)));
            FARLAND_TRY(raw.at(c), b.bytes(lengths.at((2 * c) + 1)));
        }
    } else {
        FARLAND_TRY(flags, b.u8());
        if (block_type == wbt_tile_first) {
            FARLAND_TRY(quality, b.u8());
        }
        std::array<std::uint16_t, 4> lengths{};  // yLen, cbLen, crLen, tailLen
        for (auto& length : lengths) {
            FARLAND_TRY(length, b.u16le());
        }
        for (std::size_t c = 0; c < 3; ++c) {
            FARLAND_TRY(data.at(c), b.bytes(lengths.at(c)));
        }
        FARLAND_TRY_VOID(b.skip(lengths[3]));  // tailData is ignored
    }
    FARLAND_TRY_VOID(b.expect_end("progressive tile block has trailing data"));

    if (x_idx >= grid_width_ || y_idx >= grid_height_) {
        return fail(Errc::invalid_value, "progressive tile lies outside the surface", at);
    }
    if (region.tiles.size() >= region.num_tiles) {
        return fail(Errc::invalid_length, "RFX_PROGRESSIVE_REGION has more tiles than numTiles", at);
    }
    const std::size_t index = (std::size_t{y_idx} * grid_width_) + x_idx;
    Tile& t = tile_at(index);
    t.block_type = block_type;
    t.quant_idx = quant_idx;
    t.flags = flags;
    t.quality = quality;
    t.data = data;
    t.srl = srl;
    t.raw = raw;
    region.tiles.push_back(static_cast<std::uint32_t>(index));
    if (!t.in_frame) {
        t.in_frame = true;
        frame_tiles_.push_back(static_cast<std::uint32_t>(index));
    }
    return {};
}

namespace {

/// The base and progressive quantization of a tile's components
/// (progressive_decompress_tile_first/_upgrade).
struct TileQuant {
    std::array<const rfx::Quant*, 3> base{};
    const ComponentQuants* prog = nullptr;
};

template <class Region>
[[nodiscard]] Result<TileQuant> tile_quant(const std::array<std::uint8_t, 3>& quant_idx, std::uint8_t quality,
                                           const Region& region) noexcept
{
    TileQuant out;
    for (std::size_t c = 0; c < 3; ++c) {
        if (quant_idx[c] >= region.quants.size()) {
            return fail(Errc::invalid_value, "progressive tile quantIdx has no quantization table");
        }
        out.base[c] = &region.quants[quant_idx[c]];
    }
    if (quality == full_quality) {
        out.prog = &no_extra_quant;
    } else if (quality < region.prog_quants.size()) {
        out.prog = &region.prog_quants[quality];
    } else {
        return fail(Errc::invalid_value, "progressive tile progressiveQuality has no quantization table");
    }
    return out;
}

/// quant + prog per band: the BitPos of [MS-RDPEGFX] 3.1.8.1.3.
[[nodiscard]] rfx::Quant add(const rfx::Quant& a, const rfx::Quant& b) noexcept
{
    rfx::Quant out;
    for (std::size_t i = 0; i < rfx::band_count; ++i) {
        out.bands[i] = static_cast<std::uint8_t>(a.bands[i] + b.bands[i]);
    }
    return out;
}

/// The dequantization shift: BitPos - 1 (the quantizer's -6 and the colour
/// transform's +5). Base factors are at least 6, so this cannot underflow.
[[nodiscard]] rfx::Quant shift_of(const rfx::Quant& bit_pos) noexcept
{
    rfx::Quant out;
    for (std::size_t i = 0; i < rfx::band_count; ++i) {
        out.bands[i] = static_cast<std::uint8_t>(bit_pos.bands[i] - 1);
    }
    return out;
}

[[nodiscard]] std::span<std::int16_t> band_of(rfx::Coefficients& c, const rfx::Layout& layout, rfx::Band band)
{
    const auto& b = layout.at(std::to_underlying(band));
    return std::span(c).subspan(b.offset, b.size);
}

}  // namespace

/// First pass of a tile (TILE_SIMPLE or TILE_FIRST), [MS-RDPEGFX] 3.3.8.2.1.1
/// (FreeRDP progressive_decompress_tile_first).
Result<void> Decoder::decode_first(Tile& t, const Region& region)
{
    FARLAND_TRY(const auto quant, tile_quant(t.quant_idx, t.quality, region));
    const bool difference = (t.flags & tile_difference) != 0;
    const bool extrapolate = (region.flags & reduce_extrapolate) != 0;
    const rfx::Layout& layout = extrapolate ? rfx::extrapolate_layout : rfx::standard_layout;

    const std::array<rfx::Coefficients*, 3> buffers{&planes_->y, &planes_->cb, &planes_->cr};
    for (std::size_t c = 0; c < 3; ++c) {
        const rfx::Quant bit_pos = add(*quant.base.at(c), quant.prog->at(c));
        t.bit_pos.at(c) = bit_pos;
        rfx::Coefficients& buf = *buffers.at(c);
        FARLAND_TRY_VOID(rfx::rlgr_decode(rfx::RlgrMode::rlgr1, t.data.at(c), buf));
        t.sign.at(c) = buf;
        rfx::differential_decode(band_of(buf, layout, rfx::Band::ll3));
        if (!rfx::dequantize(buf, layout, shift_of(bit_pos))) {
            return fail(Errc::invalid_value, "progressive dequantization shift is 16 or more");
        }
        rfx::Coefficients& current = t.current.at(c);
        if (difference) {
            for (std::size_t i = 0; i < buf.size(); ++i) {
                buf[i] = saturating_add(buf[i], current[i]);
            }
        }
        current = buf;
        if (extrapolate) {
            rfx::dwt_decode_extrapolate(buf, *scratch_);
        } else {
            rfx::dwt_decode(buf, *scratch_);
        }
    }
    rfx::store_tile(*planes_, t.pixels);
    return {};
}

/// Upgrade pass (TILE_UPGRADE), [MS-RDPEGFX] 3.3.8.2.1.2 (FreeRDP
/// progressive_decompress_tile_upgrade).
Result<void> Decoder::decode_upgrade(Tile& t, const Region& region)
{
    FARLAND_TRY(const auto quant, tile_quant(t.quant_idx, t.quality, region));
    const bool extrapolate = (region.flags & reduce_extrapolate) != 0;

    ComponentQuants bit_pos{};
    ComponentQuants num_bits{};
    for (std::size_t c = 0; c < 3; ++c) {
        bit_pos.at(c) = add(*quant.base.at(c), quant.prog->at(c));
        for (std::size_t b = 0; b < rfx::band_count; ++b) {
            const std::uint8_t before = t.bit_pos.at(c).bands.at(b);
            const std::uint8_t now = bit_pos.at(c).bands.at(b);
            if (before < now) {
                return fail(Errc::invalid_value, "progressive upgrade raises a band's bit position");
            }
            num_bits.at(c).bands.at(b) = static_cast<std::uint8_t>(before - now);
        }
    }
    t.bit_pos = bit_pos;

    const std::array<rfx::Coefficients*, 3> buffers{&planes_->y, &planes_->cb, &planes_->cr};
    for (std::size_t c = 0; c < 3; ++c) {
        const rfx::Quant shift = shift_of(bit_pos.at(c));
        rfx::UpgradeState state;
        state.srl = rfx::BitReader(t.srl.at(c));
        state.raw = rfx::BitReader(t.raw.at(c));
        // FreeRDP always walks the reduce-extrapolate band layout here.
        for (std::size_t b = 0; b < rfx::band_count; ++b) {
            const auto band = static_cast<rfx::Band>(b);
            rfx::upgrade_band(state, band_of(t.current.at(c), rfx::extrapolate_layout, band),
                              band_of(t.sign.at(c), rfx::extrapolate_layout, band), shift.bands.at(b),
                              num_bits.at(c).bands.at(b), band != rfx::Band::ll3);
        }
        rfx::Coefficients& buf = *buffers.at(c);
        buf = t.current.at(c);
        if (extrapolate) {
            rfx::dwt_decode_extrapolate(buf, *scratch_);
        } else {
            rfx::dwt_decode(buf, *scratch_);
        }
    }
    rfx::store_tile(*planes_, t.pixels);
    return {};
}

/// Copies every tile updated in this frame to the surface, clipped to the
/// rects of the last region (FreeRDP update_tiles).
Result<void> Decoder::update_surface()
{
    if (rects_.empty() || frame_tiles_.empty()) {
        return {};
    }
    const auto tile_range = [](std::uint64_t begin, std::uint64_t extent, std::uint32_t grid) {
        const std::uint64_t first = std::min<std::uint64_t>(begin / tile, grid);
        const std::uint64_t last = std::min<std::uint64_t>((begin + extent + tile - 1) / tile, grid);
        return std::pair{static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(last)};
    };
    std::uint64_t pairs = 0;
    for (const Rect& rect : rects_) {
        const auto [x0, x1] = tile_range(rect.x, rect.width, grid_width_);
        const auto [y0, y1] = tile_range(rect.y, rect.height, grid_height_);
        pairs += std::uint64_t{x1 - x0} * (y1 - y0);
        if (pairs > max_blit_pairs) {
            return fail(Errc::limit_exceeded, "progressive region rects cover too many tiles");
        }
    }

    // Per updated tile, which pixels the rects cover: one 64-bit mask per row.
    using Mask = std::array<std::uint64_t, tile>;
    std::vector<Mask> masks(frame_tiles_.size(), Mask{});
    std::vector<std::uint32_t> slot_of(tiles_.size(), UINT32_MAX);
    for (std::size_t k = 0; k < frame_tiles_.size(); ++k) {
        slot_of.at(frame_tiles_[k]) = static_cast<std::uint32_t>(k);
    }
    for (const Rect& rect : rects_) {
        const std::uint64_t right = std::uint64_t{rect.x} + rect.width;
        const std::uint64_t bottom = std::uint64_t{rect.y} + rect.height;
        const auto [x0, x1] = tile_range(rect.x, rect.width, grid_width_);
        const auto [y0, y1] = tile_range(rect.y, rect.height, grid_height_);
        for (std::uint32_t ty = y0; ty < y1; ++ty) {
            for (std::uint32_t tx = x0; tx < x1; ++tx) {
                const std::uint32_t slot = slot_of.at((std::size_t{ty} * grid_width_) + tx);
                if (slot == UINT32_MAX) {
                    continue;
                }
                const std::uint64_t left = std::uint64_t{tx} * tile;
                const std::uint64_t top = std::uint64_t{ty} * tile;
                const std::uint64_t ix0 = std::max<std::uint64_t>(rect.x, left);
                const std::uint64_t ix1 = std::min<std::uint64_t>(right, left + tile);
                const std::uint64_t iy0 = std::max<std::uint64_t>(rect.y, top);
                const std::uint64_t iy1 = std::min<std::uint64_t>(bottom, top + tile);
                if (ix0 >= ix1 || iy0 >= iy1) {
                    continue;
                }
                if (ix1 > width_ || iy1 > height_) {
                    return fail(Errc::invalid_value, "progressive region rect extends past the surface");
                }
                const std::uint64_t span_bits = ix1 - ix0;
                const std::uint64_t bits = (span_bits == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << span_bits) - 1)
                                           << (ix0 - left);
                for (std::uint64_t row = iy0 - top; row < iy1 - top; ++row) {
                    masks.at(slot).at(row) |= bits;
                }
            }
        }
    }

    const std::size_t stride = std::size_t{width_} * 4;
    for (std::size_t k = 0; k < frame_tiles_.size(); ++k) {
        const Tile& t = *tiles_.at(frame_tiles_[k]);
        const std::span<const std::byte> src(t.pixels);
        for (std::size_t row = 0; row < tile; ++row) {
            std::uint64_t m = masks[k].at(row);
            while (m != 0) {
                const auto start = static_cast<std::size_t>(std::countr_zero(m));
                const auto length = static_cast<std::size_t>(std::countr_one(m >> start));
                const std::size_t x = (std::size_t{t.x_idx} * tile) + start;
                const std::size_t y = (std::size_t{t.y_idx} * tile) + row;
                std::ranges::copy(src.subspan((row * tile * 4) + (start * 4), length * 4),
                                  std::span(pixels_).subspan((y * stride) + (x * 4), length * 4).begin());
                m = length + start >= 64 ? 0 : m & ~(((std::uint64_t{1} << length) - 1) << start);
            }
        }
    }
    return {};
}

}  // namespace farland::codec::progressive
