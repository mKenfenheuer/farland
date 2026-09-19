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

/// One quality stage of progressive refinement: an RFX_PROGRESSIVE_CODEC_QUANT
/// ([MS-RDPEGFX] 2.2.4.2.1.5.1) whose extra quantization (BitPos, 3.1.8.1.3)
/// applies to Y, Cb and Cr alike.
struct QualityStage {
    std::uint8_t quality = 100;  ///< informational, 0..100
    rfx::Quant shift{};          ///< bits dropped per band, 0..8
};

/// Extra quantization per DWT level (level-1, level-2 and level-3 high
/// bands); LL3 is never reduced (see Encoder).
[[nodiscard]] constexpr rfx::Quant stage_shift(std::uint8_t level1, std::uint8_t level2, std::uint8_t level3) noexcept
{
    using rfx::Band;
    rfx::Quant q;
    q[Band::hl1] = q[Band::lh1] = q[Band::hh1] = level1;
    q[Band::hl2] = q[Band::lh2] = q[Band::hh2] = level2;
    q[Band::hl3] = q[Band::lh3] = q[Band::hh3] = level3;
    return q;
}

/// The stages a refined tile goes through: TILE_FIRST at stage 0, then
/// TILE_UPGRADE to later stages; the last is full quality (no extra
/// quantization), the same coefficients a single pass sends.
inline constexpr std::array<QualityStage, 4> quality_stages{{
    {.quality = 25, .shift = stage_shift(4, 3, 2)},
    {.quality = 50, .shift = stage_shift(2, 2, 1)},
    {.quality = 75, .shift = stage_shift(1, 1, 0)},
    {.quality = 100, .shift = stage_shift(0, 0, 0)},
}};
inline constexpr std::uint8_t full_quality_stage = quality_stages.size() - 1;

struct EncoderOptions {
    /// Quantization for every component. Factors 6 (finest) to 15.
    rfx::Quant quant = quant_default;
    /// Upper bound for each stream, min_max_bytes..max_max_bytes.
    std::size_t max_bytes = default_max_bytes;
    /// Start every stream with RFX_PROGRESSIVE_SYNC, as FreeRDP and macRDP do.
    /// When false, only the first stream after construction or reset() has it.
    bool sync_every_stream = true;
    /// The reduce-extrapolate DWT (RFX_DWT_REDUCE_EXTRAPOLATE, [MS-RDPEGFX]
    /// 3.2.8.1.2.2), which avoids visible tile edges. Off: the classic DWT.
    bool reduce_extrapolate = false;
    /// Progressive refinement: tiles go out at quality_stages[0] and
    /// upgrade() raises them. Needs reduce_extrapolate (asserted): FreeRDP
    /// reads upgrade passes in the reduce-extrapolate band layout whatever
    /// the region's DWT flag says, so with the classic DWT it would decode
    /// upgrades differently from a decoder that follows the specification.
    bool refine = false;
};

/// Encoder for one surface (one codec context).
///
/// Single pass (the default): every tile that intersects the damage is sent
/// as RFX_PROGRESSIVE_TILE_FIRST ([MS-RDPEGFX] 2.2.4.2.1.5.4) at full
/// progressive quality: the region carries one RFX_PROGRESSIVE_CODEC_QUANT of
/// quality 100 with all extra shifts zero, and the tile's progressiveQuality
/// points at it. With the classic DWT this is the profile macRDP ships to
/// mstsc.
///
/// Refinement (EncoderOptions::refine): encode() sends each damaged tile as
/// TILE_FIRST at quality stage 0 and keeps its quantized coefficients; each
/// upgrade() call then sends TILE_UPGRADE blocks (SRL and RAW bits,
/// 3.2.8.1.5.2) that raise tiles to later stages until they reach full
/// quality, where they equal a single-pass tile bit for bit. A tile damaged
/// again starts over with TILE_FIRST. Every region carries all
/// quality_stages tables, and the context sets RFX_SUBBAND_DIFFING (FreeRDP
/// warns about upgrades without it); no difference tiles are sent.
///
/// FreeRDP decodes the reduce-extrapolate DWT with the rounding of its C code
/// (progressive_rfx_idwt_x/y), which farland's decoder shares, except in
/// its NEON build (aarch64): rfx_neon.c rounds the even lifting step up
/// ((a + b + 1) >> 1 instead of (a + b) / 2), so pixels there can differ by 1.
///
/// Entropy coding is always RLGR1: [MS-RDPEGFX] 2.2.4.2.1.5.3 and .4 require
/// it for TILE_SIMPLE and TILE_FIRST and a stream has no field to announce
/// RLGR3 (FreeRDP's progressive decoder hard-codes RLGR1). rfx::rlgr_encode
/// and rfx::rlgr_decode do both modes for RemoteFX proper.
///
/// Output streams never exceed EncoderOptions::max_bytes. Tiles are dealt
/// across as many streams as needed; a tile too large for a stream on its own
/// is re-encoded with coarser quantization, down to its average colour (such
/// a tile is not refined further).
///
/// Driving refinement from the graphics pipeline, one RDPGFX frame at a time:
///   1. encode(frame, damage) for the tiles that changed; send the streams.
///   2. If the frame has bandwidth left (the congestion controller's byte
///      budget minus what step 1 produced), upgrade(static_area, left) for
///      the regions the classifier considers static, or upgrade(left) for
///      all of them. Tiles at the lowest stage go first, a stage at a time,
///      and the streams add up to at most `left` bytes. A 1080p photo at
///      quant_default takes about 75 KB for the first pass and 290 KB of
///      upgrades (9% more than one full-quality pass), so 16 KB per frame at
///      30 fps refines it in about 0.6 s; at quant_highest the upgrades are
///      about 1.6 MB. Encoding upgrades costs about 5 ms per 100 KB on one
///      x86-64 core (bench-progressive).
///   3. Before painting pixels with another codec (planar, ClearCodec,
///      AVC, SolidFill, cache or surface copies), discard() those tiles:
///      decoders copy a progressive tile's whole pixels again on every
///      upgrade, which would overwrite the other codec's output.
///   4. reset() when the client loses its codec context (new surface,
///      ResetGraphics): that drops the pending refinement too.
/// A caller that never calls upgrade() leaves refined tiles at stage 0.
class Encoder {
public:
    /// Asserts width and height in 1..max_dimension and valid options.
    Encoder(std::uint32_t width, std::uint32_t height, const EncoderOptions& options = {});
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    Encoder(Encoder&&) noexcept;
    Encoder& operator=(Encoder&&) noexcept;
    ~Encoder();

    /// How the first pass of a tile is coded while refinement is on.
    enum class Pass : std::uint8_t {
        /// TILE_FIRST at quality_stages[0]; upgrade() raises it from there.
        refined,
        /// TILE_FIRST at full quality, the same coefficients a single pass
        /// sends, and nothing left to upgrade. For damage small enough that
        /// the bytes do not matter: a caret, a spinner or a clock changes
        /// the same tile over and over, and would otherwise never leave the
        /// coarsest stage, which is exactly where the eye rests.
        direct,
    };

    /// Encodes the 64 x 64 tiles of `image` that intersect `damage` (surface
    /// coordinates, clipped to the surface) into complete streams, one per
    /// WIRE_TO_SURFACE_2 PDU. No damage gives no streams. `image` must match
    /// the surface size (asserted).
    [[nodiscard]] std::vector<std::vector<std::byte>> encode(const ImageView& image, std::span<const Rect> damage,
                                                             Pass pass = Pass::refined);

    /// Refinement: TILE_UPGRADE blocks for the tiles that intersect `area`
    /// and are below full quality, in streams like encode()'s whose sizes
    /// add up to at most `budget` bytes. Lowest stage first, rotating among
    /// equals across calls; a tile may rise several stages in one block when
    /// the budget allows. No streams when nothing there needs refining, the
    /// budget is too small for the first block, or refine is off.
    [[nodiscard]] std::vector<std::vector<std::byte>> upgrade(std::span<const Rect> area, std::size_t budget);
    /// upgrade() over the whole surface.
    [[nodiscard]] std::vector<std::vector<std::byte>> upgrade(std::size_t budget);

    /// Forgets the refinement of the tiles that intersect `area` (their
    /// pixels are about to come from another codec).
    void discard(std::span<const Rect> area);
    /// Tiles waiting for upgrade().
    [[nodiscard]] std::size_t pending_tiles() const noexcept { return pending_; }
    /// Whether any tile that intersects `area` waits for upgrade().
    [[nodiscard]] bool pending(std::span<const Rect> area);
    /// The quality stage the client has for tile (tx, ty): full_quality_stage
    /// unless the tile waits for upgrade().
    [[nodiscard]] std::uint8_t tile_stage(std::uint32_t tx, std::uint32_t ty) const noexcept;

    /// Changes the quantization for later encode() calls (factors 6..15).
    /// Tiles already sent are refined with the quantization they started with.
    void set_quant(const rfx::Quant& quant);
    /// The next stream starts with SYNC again (new codec context on the
    /// client), and pending refinement is dropped.
    void reset() noexcept;

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

private:
    struct Block;
    struct TileState;
    using Group = std::vector<const Block*>;

    /// Marks the tiles that intersect `rects` in selected_; returns how many.
    std::size_t select(std::span<const Rect> rects);
    /// Codes one tile at quality stage `stage` (0 unless refinement is on).
    [[nodiscard]] Block encode_tile(const ImageView& image, std::uint32_t tx, std::uint32_t ty, std::uint8_t stage = 0);
    void transform(const rfx::Planes& planes);
    void code_components(std::size_t level, std::uint8_t stage, Block& out);
    [[nodiscard]] Block encode_upgrade(const TileState& state, std::uint32_t index, std::uint8_t to) const;
    [[nodiscard]] std::size_t stream_overhead(std::size_t quant_tables, bool sync) const noexcept;
    [[nodiscard]] std::size_t stream_size(const Group& group, bool sync) const noexcept;
    /// Deals blocks over streams (round-robin, then greedy by size).
    [[nodiscard]] std::vector<Group> group(std::span<const Block* const> blocks) const;
    [[nodiscard]] std::size_t total_size(const std::vector<Group>& groups) const noexcept;
    /// Whether the streams for `blocks` add up to at most `budget` bytes.
    [[nodiscard]] bool within(std::span<const Block* const> blocks, std::size_t budget) const;
    [[nodiscard]] std::vector<std::byte> write_stream(const Group& blocks);
    [[nodiscard]] std::vector<std::vector<std::byte>> write_streams(const std::vector<Group>& groups);
    void drop_state(std::size_t index) noexcept;

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
    std::unique_ptr<std::array<rfx::Coefficients, 3>> quantized_;  // of the last code_components()
    std::unique_ptr<rfx::Coefficients> scratch_;
    // Refinement: per grid tile, the state of a tile below full quality.
    std::vector<std::unique_ptr<TileState>> states_;
    std::size_t pending_ = 0;
    std::size_t cursor_ = 0;  // where upgrade() starts among tiles of equal stage
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
