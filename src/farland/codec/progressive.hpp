// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/codec/image.hpp>
#include <farland/codec/rfx_common.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

/// RemoteFX Progressive codec: the RFX_PROGRESSIVE_BITMAP_STREAM of
/// [MS-RDPEGFX] 2.2.4.2, carried in RDPGFX_WIRE_TO_SURFACE_PDU_2 with codecId
/// RDPGFX_CODECID_CAPROGRESSIVE (0x0009), [MS-RDPEGFX] 2.2.2.2.
///
/// A stream is a sequence of blocks: SYNC, CONTEXT, FRAME_BEGIN, REGION (with
/// rects, quantization tables and tiles) and FRAME_END. Tiles are 64 x 64 and
/// sit on a grid anchored at the surface origin.
namespace farland::codec::progressive {

/// A rectangle in surface pixels.
struct Rect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    friend bool operator==(const Rect&, const Rect&) = default;
};

/// Largest surface width or height either direction accepts.
inline constexpr std::uint32_t max_dimension = 8192;

/// mstsc rejects WIRE_TO_SURFACE_2 PDUs whose progressive stream is much
/// larger than this (0x8007006f, docs/PLAN.md §4.1).
inline constexpr std::size_t default_max_bytes = 16384;
/// Smallest accepted stream budget: one worst-case fallback tile must fit.
inline constexpr std::size_t min_max_bytes = 1024;
/// Largest accepted stream budget: component lengths are 16-bit fields.
inline constexpr std::size_t max_max_bytes = 65535;

/// The finest quantization RemoteFX allows: every factor 6.
inline constexpr rfx::Quant quant_highest = rfx::uniform_quant(6);
/// FreeRDP's and macRDP's default table (rfx.c), in TS_RFX_CODEC_QUANT order
/// LL3 6, LH3 6, HL3 6, HH3 6, LH2 7, HL2 7, HH2 8, LH1 8, HL1 8, HH1 9.
inline constexpr rfx::Quant quant_default = rfx::quant_from_rfx_order({6, 6, 6, 6, 7, 7, 8, 8, 8, 9});

struct EncoderOptions {
    /// Quantization for every component. Factors 6 (finest) to 15.
    rfx::Quant quant = quant_default;
    /// Upper bound for each stream, min_max_bytes..max_max_bytes.
    std::size_t max_bytes = default_max_bytes;
    /// Start every stream with RFX_PROGRESSIVE_SYNC, as FreeRDP and macRDP do.
    /// When false, only the first stream after construction or reset() has it.
    bool sync_every_stream = true;
};

/// Encoder for one surface (one codec context).
///
/// Every tile that intersects the damage is sent as RFX_PROGRESSIVE_TILE_FIRST
/// ([MS-RDPEGFX] 2.2.4.2.1.5.4) at full progressive quality: the region
/// carries one RFX_PROGRESSIVE_CODEC_QUANT of quality 100 with all extra
/// shifts zero, and the tile's progressiveQuality points at it. The DWT is the
/// classic one (no RFX_DWT_REDUCE_EXTRAPOLATE), entropy coding is RLGR1, and
/// there is no sub-band diffing. This is the profile macRDP ships to mstsc.
///
/// Output streams never exceed EncoderOptions::max_bytes. Tiles are dealt
/// across as many streams as needed; a tile too large for a stream on its own
/// is re-encoded with coarser quantization, down to its average colour.
class Encoder {
public:
    /// Asserts width and height in 1..max_dimension and valid options.
    Encoder(std::uint32_t width, std::uint32_t height, const EncoderOptions& options = {});

    /// Encodes the 64 x 64 tiles of `image` that intersect `damage` (surface
    /// coordinates, clipped to the surface) into complete streams, one per
    /// WIRE_TO_SURFACE_2 PDU. No damage gives no streams. `image` must match
    /// the surface size (asserted).
    [[nodiscard]] std::vector<std::vector<std::byte>> encode(const ImageView& image, std::span<const Rect> damage);

    /// Changes the quantization for later encode() calls (factors 6..15).
    void set_quant(const rfx::Quant& quant);
    /// The next stream starts with SYNC again (new codec context on the client).
    void reset() noexcept { synced_ = false; }

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

private:
    struct EncodedTile;

    [[nodiscard]] EncodedTile encode_tile(const ImageView& image, std::uint32_t tx, std::uint32_t ty);
    void transform(const rfx::Planes& planes);
    void code_components(std::size_t level, EncodedTile& out);
    [[nodiscard]] static std::size_t stream_overhead(std::size_t quant_tables, bool sync) noexcept;
    [[nodiscard]] std::vector<std::byte> write_stream(std::span<const EncodedTile* const> tiles);

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t grid_width_ = 0;
    std::uint32_t grid_height_ = 0;
    EncoderOptions options_;
    std::vector<rfx::Quant> ladder_;  // [0] is options_.quant, then coarser
    std::uint32_t frame_index_ = 0;
    bool synced_ = false;
    std::vector<bool> selected_;
    std::unique_ptr<rfx::Planes> planes_;
    std::unique_ptr<std::array<rfx::Coefficients, 3>> transformed_;
    std::unique_ptr<rfx::Coefficients> scratch_;
};

/// Decoder for one surface, following FreeRDP's progressive.c: TILE_SIMPLE,
/// TILE_FIRST and TILE_UPGRADE (SRL and RAW), sub-band diffing (difference
/// tiles), both DWT variants, and per-tile state across calls.
///
/// Like FreeRDP, tiles decoded under the same RDPGFX frame id accumulate, and
/// at the end of every stream all of them are copied to the surface, clipped
/// to the rects of the last RFX_PROGRESSIVE_REGION.
class Decoder {
public:
    /// Fails for a width or height outside 1..max_dimension.
    [[nodiscard]] static Result<Decoder> create(std::uint32_t width, std::uint32_t height);

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;
    ~Decoder();

    /// Decodes one RFX_PROGRESSIVE_BITMAP_STREAM that belongs to RDPGFX frame
    /// `frame_id`. On error the surface and tile state may be partly updated.
    [[nodiscard]] Result<void> decode(std::span<const std::byte> stream, std::uint32_t frame_id);

    /// The surface: B, G, R, A (A = 0xFF), stride width * 4. It starts black.
    [[nodiscard]] ImageView image() const noexcept;

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

private:
    struct Tile;
    struct Region;
    struct Frame;

    Decoder(std::uint32_t width, std::uint32_t height);

    [[nodiscard]] Result<void> parse_block(Reader& r, Frame& frame);
    [[nodiscard]] Result<void> parse_region(Reader& r, bool skip);
    [[nodiscard]] Result<void> parse_tile(Reader& r, Region& region);
    [[nodiscard]] Result<void> decode_first(Tile& tile, const Region& region);
    [[nodiscard]] Result<void> decode_upgrade(Tile& tile, const Region& region);
    [[nodiscard]] Result<void> update_surface();
    [[nodiscard]] Tile& tile_at(std::size_t index);

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint32_t grid_width_ = 0;
    std::uint32_t grid_height_ = 0;
    std::vector<std::byte> pixels_;
    std::vector<std::unique_ptr<Tile>> tiles_;
    std::uint8_t context_flags_ = 0;
    std::vector<Rect> rects_;  // of the last RFX_PROGRESSIVE_REGION
    std::uint32_t frame_id_ = 0;
    std::vector<std::uint32_t> frame_tiles_;  // tiles updated in frame_id_
    std::unique_ptr<rfx::Planes> planes_;
    std::unique_ptr<rfx::Coefficients> scratch_;
};

}  // namespace farland::codec::progressive
