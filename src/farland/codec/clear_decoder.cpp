// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// ClearCodec decoder, [MS-RDPEGFX] 2.2.4.1 and 3.3.8.1.
//
// Written from the specification, with the behaviour of FreeRDP's
// libfreerdp/codec/clear.c (Apache-2.0) as the reference for what the spec
// leaves open: the glyph layer, how short V-bars fill the V-Bar Storage and
// that CLEARCODEC_FLAG_CACHE_RESET only resets the cursors. Where this decoder
// is stricter, clear.hpp lists it.

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/codec/clear.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace farland::codec::clear {

namespace {

/// A pixel as 0x00RRGGBB.
using Color = std::uint32_t;

constexpr std::size_t bytes_per_pixel = 4;
constexpr std::uint8_t known_flags = flag_glyph_index | flag_glyph_hit | flag_cache_reset;

[[nodiscard]] Result<Color> read_bgr(Reader& r)
{
    FARLAND_TRY(const auto bgr, r.bytes(3));
    return std::to_integer<Color>(bgr[0]) | (std::to_integer<Color>(bgr[1]) << 8U) |
           (std::to_integer<Color>(bgr[2]) << 16U);
}

/// runLengthFactor1, with runLengthFactor2 behind 0xFF and runLengthFactor3
/// behind 0xFFFF ([MS-RDPEGFX] 2.2.4.1.1.1.1, 2.2.4.1.1.3.1.1.2).
[[nodiscard]] Result<std::size_t> read_run_length(Reader& r)
{
    FARLAND_TRY(const auto factor1, r.u8());
    if (factor1 < 0xFF) {
        return factor1;
    }
    FARLAND_TRY(const auto factor2, r.u16le());
    if (factor2 < 0xFFFF) {
        return factor2;
    }
    FARLAND_TRY(const auto factor3, r.u32le());
    return factor3;
}

/// The destination rectangle.
struct Target {
    std::span<std::byte> out;
    std::size_t stride = 0;
    std::size_t width = 0;
    std::size_t height = 0;

    [[nodiscard]] std::size_t pixels() const { return width * height; }

    void put(std::size_t x, std::size_t y, Color c) const
    {
        const auto pixel = out.subspan((y * stride) + (x * bytes_per_pixel), bytes_per_pixel);
        pixel[0] = std::byte{static_cast<std::uint8_t>(c)};
        pixel[1] = std::byte{static_cast<std::uint8_t>(c >> 8U)};
        pixel[2] = std::byte{static_cast<std::uint8_t>(c >> 16U)};
        pixel[3] = std::byte{0xFF};
    }

    [[nodiscard]] Color get(std::size_t x, std::size_t y) const
    {
        const auto pixel = out.subspan((y * stride) + (x * bytes_per_pixel), bytes_per_pixel);
        return std::to_integer<Color>(pixel[0]) | (std::to_integer<Color>(pixel[1]) << 8U) |
               (std::to_integer<Color>(pixel[2]) << 16U);
    }
};

/// Paints a sequence of pixels in raster order over a sub-rectangle.
class RasterCursor {
public:
    RasterCursor(const Target& target, std::size_t x0, std::size_t y0, std::size_t width)
        : target_(target), x0_(x0), y0_(y0), width_(width)
    {
    }

    /// The caller has checked that `count` more pixels fit.
    void fill(Color c, std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i) {
            target_.put(x0_ + x_, y0_ + y_, c);
            if (++x_ == width_) {
                x_ = 0;
                ++y_;
            }
        }
    }

private:
    Target target_;
    std::size_t x0_;
    std::size_t y0_;
    std::size_t width_;
    std::size_t x_ = 0;
    std::size_t y_ = 0;
};

/// A V-Bar or Short V-Bar Storage entry. Unused entries are distinct from
/// used empty ones (a Short V-bar may have no pixels).
struct Entry {
    std::vector<Color> pixels;
    bool used = false;
};

}  // namespace

struct Decoder::State {
    std::uint8_t sequence = 0;
    std::vector<Entry> vbars;  // allocated on first use
    std::vector<Entry> short_vbars;
    std::size_t vbar_cursor = 0;
    std::size_t short_vbar_cursor = 0;
    std::vector<Entry> glyphs;
    std::vector<Color> column;

    [[nodiscard]] static Result<void> decode_residual(Reader r, const Target& t);
    [[nodiscard]] Result<void> decode_bands(Reader r, const Target& t);
    [[nodiscard]] Result<void> decode_band(Reader& r, const Target& t);
    [[nodiscard]] static Result<void> decode_subcodecs(Reader r, const Target& t);
    [[nodiscard]] static Result<void> decode_rlex(Reader r, const Target& t, std::size_t x0, std::size_t y0,
                                                  std::size_t width, std::size_t height);
};

/// CLEARCODEC_RESIDUAL_DATA, [MS-RDPEGFX] 2.2.4.1.1.1. The runs must cover
/// the rectangle exactly, as FreeRDP requires.
Result<void> Decoder::State::decode_residual(Reader r, const Target& t)
{
    RasterCursor cursor(t, 0, 0, t.width);
    std::size_t painted = 0;
    while (!r.empty()) {
        const std::size_t at = r.offset();
        FARLAND_TRY(const Color c, read_bgr(r));
        FARLAND_TRY(const std::size_t run, read_run_length(r));
        if (run == 0) {
            return fail(Errc::invalid_value, "ClearCodec residual run length is 0", at);
        }
        if (run > t.pixels() - painted) {
            return fail(Errc::invalid_length, "ClearCodec residual runs overrun the bitmap", at);
        }
        cursor.fill(c, run);
        painted += run;
    }
    if (painted != t.pixels()) {
        return fail(Errc::invalid_length, "ClearCodec residual runs do not cover the bitmap", r.offset());
    }
    return {};
}

/// CLEARCODEC_BANDS_DATA, [MS-RDPEGFX] 2.2.4.1.1.2.
Result<void> Decoder::State::decode_bands(Reader r, const Target& t)
{
    if (vbars.empty()) {
        vbars.resize(vbar_cache_size);
        short_vbars.resize(short_vbar_cache_size);
    }
    while (!r.empty()) {
        FARLAND_TRY_VOID(decode_band(r, t));
    }
    return {};
}

/// One CLEARCODEC_BAND and its V-bars, [MS-RDPEGFX] 2.2.4.1.1.2.1.
Result<void> Decoder::State::decode_band(Reader& r, const Target& t)
{
    const std::size_t at = r.offset();
    FARLAND_TRY(const std::size_t x_start, r.u16le());
    FARLAND_TRY(const std::size_t x_end, r.u16le());
    FARLAND_TRY(const std::size_t y_start, r.u16le());
    FARLAND_TRY(const std::size_t y_end, r.u16le());
    FARLAND_TRY(const Color background, read_bgr(r));
    if (x_end < x_start || y_end < y_start) {
        return fail(Errc::invalid_value, "ClearCodec band ends before it starts", at);
    }
    if (x_end >= t.width || y_end >= t.height) {
        return fail(Errc::invalid_value, "ClearCodec band lies outside the bitmap", at);
    }
    const std::size_t height = y_end - y_start + 1;
    if (height > max_band_height) {
        return fail(Errc::invalid_value, "ClearCodec band is higher than 52 pixels", at);
    }

    for (std::size_t x = x_start; x <= x_end; ++x) {
        const std::size_t header_at = r.offset();
        FARLAND_TRY(const auto header, r.u16le());
        const Entry* vbar = nullptr;
        if ((header & 0x8000U) != 0) {
            // VBAR_CACHE_HIT, 2.2.4.1.1.2.1.1.1: the entry must have been
            // stored with this band's height.
            const Entry& entry = vbars[header & 0x7FFFU];
            if (!entry.used || entry.pixels.size() != height) {
                return fail(Errc::invalid_value, "ClearCodec VBAR_CACHE_HIT names no V-bar of the band's height",
                            header_at);
            }
            vbar = &entry;
        } else {
            std::size_t y_on = 0;
            const Entry* short_vbar = nullptr;
            if ((header & 0xC000U) == 0x4000U) {
                // SHORT_VBAR_CACHE_HIT, 2.2.4.1.1.2.1.1.2.
                FARLAND_TRY(y_on, r.u8());
                const Entry& entry = short_vbars[header & 0x3FFFU];
                if (!entry.used) {
                    return fail(Errc::invalid_value, "ClearCodec SHORT_VBAR_CACHE_HIT names an empty entry", header_at);
                }
                if (y_on > height || entry.pixels.size() > height - y_on) {
                    return fail(Errc::invalid_value, "ClearCodec short V-bar does not fit in the band", header_at);
                }
                short_vbar = &entry;
            } else {
                // SHORT_VBAR_CACHE_MISS, 2.2.4.1.1.2.1.1.3: shortVBarYOn in
                // the low byte, shortVBarYOff (exclusive) in the next 6 bits.
                y_on = header & 0xFFU;
                const std::size_t y_off = (header >> 8U) & 0x3FU;
                if (y_off < y_on || y_off > height) {
                    return fail(Errc::invalid_value, "ClearCodec short V-bar does not fit in the band", header_at);
                }
                Entry& entry = short_vbars[short_vbar_cursor];
                entry.pixels.resize(y_off - y_on);
                for (Color& c : entry.pixels) {
                    FARLAND_TRY(c, read_bgr(r));
                }
                entry.used = true;
                short_vbar_cursor = (short_vbar_cursor + 1) % short_vbar_cache_size;
                short_vbar = &entry;
            }
            // Both short forms store the whole V-bar at the V-Bar Storage
            // Cursor: background, the short V-bar at y_on, background.
            column.assign(height, background);
            std::ranges::copy(short_vbar->pixels, column.begin() + static_cast<std::ptrdiff_t>(y_on));
            Entry& entry = vbars[vbar_cursor];
            entry.pixels = column;
            entry.used = true;
            vbar_cursor = (vbar_cursor + 1) % vbar_cache_size;
            vbar = &entry;
        }
        for (std::size_t y = 0; y < height; ++y) {
            t.put(x, y_start + y, vbar->pixels[y]);
        }
    }
    return {};
}

/// CLEARCODEC_SUBCODECS_DATA, [MS-RDPEGFX] 2.2.4.1.1.3.
Result<void> Decoder::State::decode_subcodecs(Reader r, const Target& t)
{
    while (!r.empty()) {
        // CLEARCODEC_SUBCODEC, 2.2.4.1.1.3.1.
        const std::size_t at = r.offset();
        FARLAND_TRY(const std::size_t x_start, r.u16le());
        FARLAND_TRY(const std::size_t y_start, r.u16le());
        FARLAND_TRY(const std::size_t width, r.u16le());
        FARLAND_TRY(const std::size_t height, r.u16le());
        FARLAND_TRY(const std::size_t byte_count, r.u32le());
        FARLAND_TRY(const auto id, r.u8());
        if (x_start + width > t.width || y_start + height > t.height) {
            return fail(Errc::invalid_value, "ClearCodec subcodec lies outside the bitmap", at);
        }
        const std::size_t raw_size = 3 * width * height;
        if (byte_count > raw_size) {
            return fail(Errc::invalid_length, "ClearCodec subcodec bitmapDataByteCount exceeds 3 * width * height", at);
        }
        FARLAND_TRY(auto data, r.sub(byte_count));
        switch (id) {
        case subcodec_uncompressed: {
            if (byte_count != raw_size) {
                return fail(Errc::invalid_length, "ClearCodec uncompressed subcodec has the wrong size", at);
            }
            for (std::size_t y = 0; y < height; ++y) {
                for (std::size_t x = 0; x < width; ++x) {
                    FARLAND_TRY(const Color c, read_bgr(data));
                    t.put(x_start + x, y_start + y, c);
                }
            }
            break;
        }
        case subcodec_nscodec:
            return fail(Errc::unsupported, "ClearCodec NSCodec subcodec is not supported", at);
        case subcodec_rlex:
            FARLAND_TRY_VOID(decode_rlex(data, t, x_start, y_start, width, height));
            break;
        default:
            return fail(Errc::invalid_value, "unknown ClearCodec subCodecId", at);
        }
    }
    return {};
}

/// CLEARCODEC_SUBCODEC_RLEX, [MS-RDPEGFX] 2.2.4.1.1.3.1.1. The segments must
/// cover the sub-rectangle exactly, as FreeRDP requires.
Result<void> Decoder::State::decode_rlex(Reader r, const Target& t, std::size_t x0, std::size_t y0, std::size_t width,
                                         std::size_t height)
{
    const std::size_t at = r.offset();
    FARLAND_TRY(const std::size_t palette_count, r.u8());
    if (palette_count == 0 || palette_count > max_palette_size) {
        return fail(Errc::invalid_value, "ClearCodec RLEX paletteCount is not 1..127", at);
    }
    std::array<Color, max_palette_size> palette{};
    for (std::size_t i = 0; i < palette_count; ++i) {
        FARLAND_TRY(palette[i], read_bgr(r));
    }

    const unsigned index_bits = rlex_index_bits(palette_count);
    const std::size_t pixels = width * height;
    RasterCursor cursor(t, x0, y0, width);
    std::size_t painted = 0;
    while (!r.empty()) {
        // CLEARCODEC_SUBCODEC_RLEX_SEGMENT, 2.2.4.1.1.3.1.1.2.
        const std::size_t segment_at = r.offset();
        FARLAND_TRY(const std::size_t packed, r.u8());
        FARLAND_TRY(const std::size_t run, read_run_length(r));
        const std::size_t stop = packed & ((1U << index_bits) - 1U);
        const std::size_t depth = packed >> index_bits;
        if (stop >= palette_count || depth > stop) {
            return fail(Errc::invalid_value, "ClearCodec RLEX suite leaves the palette", segment_at);
        }
        if (run > pixels - painted || depth + 1 > pixels - painted - run) {
            return fail(Errc::invalid_length, "ClearCodec RLEX segments overrun the subcodec", segment_at);
        }
        const std::size_t start = stop - depth;
        cursor.fill(palette[start], run);
        for (std::size_t index = start; index <= stop; ++index) {
            cursor.fill(palette[index], 1);
        }
        painted += run + depth + 1;
    }
    if (painted != pixels) {
        return fail(Errc::invalid_length, "ClearCodec RLEX segments do not cover the subcodec", r.offset());
    }
    return {};
}

Decoder::Decoder() : state_(std::make_unique<State>()) {}
Decoder::~Decoder() = default;
Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;

void Decoder::reset()
{
    *state_ = State{};
}

Result<void> Decoder::decode(std::span<const std::byte> stream, std::uint32_t width, std::uint32_t height,
                             std::span<std::byte> out, std::size_t stride)
{
    if (width == 0 || height == 0) {
        return fail(Errc::invalid_value, "ClearCodec bitmap has no pixels");
    }
    if (width > max_dimension || height > max_dimension) {
        return fail(Errc::limit_exceeded, "ClearCodec bitmap exceeds clear::max_dimension");
    }
    const Target t{.out = out, .stride = stride, .width = width, .height = height};
    FARLAND_ASSERT(stride >= t.width * bytes_per_pixel);
    FARLAND_ASSERT(out.size() >= ((t.height - 1) * stride) + (t.width * bytes_per_pixel));
    State& s = *state_;

    // CLEARCODEC_BITMAP_STREAM, [MS-RDPEGFX] 2.2.4.1.
    Reader r(stream);
    FARLAND_TRY(const auto flags, r.u8());
    FARLAND_TRY(const auto sequence, r.u8());
    if ((flags & ~known_flags) != 0) {
        return fail(Errc::invalid_value, "ClearCodec flags has unknown bits", 0);
    }
    if (sequence != s.sequence) {
        return fail(Errc::invalid_value, "ClearCodec seqNumber is out of sequence", 1);
    }
    s.sequence = static_cast<std::uint8_t>(sequence + 1);
    if ((flags & flag_cache_reset) != 0) {
        s.vbar_cursor = 0;
        s.short_vbar_cursor = 0;
    }

    // Glyph storage, [MS-RDPEGFX] 3.3.1.9: the pixels are a linear stream,
    // replayed into any rectangle of at most as many pixels.
    Entry* store_glyph = nullptr;
    if ((flags & flag_glyph_hit) != 0 && (flags & flag_glyph_index) == 0) {
        return fail(Errc::invalid_value, "ClearCodec CLEARCODEC_FLAG_GLYPH_HIT without GLYPH_INDEX", 0);
    }
    if ((flags & flag_glyph_index) != 0) {
        if (t.pixels() > max_glyph_pixels) {
            return fail(Errc::invalid_value, "ClearCodec glyph is larger than 1024 pixels", 0);
        }
        const std::size_t at = r.offset();
        FARLAND_TRY(const std::size_t index, r.u16le());
        if (index >= glyph_cache_size) {
            return fail(Errc::invalid_value, "ClearCodec glyphIndex is above 3999", at);
        }
        if (s.glyphs.empty()) {
            s.glyphs.resize(glyph_cache_size);
        }
        Entry& glyph = s.glyphs[index];
        if ((flags & flag_glyph_hit) != 0) {
            if (!glyph.used || glyph.pixels.size() < t.pixels()) {
                return fail(Errc::invalid_value, "ClearCodec glyph hit names no glyph of that size", at);
            }
            RasterCursor cursor(t, 0, 0, t.width);
            for (std::size_t i = 0; i < t.pixels(); ++i) {
                cursor.fill(glyph.pixels[i], 1);
            }
            return r.expect_end("ClearCodec glyph hit has a compositePayload");
        }
        store_glyph = &glyph;
    }

    // CLEARCODEC_COMPOSITE_PAYLOAD, [MS-RDPEGFX] 2.2.4.1.1.
    FARLAND_TRY(const auto residual_count, r.u32le());
    FARLAND_TRY(const auto bands_count, r.u32le());
    FARLAND_TRY(const auto subcodec_count, r.u32le());
    FARLAND_TRY(const auto residual, r.sub(residual_count));
    FARLAND_TRY(const auto bands, r.sub(bands_count));
    FARLAND_TRY(const auto subcodecs, r.sub(subcodec_count));
    FARLAND_TRY_VOID(r.expect_end("ClearCodec bitmap stream has trailing data"));
    // A layer is present only with a nonzero byte count (2.2.4.1.1).
    if (residual_count != 0) {
        FARLAND_TRY_VOID(State::decode_residual(residual, t));
    }
    FARLAND_TRY_VOID(s.decode_bands(bands, t));
    FARLAND_TRY_VOID(State::decode_subcodecs(subcodecs, t));

    if (store_glyph != nullptr) {
        // FreeRDP stores what the rectangle holds after all layers.
        store_glyph->pixels.resize(t.pixels());
        for (std::size_t y = 0; y < t.height; ++y) {
            for (std::size_t x = 0; x < t.width; ++x) {
                store_glyph->pixels[(y * t.width) + x] = t.get(x, y);
            }
        }
        store_glyph->used = true;
    }
    return {};
}

}  // namespace farland::codec::clear
