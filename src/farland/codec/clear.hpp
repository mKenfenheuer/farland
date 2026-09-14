// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/codec/image.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

/// ClearCodec: the CLEARCODEC_BITMAP_STREAM of [MS-RDPEGFX] 2.2.4.1, carried in
/// RDPGFX_WIRE_TO_SURFACE_PDU_1 with codecId RDPGFX_CODECID_CLEARCODEC
/// (0x0008) and decoded as in [MS-RDPEGFX] 3.3.8.1. It is lossless.
///
/// A stream paints one rectangle (the destRect of the PDU) in up to three
/// layers, each drawn over the one before:
///  - residual: BGR runs in raster order ([MS-RDPEGFX] 2.2.4.1.1.1);
///  - bands: horizontal bands of at most 52 rows, sent column by column as
///    "V-bars" that the client caches ([MS-RDPEGFX] 2.2.4.1.1.2);
///  - subcodecs: rectangles of raw BGR, NSCodec or RLEX palette runs
///    ([MS-RDPEGFX] 2.2.4.1.1.3).
/// A bitmap of at most 1,024 pixels can also be stored in the client's glyph
/// storage and later replayed from it with a 4-byte stream.
///
/// The client keeps one ClearCodec context per graphics channel, shared by
/// all surfaces ([MS-RDPEGFX] 3.3.1.9 to 3.3.1.13): the sequence number, the
/// V-Bar Storage (32,768 entries) and Short V-Bar Storage (16,384), each
/// filled at a cursor that wraps around, and the Decompressor Glyph Storage
/// (4,000 slots). FreeRDP keeps the storages across ResetGraphics as well;
/// its clear_context_reset() only restarts the sequence number.
namespace farland::codec::clear {

/// RDPGFX_CODECID_CLEARCODEC, [MS-RDPEGFX] 2.2.2.1.
inline constexpr std::uint16_t codec_id = 0x0008;

// CLEARCODEC_BITMAP_STREAM flags, [MS-RDPEGFX] 2.2.4.1.
inline constexpr std::uint8_t flag_glyph_index = 0x01;
inline constexpr std::uint8_t flag_glyph_hit = 0x02;
inline constexpr std::uint8_t flag_cache_reset = 0x04;

// CLEARCODEC_SUBCODEC subCodecId, [MS-RDPEGFX] 2.2.4.1.1.3.1.
inline constexpr std::uint8_t subcodec_uncompressed = 0x00;
inline constexpr std::uint8_t subcodec_nscodec = 0x01;
inline constexpr std::uint8_t subcodec_rlex = 0x02;

// Client storage sizes, [MS-RDPEGFX] 3.3.1.9 to 3.3.1.13.
inline constexpr std::size_t vbar_cache_size = 32768;
inline constexpr std::size_t short_vbar_cache_size = 16384;
inline constexpr std::size_t glyph_cache_size = 4000;
/// Largest bitmap (width * height) that may carry CLEARCODEC_FLAG_GLYPH_INDEX.
inline constexpr std::uint32_t max_glyph_pixels = 1024;
/// Largest CLEARCODEC_BAND height, [MS-RDPEGFX] 2.2.4.1.1.2.1.
inline constexpr std::uint32_t max_band_height = 52;
/// Largest CLEARCODEC_SUBCODEC_RLEX paletteCount, [MS-RDPEGFX] 2.2.4.1.1.3.1.1.
inline constexpr std::size_t max_palette_size = 127;

/// Largest width or height either direction accepts. The specification
/// allows 65,535; farland surfaces stop at 8,192.
inline constexpr std::uint32_t max_dimension = 8192;

/// Bits of stopIndex in a CLEARCODEC_SUBCODEC_RLEX_SEGMENT:
/// floor(log2(paletteCount - 1)) + 1, which FreeRDP evaluates as 1 for a
/// single-entry palette. suiteDepth has the remaining 8 - bits.
[[nodiscard]] constexpr unsigned rlex_index_bits(std::size_t palette_count) noexcept
{
    return palette_count <= 2 ? 1U : static_cast<unsigned>(std::bit_width(palette_count - 1));
}

struct EncodeOptions {
    /// Regions of at most max_glyph_pixels go through the glyph cache: a
    /// region seen before becomes a 4-byte glyph hit, and a new one is stored
    /// (2 bytes more than without the cache).
    bool glyph_cache = true;
};

/// What one Encoder::encode call produced, for tests, logs and benchmarks.
struct EncodeStats {
    bool glyph_hit = false;     ///< The stream is a 4-byte glyph hit.
    bool glyph_stored = false;  ///< The stream stores the region in the glyph cache.
    std::size_t residual_bytes = 0;
    std::size_t bands_bytes = 0;
    std::size_t subcodec_bytes = 0;
    std::size_t bands = 0;              ///< CLEARCODEC_BAND structures.
    std::size_t vbar_hits = 0;          ///< VBAR_CACHE_HIT
    std::size_t short_vbar_hits = 0;    ///< SHORT_VBAR_CACHE_HIT
    std::size_t short_vbar_misses = 0;  ///< SHORT_VBAR_CACHE_MISS
    std::size_t rlex_subcodecs = 0;
    std::size_t raw_subcodecs = 0;
};

/// ClearCodec encoder for one graphics channel. Use one instance for all
/// surfaces of the channel, because the client's context is shared.
///
/// Every stream encode() returns has to reach the client, in order and
/// unaltered: each one advances the sequence number and changes the caches
/// that later streams refer to. A stream that is dropped instead desyncs the
/// client (FreeRDP then fails every later ClearCodec PDU). After such a loss,
/// or for a new graphics channel, call reset().
///
/// Per region, the encoder:
///  1. sends a glyph hit if the region (at most 1,024 pixels) is in the glyph
///     cache, else stores it there (EncodeOptions::glyph_cache);
///  2. cuts the region into strips: maximal runs of rows that are not a
///     single colour, split every 52 rows. For each strip it compares the
///     estimated cost of leaving it to the residual layer, of bands with
///     V-bars (against the live caches), of an RLEX subcodec (at most 127
///     colours) and of a raw subcodec, and takes the cheapest. Bands are cut
///     at gaps of background columns;
///  3. covers everything else with the residual layer. Pixels that bands or
///     subcodecs paint over are free there: they extend the current run.
///
/// V-bars are the only layer that pays off later, so the bands estimate uses
/// second-chance admission: columns seen before (in an earlier strip or
/// frame) but not cached count as cache hits. Text and UI elements, which
/// recur, move into the client's V-bar storage the second time they are
/// encoded and cost 2 bytes per column from then on; content seen once stays
/// with RLEX or raw pixels and leaves the V-bar storage alone.
///
/// NSCodec as a subcodec is not implemented. It is a second lossy codec
/// ([MS-RDPNSC], YCoCg with colour loss and chroma subsampling) for photo-like
/// content, which the region classifier sends to Progressive or AVC instead;
/// for text and UI, bands and RLEX are lossless and smaller. The decoder
/// reports it as Errc::unsupported.
///
/// The caches compare content, not only hashes, so they hold copies of what
/// the client stores. Memory grows with use: a full V-bar storage takes about
/// 12 MiB, and a glyph storage full of 1,024-pixel regions 16 MiB more. Not
/// thread-safe.
class Encoder {
public:
    Encoder();
    ~Encoder();
    Encoder(Encoder&&) noexcept;
    Encoder& operator=(Encoder&&) noexcept;
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    /// Encodes `region` (B, G, R, X; top-down) into one
    /// CLEARCODEC_BITMAP_STREAM for a WIRE_TO_SURFACE_PDU_1 whose destRect has
    /// the region's size. Every pixel of the region is painted. Asserts width
    /// and height in 1..max_dimension and a view that holds them.
    [[nodiscard]] std::vector<std::byte> encode(const ImageView& region, const EncodeOptions& options = {});

    /// Starts a new codec context: forgets the caches, the next stream has
    /// seqNumber 0 and CLEARCODEC_FLAG_CACHE_RESET (the client's glyph storage
    /// keeps its contents and is simply overwritten).
    void reset();

    /// Statistics of the last encode() call.
    [[nodiscard]] const EncodeStats& last_stats() const noexcept;
    /// The seqNumber of the next stream.
    [[nodiscard]] std::uint8_t next_sequence_number() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

/// ClearCodec decoder for one graphics channel, following FreeRDP's
/// clear_decompress(). Layers paint onto the caller's buffer, so pixels that
/// no layer covers keep their previous value (FreeRDP paints onto the surface
/// the same way); a glyph is stored as the whole rectangle after all layers.
///
/// Stricter than FreeRDP, which the encoder never trips over:
///  - seqNumber must follow the previous one exactly, starting at 0 after
///    construction or reset() (FreeRDP resynchronizes when it expects 0);
///  - unknown flags, a residual run of 0, or residual runs that do not cover
///    the rectangle exactly are errors;
///  - a band must lie inside the rectangle, a VBAR_CACHE_HIT must name an
///    entry of exactly the band's height (FreeRDP pads empty or short entries
///    with black), and a short V-bar must fit below its shortVBarYOn;
///  - every layer must consume exactly its byteCount, and nothing may follow;
///  - CLEARCODEC_FLAG_GLYPH_INDEX needs a rectangle of at most 1,024 pixels
///    (FreeRDP allows 1024 * 1024) and a glyph hit a stored glyph of at least
///    as many pixels;
///  - NSCodec subcodecs are Errc::unsupported.
/// CLEARCODEC_FLAG_CACHE_RESET resets both V-bar cursors and keeps the
/// entries, as FreeRDP does.
///
/// After an error the rectangle and the context may be partly updated; the
/// channel is unusable, as it is for FreeRDP.
class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    /// Decodes one CLEARCODEC_BITMAP_STREAM for a width x height rectangle
    /// into `out` (B, G, R, A with A = 0xFF for painted pixels; top-down rows
    /// of `stride` bytes). Fails for a width or height outside
    /// 1..max_dimension; asserts that `out` holds the rectangle.
    [[nodiscard]] Result<void> decode(std::span<const std::byte> stream, std::uint32_t width, std::uint32_t height,
                                      std::span<std::byte> out, std::size_t stride);

    /// Starts a new codec context: empty storages, next seqNumber 0.
    void reset();

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace farland::codec::clear
