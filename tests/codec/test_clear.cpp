// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/error.hpp>
#include <farland/base/hexdump.hpp>
#include <farland/base/writer.hpp>
#include <farland/codec/clear.hpp>
#include <farland/codec/image.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace clear = farland::codec::clear;
using farland::Errc;
using farland::to_hex;
using farland::codec::ImageView;
using farland::test::hex;

namespace {

struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

constexpr Rgb white{0xFF, 0xFF, 0xFF};
constexpr Rgb black{0x00, 0x00, 0x00};

/// Owns the pixels behind an ImageView. Unused bytes (X and stride padding)
/// hold 0xEE so tests notice if the encoder reads them.
struct TestImage {
    std::vector<std::byte> data;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t stride = 0;

    TestImage(std::uint32_t w, std::uint32_t h, std::size_t row_stride = 0)
        : width(w), height(h), stride(row_stride == 0 ? std::size_t{w} * 4 : row_stride)
    {
        data.assign(stride * h, std::byte{0xEE});
    }

    void set(std::uint32_t x, std::uint32_t y, Rgb c)
    {
        const std::size_t at = (y * stride) + (std::size_t{x} * 4);
        data[at] = std::byte{c.b};
        data[at + 1] = std::byte{c.g};
        data[at + 2] = std::byte{c.r};
    }

    [[nodiscard]] Rgb get(std::uint32_t x, std::uint32_t y) const
    {
        const std::size_t at = (y * stride) + (std::size_t{x} * 4);
        return {std::to_integer<std::uint8_t>(data[at + 2]), std::to_integer<std::uint8_t>(data[at + 1]),
                std::to_integer<std::uint8_t>(data[at])};
    }

    void fill(std::uint32_t x0, std::uint32_t y0, std::uint32_t w, std::uint32_t h, Rgb c)
    {
        for (std::uint32_t y = y0; y < y0 + h; ++y) {
            for (std::uint32_t x = x0; x < x0 + w; ++x) {
                set(x, y, c);
            }
        }
    }

    [[nodiscard]] ImageView view() const { return {.data = data, .width = width, .height = height, .stride = stride}; }
};

/// What the decoder yields for `image`: B, G, R, 0xFF, stride width * 4.
std::vector<std::byte> expected_pixels(const TestImage& image)
{
    std::vector<std::byte> out;
    out.reserve(std::size_t{image.width} * image.height * 4);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const Rgb c = image.get(x, y);
            out.insert(out.end(), {std::byte{c.b}, std::byte{c.g}, std::byte{c.r}, std::byte{0xFF}});
        }
    }
    return out;
}

class Lcg {
public:
    explicit Lcg(std::uint32_t seed) : state_(seed) {}
    std::uint32_t next()
    {
        state_ = (state_ * 1664525U) + 1013904223U;  // Numerical Recipes LCG
        return state_ >> 8U;
    }

private:
    std::uint32_t state_;
};

Rgb blend(Rgb fg, Rgb bg, unsigned coverage, unsigned levels)
{
    const auto mix = [&](std::uint8_t f, std::uint8_t b) {
        return static_cast<std::uint8_t>(((f * coverage) + (b * (levels - coverage))) / levels);
    };
    return {mix(fg.r, bg.r), mix(fg.g, bg.g), mix(fg.b, bg.b)};
}

/// A 5 x 9 pseudo-glyph per letter (1..26; 0 is a space): bit x of row y.
std::array<std::uint8_t, 9> glyph_bitmap(unsigned letter)
{
    std::array<std::uint8_t, 9> rows{};
    if (letter == 0) {
        return rows;
    }
    Lcg lcg(0x9E37'79B9U * letter);
    for (std::size_t y = 1; y < 8; ++y) {
        rows[y] = static_cast<std::uint8_t>(lcg.next() & 0x1FU);
    }
    rows[1] |= 0x04;  // no empty letters
    return rows;
}

constexpr std::uint32_t cell_width = 7;
constexpr std::uint32_t cell_height = 11;

/// Draws one letter cell at (x0, y0) with a 4-level anti-aliasing halo, as
/// ClearType-like text rendering produces.
void draw_letter(TestImage& image, std::uint32_t x0, std::uint32_t y0, unsigned letter, Rgb fg, Rgb bg)
{
    const auto bitmap = glyph_bitmap(letter);
    const auto ink = [&](int x, int y) {
        return x >= 0 && x < 5 && y >= 0 && y < 9 &&
               (bitmap[static_cast<std::size_t>(y)] & (1U << static_cast<unsigned>(x))) != 0;
    };
    for (std::uint32_t cy = 0; cy < cell_height; ++cy) {
        for (std::uint32_t cx = 0; cx < cell_width; ++cx) {
            const int x = static_cast<int>(cx) - 1;
            const int y = static_cast<int>(cy) - 1;
            unsigned coverage = 0;
            if (ink(x, y)) {
                coverage = 6;
            } else {
                coverage = static_cast<unsigned>(ink(x - 1, y)) + static_cast<unsigned>(ink(x + 1, y)) +
                           static_cast<unsigned>(ink(x, y - 1)) + static_cast<unsigned>(ink(x, y + 1));
            }
            image.set(x0 + cx, y0 + cy, blend(fg, bg, coverage, 6));
        }
    }
}

/// Lines of pseudo-words, 16 pixels apart, inside the w x h area at (x0, y0).
void draw_text(TestImage& image, std::uint32_t x0, std::uint32_t y0, std::uint32_t w, std::uint32_t h,
               std::uint32_t seed, Rgb fg, Rgb bg)
{
    Lcg lcg(seed);
    for (std::uint32_t y = y0; y + cell_height <= y0 + h; y += 16) {
        for (std::uint32_t x = x0; x + cell_width <= x0 + w; x += cell_width) {
            const std::uint32_t r = lcg.next() % 32U;
            draw_letter(image, x, y, r < 26 ? r + 1 : 0, fg, bg);
        }
    }
}

/// One letter on white, with a one-pixel margin.
TestImage letter_image(unsigned letter)
{
    TestImage image(cell_width + 2, cell_height + 2);
    image.fill(0, 0, image.width, image.height, white);
    draw_letter(image, 1, 1, letter, black, white);
    return image;
}

enum class Pattern { solid, text, ui, gradient_h, gradient_v, gradient_d, checker, noise };

constexpr std::array all_patterns{Pattern::solid,      Pattern::text,       Pattern::ui,      Pattern::gradient_h,
                                  Pattern::gradient_v, Pattern::gradient_d, Pattern::checker, Pattern::noise};

TestImage make_image(Pattern pattern, std::uint32_t width, std::uint32_t height, std::uint32_t seed = 1,
                     std::size_t stride = 0)
{
    TestImage image(width, height, stride);
    const Rgb paper{0xF0, 0xF0, 0xF0};
    const Rgb frame{100, 100, 100};
    switch (pattern) {
    case Pattern::solid:
        image.fill(0, 0, width, height, {0x80, 0x40, 0x20});
        break;
    case Pattern::text:
        image.fill(0, 0, width, height, white);
        if (width > 2 && height > 2) {
            draw_text(image, 2, 2, width - 2, height - 2, seed, black, white);
        }
        break;
    case Pattern::ui: {
        // A window: title bar with a vertical gradient and white text, a
        // one-pixel frame, a text area and a button.
        image.fill(0, 0, width, height, paper);
        const std::uint32_t title = std::min<std::uint32_t>(22, height);
        for (std::uint32_t y = 0; y < title; ++y) {
            image.fill(0, y, width, 1,
                       {static_cast<std::uint8_t>(40 + y), static_cast<std::uint8_t>(80 + (2 * y)), 200});
        }
        if (width > 16 && height > title + 8) {
            draw_text(image, 6, 6, std::min<std::uint32_t>(width - 6, 120), 12, seed, white, {45, 90, 200});
            image.fill(0, title, 1, height - title, frame);
            image.fill(width - 1, title, 1, height - title, frame);
            image.fill(0, height - 1, width, 1, frame);
            if (height > title + 36) {
                draw_text(image, 8, title + 6, width - 16, height - title - 36, seed + 1, {0x20, 0x20, 0x20}, paper);
            }
        }
        if (width > 80 && height > 60) {
            const std::uint32_t bx = width - 76;
            const std::uint32_t by = height - 28;
            const Rgb face{0xD8, 0xD8, 0xD8};
            const Rgb edge{0x70, 0x70, 0x70};
            image.fill(bx, by, 64, 20, face);
            image.fill(bx, by, 64, 1, edge);
            image.fill(bx, by + 19, 64, 1, edge);
            image.fill(bx, by, 1, 20, edge);
            image.fill(bx + 63, by, 1, 20, edge);
            draw_text(image, bx + 11, by + 4, 42, 12, seed + 2, black, face);
        }
        break;
    }
    case Pattern::gradient_h:
    case Pattern::gradient_v:
    case Pattern::gradient_d:
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::uint32_t t =
                    pattern == Pattern::gradient_h ? x : (pattern == Pattern::gradient_v ? y : x + y);
                image.set(x, y, {static_cast<std::uint8_t>(t), static_cast<std::uint8_t>(255 - t), 0x40});
            }
        }
        break;
    case Pattern::checker:
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                image.set(x, y, ((x / 4) + (y / 4)) % 2 == 0 ? white : Rgb{0x33, 0x66, 0x99});
            }
        }
        break;
    case Pattern::noise: {
        Lcg lcg(seed);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::uint32_t v = lcg.next();
                image.set(x, y,
                          {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8U),
                           static_cast<std::uint8_t>(v >> 16U)});
            }
        }
        break;
    }
    }
    return image;
}

std::vector<std::byte> decode_ok(clear::Decoder& decoder, std::span<const std::byte> stream, std::uint32_t width,
                                 std::uint32_t height)
{
    std::vector<std::byte> out(std::size_t{width} * height * 4, std::byte{0xEE});
    const auto result = decoder.decode(stream, width, height, out, std::size_t{width} * 4);
    INFO((result.has_value() ? std::string("ok") : result.error().message()));
    REQUIRE(result.has_value());
    return out;
}

Errc decode_error(clear::Decoder& decoder, std::span<const std::byte> stream, std::uint32_t width, std::uint32_t height)
{
    std::vector<std::byte> out(std::size_t{width} * height * 4, std::byte{0xEE});
    const auto result = decoder.decode(stream, width, height, out, std::size_t{width} * 4);
    REQUIRE_FALSE(result.has_value());
    return result.error().code;
}

Errc decode_error(std::span<const std::byte> stream, std::uint32_t width, std::uint32_t height)
{
    clear::Decoder decoder;
    return decode_error(decoder, stream, width, height);
}

/// Encodes `image`, decodes it and checks every pixel. Returns the stream.
std::vector<std::byte> round_trip(clear::Encoder& encoder, clear::Decoder& decoder, const TestImage& image,
                                  const clear::EncodeOptions& options = {})
{
    auto stream = encoder.encode(image.view(), options);
    CHECK(decode_ok(decoder, stream, image.width, image.height) == expected_pixels(image));
    return stream;
}

/// A CLEARCODEC_BITMAP_STREAM with a composite payload built from hex layers.
std::vector<std::byte> composite(std::string_view residual, std::string_view bands, std::string_view subcodecs,
                                 std::uint8_t flags = 0, std::uint8_t sequence = 0)
{
    const auto r = hex(residual);
    const auto b = hex(bands);
    const auto s = hex(subcodecs);
    farland::Writer w;
    w.u8(flags);
    w.u8(sequence);
    w.u32le(static_cast<std::uint32_t>(r.size()));
    w.u32le(static_cast<std::uint32_t>(b.size()));
    w.u32le(static_cast<std::uint32_t>(s.size()));
    w.bytes(r);
    w.bytes(b);
    w.bytes(s);
    return std::move(w).take();
}

std::vector<std::byte> bgra(std::initializer_list<Rgb> pixels)
{
    std::vector<std::byte> out;
    for (const Rgb& c : pixels) {
        out.insert(out.end(), {std::byte{c.b}, std::byte{c.g}, std::byte{c.r}, std::byte{0xFF}});
    }
    return out;
}

/// `pixels` with every alpha byte 0xFF, as a glyph replay paints them.
std::vector<std::byte> opaque(std::vector<std::byte> pixels)
{
    for (std::size_t i = 3; i < pixels.size(); i += 4) {
        pixels[i] = std::byte{0xFF};
    }
    return pixels;
}

constexpr clear::EncodeOptions no_glyphs{.glyph_cache = false};

}  // namespace

// ---------------------------------------------------------------------------
// Encoder

TEST_CASE("ClearCodec stream header, residual runs and the sequence number ([MS-RDPEGFX] 2.2.4.1)")
{
    clear::Encoder encoder;
    clear::Decoder decoder;
    const auto solid = make_image(Pattern::solid, 8, 8);  // R 80, G 40, B 20
    // flags CACHE_RESET (first stream of a context), seqNumber 0, residual 4
    // bytes, no bands, no subcodecs; one run of 64 pixels of B 20 G 40 R 80.
    CHECK(to_hex(round_trip(encoder, decoder, solid, no_glyphs)) ==
          "04 00 04 00 00 00 00 00 00 00 00 00 00 00 20 40 80 40");
    CHECK(to_hex(round_trip(encoder, decoder, solid, no_glyphs)) ==
          "00 01 04 00 00 00 00 00 00 00 00 00 00 00 20 40 80 40");
    CHECK(encoder.next_sequence_number() == 2);

    // runLengthFactor2 behind 0xFF (300 = 0x012c), runLengthFactor3 behind 0xFFFF.
    CHECK(to_hex(round_trip(encoder, decoder, make_image(Pattern::solid, 300, 1), no_glyphs)) ==
          "00 02 06 00 00 00 00 00 00 00 00 00 00 00 20 40 80 ff 2c 01");
    CHECK(to_hex(round_trip(encoder, decoder, make_image(Pattern::solid, 1920, 1080), no_glyphs)) ==
          "00 03 0a 00 00 00 00 00 00 00 00 00 00 00 20 40 80 ff ff ff 00 a4 1f 00");

    // The sequence number wraps from 0xFF to 0x00.
    for (int i = 0; i < 251; ++i) {
        static_cast<void>(round_trip(encoder, decoder, solid, no_glyphs));
    }
    CHECK(encoder.next_sequence_number() == 0xFF);
    CHECK(round_trip(encoder, decoder, solid, no_glyphs)[1] == std::byte{0xFF});
    CHECK(round_trip(encoder, decoder, solid, no_glyphs)[1] == std::byte{0x00});

    // reset() starts a new context: seqNumber 0 and CACHE_RESET again.
    encoder.reset();
    decoder.reset();
    CHECK(to_hex(round_trip(encoder, decoder, solid, no_glyphs)).starts_with("04 00 "));
}

TEST_CASE("ClearCodec glyph cache: store, then a 4-byte hit ([MS-RDPEGFX] 4.1.1.1, 4.1.1.5)")
{
    clear::Encoder encoder;
    clear::Decoder decoder;
    const auto glyph = letter_image(3);
    // GLYPH_INDEX | CACHE_RESET, seqNumber 0, glyphIndex 0, composite payload.
    CHECK(to_hex(round_trip(encoder, decoder, glyph)).starts_with("05 00 00 00 "));
    CHECK(encoder.last_stats().glyph_stored);
    // GLYPH_INDEX | GLYPH_HIT, seqNumber 1, glyphIndex 0, nothing else.
    CHECK(to_hex(round_trip(encoder, decoder, glyph)) == "03 01 00 00");
    CHECK(encoder.last_stats().glyph_hit);

    // Another glyph takes the next slot; the first one still hits.
    const auto other = letter_image(4);
    CHECK(to_hex(round_trip(encoder, decoder, other)).starts_with("01 02 01 00 "));
    CHECK(to_hex(round_trip(encoder, decoder, glyph)) == "03 03 00 00");
    CHECK(to_hex(round_trip(encoder, decoder, other)) == "03 04 01 00");

    // The same pixel stream in another shape is a different glyph.
    TestImage reshaped(glyph.height, glyph.width);
    for (std::uint32_t i = 0; i < glyph.width * glyph.height; ++i) {
        reshaped.set(i % reshaped.width, i / reshaped.width, glyph.get(i % glyph.width, i / glyph.width));
    }
    CHECK(to_hex(round_trip(encoder, decoder, reshaped)).starts_with("01 05 02 00 "));

    // Regions above 1024 pixels never use the glyph cache, and neither does
    // an encode with the cache switched off.
    const auto large = make_image(Pattern::text, 33, 32);
    CHECK(round_trip(encoder, decoder, large)[0] == std::byte{0x00});
    CHECK(round_trip(encoder, decoder, large)[0] == std::byte{0x00});
    CHECK(round_trip(encoder, decoder, glyph, no_glyphs)[0] == std::byte{0x00});
    const auto limit = make_image(Pattern::text, 32, 32);
    CHECK(round_trip(encoder, decoder, limit)[0] == std::byte{0x01});
    CHECK(round_trip(encoder, decoder, limit)[0] == std::byte{0x03});
}

TEST_CASE("ClearCodec glyph cache evicts the least recently used of 4000 slots")
{
    clear::Encoder encoder;
    clear::Decoder decoder;
    const auto glyph = [](std::uint32_t n) {
        TestImage image(4, 2);
        image.fill(0, 0, 4, 2, white);
        image.set(n % 4, 0, {static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(n >> 8U), 0x55});
        return image;
    };
    for (std::uint32_t n = 0; n < clear::glyph_cache_size; ++n) {
        static_cast<void>(round_trip(encoder, decoder, glyph(n)));
        REQUIRE(encoder.last_stats().glyph_stored);
    }
    static_cast<void>(round_trip(encoder, decoder, glyph(0)));  // glyph 0 is now the most recent
    CHECK(encoder.last_stats().glyph_hit);
    const auto evicting = round_trip(encoder, decoder, glyph(4000));
    CHECK(encoder.last_stats().glyph_stored);
    CHECK(to_hex(std::span(evicting).subspan(2, 2)) == "01 00");  // glyph 1's slot
    static_cast<void>(round_trip(encoder, decoder, glyph(0)));
    CHECK(encoder.last_stats().glyph_hit);
    static_cast<void>(round_trip(encoder, decoder, glyph(1)));
    CHECK(encoder.last_stats().glyph_stored);
    static_cast<void>(round_trip(encoder, decoder, glyph(3999)));
    CHECK(encoder.last_stats().glyph_hit);
}

TEST_CASE("ClearCodec bands: recurring text moves into the V-bar storage")
{
    clear::Encoder encoder;
    clear::Decoder decoder;
    const auto text = make_image(Pattern::text, 240, 40, 3);
    const auto first = round_trip(encoder, decoder, text, no_glyphs);
    const auto s1 = encoder.last_stats();

    // Seen before: the columns are worth caching and go out as V-bars.
    static_cast<void>(round_trip(encoder, decoder, text, no_glyphs));
    const auto s2 = encoder.last_stats();
    CHECK(s2.bands > 0);
    CHECK(s2.short_vbar_misses > 0);

    // From then on every column is a 2-byte V-bar hit.
    const auto third = round_trip(encoder, decoder, text, no_glyphs);
    const auto s3 = encoder.last_stats();
    CHECK(s3.bands == s2.bands);
    CHECK(s3.short_vbar_misses == 0);
    CHECK(s3.short_vbar_hits == 0);
    CHECK(s3.vbar_hits == s2.vbar_hits + s2.short_vbar_hits + s2.short_vbar_misses);
    INFO("first " << first.size() << " (" << s1.bands << " bands, " << s1.rlex_subcodecs << " RLEX), third "
                  << third.size());
    CHECK(third.size() * 2 < first.size());

    // The same text two rows lower, with a dot in the row above each line:
    // the bands are one row taller and the V-bars differ, but most columns
    // are the Short V-bars sent before, now at shortVBarYOn + 1.
    TestImage shifted(240, 44);
    shifted.fill(0, 0, 240, 44, white);
    for (std::uint32_t y = 0; y < 40; ++y) {
        for (std::uint32_t x = 0; x < 240; ++x) {
            shifted.set(x, y + 2, text.get(x, y));
        }
    }
    shifted.set(0, 4, {0x80, 0x80, 0x80});
    shifted.set(0, 20, {0x80, 0x80, 0x80});
    const auto fourth = round_trip(encoder, decoder, shifted, no_glyphs);
    const auto s4 = encoder.last_stats();
    CHECK(s4.bands > 0);
    CHECK(s4.short_vbar_hits > s4.short_vbar_misses * 4);
    CHECK(fourth.size() < first.size());
}

TEST_CASE("ClearCodec subcodecs: RLEX for few colours, raw for many")
{
    clear::Encoder encoder;
    clear::Decoder decoder;
    // Rows of three colour runs with different split points: no two columns
    // alike, but few palette runs.
    TestImage bars(100, 40);
    for (std::uint32_t y = 0; y < 40; ++y) {
        const std::uint32_t a = ((y * 7) % 50) + 10;
        const std::uint32_t b = a + 20 + ((y * 13) % 30);
        bars.fill(0, y, a, 1, {0xC0, 0x20, 0x20});
        bars.fill(a, y, b - a, 1, {0x20, 0xC0, 0x20});
        bars.fill(b, y, 100 - b, 1, {0x20, 0x20, 0xC0});
    }
    const auto bars_stream = round_trip(encoder, decoder, bars, no_glyphs);
    CHECK(encoder.last_stats().rlex_subcodecs == 1);
    CHECK(encoder.last_stats().bands == 0);
    CHECK(bars_stream.size() < 400);

    // Noise has too many colours for RLEX and no runs: raw pixels.
    const auto noise = make_image(Pattern::noise, 40, 30);
    const auto stream = round_trip(encoder, decoder, noise, no_glyphs);
    CHECK(encoder.last_stats().raw_subcodecs == 1);
    CHECK(encoder.last_stats().residual_bytes == 0);  // the subcodec paints every pixel
    CHECK(stream.size() == 2 + 12 + 13 + (3 * 40 * 30));
}

TEST_CASE("ClearCodec round trip of synthetic text and UI content")
{
    constexpr std::array<std::array<std::uint32_t, 2>, 11> sizes{
        {{1, 1}, {1, 60}, {60, 1}, {3, 7}, {16, 16}, {32, 32}, {64, 53}, {65, 104}, {128, 160}, {300, 70}, {513, 9}}};
    clear::Encoder encoder;
    clear::Decoder decoder;
    std::uint32_t seed = 1;
    for (const bool glyphs : {true, false}) {
        for (const auto [width, height] : sizes) {
            for (const Pattern pattern : all_patterns) {
                INFO("size " << width << "x" << height << ", pattern " << static_cast<int>(pattern) << ", glyphs "
                             << glyphs);
                const auto image = make_image(pattern, width, height, ++seed);
                static_cast<void>(round_trip(encoder, decoder, image, {.glyph_cache = glyphs}));
                // Later passes hit the caches that the first one filled.
                static_cast<void>(round_trip(encoder, decoder, image, {.glyph_cache = glyphs}));
                static_cast<void>(round_trip(encoder, decoder, image, {.glyph_cache = glyphs}));
            }
        }
    }
}

TEST_CASE("ClearCodec round trip honours a stride larger than width * 4")
{
    clear::Encoder encoder;
    clear::Decoder decoder;
    for (const Pattern pattern : all_patterns) {
        const auto image = make_image(pattern, 37, 61, 5, (37 * 4) + 20);
        static_cast<void>(round_trip(encoder, decoder, image, no_glyphs));
    }
    // The last row needs only width * 4 bytes, not a whole stride.
    auto image = make_image(Pattern::ui, 90, 70, 5, 400);
    image.data.resize((69 * 400) + (90 * 4));
    static_cast<void>(round_trip(encoder, decoder, image));
}

TEST_CASE("ClearCodec caches stay in sync when the V-bar cursors wrap around")
{
    // Columns of white with a short run of a random colour: too many colours
    // for RLEX and cheaper than raw pixels, so every strip goes to bands and
    // each column is a new V-bar. The image stores about 2000 * 17 V-bars,
    // more than the 32,768 slots.
    constexpr std::uint32_t width = 2000;
    constexpr std::uint32_t height = 52 * 17;
    TestImage image(width, height);
    image.fill(0, 0, width, height, white);
    Lcg lcg(99);
    for (std::uint32_t band = 0; band < height / 52; ++band) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t v = lcg.next();
            image.fill(x, (band * 52) + (v % 48), 1, 1 + ((v >> 6U) % 4),
                       {static_cast<std::uint8_t>(v >> 8U), static_cast<std::uint8_t>(v >> 16U), 0x10});
        }
    }
    clear::Encoder encoder;
    clear::Decoder decoder;
    static_cast<void>(round_trip(encoder, decoder, image, no_glyphs));
    const auto s1 = encoder.last_stats();
    CHECK(s1.short_vbar_misses + s1.short_vbar_hits > clear::vbar_cache_size);

    // The bottom eight bands are still in the V-bar storage, except for the
    // few columns that repeated a column of an early band (random columns
    // collide now and then) and hit its since evicted V-bar.
    constexpr std::uint32_t kept = 52 * 8;
    TestImage bottom(width, kept);
    for (std::uint32_t y = 0; y < kept; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            bottom.set(x, y, image.get(x, y + height - kept));
        }
    }
    static_cast<void>(round_trip(encoder, decoder, bottom, no_glyphs));
    const auto s2 = encoder.last_stats();
    CHECK(s2.vbar_hits + s2.short_vbar_hits + s2.short_vbar_misses == std::size_t{8} * width);
    CHECK(s2.short_vbar_misses + s2.short_vbar_hits < 50);

    // The whole image again: its first bands were evicted and are sent
    // again, overwriting the slots that later columns of the same image
    // would have hit. Encoder and decoder must agree throughout.
    static_cast<void>(round_trip(encoder, decoder, image, no_glyphs));
    static_cast<void>(round_trip(encoder, decoder, image, no_glyphs));
}

TEST_CASE("ClearCodec compression ratio")
{
    clear::Encoder encoder;
    clear::Decoder decoder;
    const auto raw_size = [](const TestImage& image) { return std::size_t{3} * image.width * image.height; };

    const auto solid = make_image(Pattern::solid, 1920, 1080);
    CHECK(round_trip(encoder, decoder, solid).size() == 24);

    const auto text = make_image(Pattern::text, 400, 200, 11);
    const auto text_stream = round_trip(encoder, decoder, text);
    const auto text_again = round_trip(encoder, decoder, text);
    const auto text_cached = round_trip(encoder, decoder, text);
    INFO("text: " << text_stream.size() << ", " << text_again.size() << ", " << text_cached.size() << " of "
                  << raw_size(text));
    CHECK(text_stream.size() * 6 < raw_size(text));
    CHECK(text_cached.size() * 25 < raw_size(text));

    const auto ui = make_image(Pattern::ui, 400, 300, 12);
    const auto ui_stream = round_trip(encoder, decoder, ui);
    INFO("ui: " << ui_stream.size() << " of " << raw_size(ui));
    CHECK(ui_stream.size() * 7 < raw_size(ui));

    // One run per row: BGR and runLengthFactor2 (runs of 256).
    const auto vertical = make_image(Pattern::gradient_v, 256, 256);
    CHECK(round_trip(encoder, decoder, vertical).size() == 2 + 12 + (6 * 256));

    // Every strip has the V-bars of the one above: the first strip goes out
    // as raw pixels, the second one fills the V-bar storage, the rest hit.
    const auto horizontal = make_image(Pattern::gradient_h, 256, 520);
    const auto horizontal_stream = round_trip(encoder, decoder, horizontal);
    INFO("horizontal gradient: " << horizontal_stream.size());
    CHECK(horizontal_stream.size() < (2 * 3 * 256 * 52) + (10 * (11 + (2 * 256))) + 1024);

    // Noise never grows beyond raw BGR plus the headers.
    const auto noise = make_image(Pattern::noise, 200, 104);
    CHECK(round_trip(encoder, decoder, noise).size() <= raw_size(noise) + 14 + (2 * 13));
}

// ---------------------------------------------------------------------------
// Decoder: specification examples and hand-made streams

TEST_CASE("ClearCodec decoder: RLEX subcodec example ([MS-RDPEGFX] 4.1.1.2)")
{
    // 78 x 17 bitmap; seqNumber changed from 0x0d to 0 for a fresh decoder.
    const auto stream = hex("00 00 00 00 00 00 00 00 00 00 82 00 00 00 00 00"
                            "00 00 4e 00 11 00 75 00 00 00 02 0e ff ff ff 00"
                            "00 00 db ff ff 00 3a 90 ff b6 66 66 b6 ff b6 66"
                            "00 90 db ff 00 00 3a db 90 3a 3a 90 db 66 00 00"
                            "ff ff b6 64 64 64 11 04 11 4c 11 4c 11 4c 11 4c"
                            "11 4c 00 47 13 00 01 01 04 00 01 00 00 47 16 00"
                            "11 02 00 47 29 00 11 01 00 49 0a 00 01 00 04 00"
                            "01 00 00 4a 0a 00 09 00 01 00 00 47 05 00 01 01"
                            "1c 00 01 00 11 4c 11 4c 11 4c 00 47 0d 4d 00 4d");
    clear::Decoder decoder;
    const auto pixels = decode_ok(decoder, stream, 78, 17);
    const auto at = [&](std::size_t i) { return std::span(pixels).subspan(i * 4, 4); };
    // First segment: 5 x palette[0] (white), then palette[1] (black).
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(to_hex(at(i)) == "ff ff ff ff");
    }
    CHECK(to_hex(at(5)) == "00 00 00 ff");
    // Second segment: 77 x white, then black; it wraps into the second row.
    CHECK(to_hex(at(82)) == "ff ff ff ff");
    CHECK(to_hex(at(83)) == "00 00 00 ff");

    // Our encoder reproduces the bitmap exactly.
    TestImage image(78, 17);
    for (std::uint32_t y = 0; y < 17; ++y) {
        for (std::uint32_t x = 0; x < 78; ++x) {
            const auto p = at((std::size_t{y} * 78) + x);
            image.set(x, y,
                      {std::to_integer<std::uint8_t>(p[2]), std::to_integer<std::uint8_t>(p[1]),
                       std::to_integer<std::uint8_t>(p[0])});
        }
    }
    clear::Encoder encoder;
    clear::Decoder check;
    const auto ours = round_trip(encoder, check, image);
    INFO("farland: " << ours.size() << " bytes, Windows: " << stream.size());
    CHECK(ours.size() < stream.size() * 2);
}

TEST_CASE("ClearCodec decoder: residual example ([MS-RDPEGFX] 4.1.1.3)")
{
    // 64 x 24; the example's bands are cache hits into storage filled by
    // earlier streams, which a fresh decoder must refuse.
    const auto residual = "fe fe fe ff 80 05 ff ff ff 40 fe fe fe 40";
    std::vector<std::byte> expected;
    for (std::size_t i = 0; i < 64 * 24; ++i) {
        const auto v = std::byte{static_cast<std::uint8_t>(i / 64 == 22 ? 0xFF : 0xFE)};
        expected.insert(expected.end(), {v, v, v, std::byte{0xFF}});
    }
    clear::Decoder decoder;
    CHECK(decode_ok(decoder, composite(residual, "", ""), 64, 24) == expected);

    clear::Decoder fresh;
    const auto example = hex("00 00 0e 00 00 00 8b 00 00 00 00 00 00 00 fe fe"
                             "fe ff 80 05 ff ff ff 40 fe fe fe 40 00 00 3f 00"
                             "03 00 0b 00 fe fe fe c5 d0 c6 d0 c7 d0 68 d4 69"
                             "d4 6a d4 6b d4 6c d4 6d d4 1a d4 1a d4 a6 d0 6e"
                             "d4 6f d4 70 d4 71 d4 72 d4 73 d4 74 d4 21 d4 22"
                             "d4 23 d4 24 d4 25 d4 d9 d0 da d0 db d0 c5 d0 c5"
                             "d0 dc d0 c2 d0 21 d4 22 d4 23 d4 24 d4 25 d4 c9"
                             "d0 ca d0 5a d4 2b d1 28 d1 2c d1 75 d4 27 d4 28"
                             "d4 29 d4 2a d4 1a d4 1a d4 1a d4 b7 d0 b8 d0 b9"
                             "d0 ba d0 bb d0 bc d0 bd d0 be d0 bf d0 c0 d0 c1"
                             "d0 c2 d0 c3 d0 c4 d0");
    CHECK(decode_error(fresh, example, 64, 24) == Errc::invalid_value);
}

TEST_CASE("ClearCodec decoder: short V-bar miss and glyph storage ([MS-RDPEGFX] 4.1.1.4)")
{
    // 7 x 15 with GLYPH_INDEX 120 and seqNumber 0 (the example has 11). The
    // band keeps only its first V-bar: the example's other six are hits into
    // storage filled by earlier streams.
    const auto pixels = "ff ff ff ff ff ff ff ff ff b6 ff ff ff ff ff ff ff ff ff b6 66 ff ff ff"
                        "ff ff ff ff b6 66 db 90 3a ff ff b6 ff ff ff ff ff ff ff ff ff";
    auto stream = hex("01 00 78 00");
    const auto payload = composite("", std::string("00 00 00 00 00 00 0e 00 00 00 00 00 0f ") + pixels, "");
    stream.insert(stream.end(), payload.begin() + 2, payload.end());

    clear::Decoder decoder;
    std::vector<std::byte> out(7 * 15 * 4, std::byte{0x11});
    REQUIRE(decoder.decode(stream, 7, 15, out, 7 * 4).has_value());
    const auto column = hex(pixels);
    for (std::size_t y = 0; y < 15; ++y) {
        INFO("row " << y);
        CHECK(to_hex(std::span(out).subspan(y * 28, 3)) == to_hex(std::span(column).subspan(y * 3, 3)));
        CHECK(out[(y * 28) + 3] == std::byte{0xFF});
        CHECK(out[(y * 28) + 4] == std::byte{0x11});  // column 1 was not painted
    }
    CHECK(to_hex(std::span(out).subspan(10 * 28, 3)) == "db 90 3a");

    // The glyph holds the whole rectangle as it was after decoding,
    // including the unpainted pixels; a hit replays it in another shape.
    std::vector<std::byte> replay(15 * 7 * 4, std::byte{0x22});
    REQUIRE(decoder.decode(hex("03 01 78 00"), 15, 7, replay, 15 * 4).has_value());
    CHECK(replay == opaque(out));
    std::vector<std::byte> smaller(2 * 2 * 4, std::byte{0x22});
    REQUIRE(decoder.decode(hex("03 02 78 00"), 2, 2, smaller, 2 * 4).has_value());
    CHECK(smaller == opaque(std::vector(out.begin(), out.begin() + 16)));

    clear::Decoder fresh;
    const auto example = hex("01 00 78 00 00 00 00 00 46 00 00 00 00 00 00 00"
                             "00 00 06 00 00 00 0e 00 00 00 00 00 0f ff ff ff"
                             "ff ff ff ff ff ff b6 ff ff ff ff ff ff ff ff ff"
                             "b6 66 ff ff ff ff ff ff ff b6 66 db 90 3a ff ff"
                             "b6 ff ff ff ff ff ff ff ff ff 46 91 47 91 48 91"
                             "49 91 4a 91 1b 91");
    CHECK(decode_error(fresh, example, 7, 15) == Errc::invalid_value);
}

TEST_CASE("ClearCodec decoder: short V-bar hits and CACHE_RESET")
{
    clear::Decoder decoder;
    const Rgb a{0x10, 0x20, 0x30};
    const Rgb b{0x40, 0x50, 0x60};
    // 1 x 4, background white. Stream 0: short miss of [a, b] at rows 1..2
    // (header 0x0301: shortVBarYOn 1, shortVBarYOff 3); stored as short
    // V-bar 0 and V-bar 0.
    const auto first = composite("", "00 00 00 00 00 00 03 00 ff ff ff  01 03 30 20 10 60 50 40", "");
    CHECK(decode_ok(decoder, first, 1, 4) == bgra({white, a, b, white}));
    // Stream 1: short hit of short V-bar 0 at row 2 on a black background;
    // stored as V-bar 1.
    CHECK(decode_ok(decoder, composite("", "00 00 00 00 00 00 03 00 00 00 00  00 40 02", "", 0, 1), 1, 4) ==
          bgra({black, black, a, b}));
    // Stream 2: V-bar hits of both.
    CHECK(decode_ok(decoder, composite("", "00 00 01 00 00 00 03 00 00 00 00  00 80 01 80", "", 0, 2), 2, 4) ==
          bgra({white, black, a, black, b, a, white, b}));
    // Stream 3: CACHE_RESET moves both cursors back to 0 but keeps the
    // entries: an empty short miss overwrites V-bar 0, V-bar 1 still hits.
    CHECK(decode_ok(decoder,
                    composite("", "00 00 01 00 00 00 03 00 ff ff ff  00 00  01 80", "", clear::flag_cache_reset, 3), 2,
                    4) == bgra({white, black, white, black, white, a, white, b}));
    CHECK(decode_ok(decoder, composite("", "00 00 00 00 00 00 03 00 00 00 00  00 80", "", 0, 4), 1, 4) ==
          bgra({white, white, white, white}));

    // A short V-bar must fit below its shortVBarYOn: [a, b] at row 3 of 4.
    clear::Decoder other;
    static_cast<void>(decode_ok(other, first, 1, 4));
    CHECK(decode_error(other, composite("", "00 00 00 00 00 00 03 00 00 00 00  00 40 03", "", 0, 1), 1, 4) ==
          Errc::invalid_value);
}

TEST_CASE("ClearCodec decoder: RLEX segments, runs and suites")
{
    // Palette [c0, c1] (1-bit indices): c0 c0 c0 c1 as a run of 2 and the
    // suite 0..1 (stopIndex 1, suiteDepth 1: 0x03).
    const Rgb c0{0x01, 0x02, 0x03};
    const Rgb c1{0x04, 0x05, 0x06};
    clear::Decoder decoder;
    CHECK(decode_ok(decoder, composite("", "", "00 00 00 00 02 00 02 00 09 00 00 00 02  02 03 02 01 06 05 04 03 02"), 2,
                    2) == bgra({c0, c0, c0, c1}));
    // Five entries use 3-bit indices: the suite 0..4 (stopIndex 4, suiteDepth
    // 4: 0x24), then a run of 2 x entry 2 and the suite 2..3 (0x0b).
    const Rgb g0{0x00, 0x00, 0x00};
    const Rgb g1{0x11, 0x11, 0x11};
    const Rgb g2{0x22, 0x22, 0x22};
    const Rgb g3{0x33, 0x33, 0x33};
    const Rgb g4{0x44, 0x44, 0x44};
    CHECK(decode_ok(decoder,
                    composite("", "",
                              "00 00 00 00 09 00 01 00 14 00 00 00 02  05 00 00 00 11 11 11 22 22 22 33 33 33 44 44 44 "
                              "24 00 0b 02",
                              0, 1),
                    9, 1) == bgra({g0, g1, g2, g3, g4, g2, g2, g2, g3}));
}

TEST_CASE("ClearCodec decoder: pixels no layer covers keep their value")
{
    clear::Decoder decoder;
    std::vector<std::byte> out(3 * 1 * 4, std::byte{0x77});
    REQUIRE(decoder.decode(composite("", "", "01 00 00 00 01 00 01 00 03 00 00 00 00  aa bb cc"), 3, 1, out, 12)
                .has_value());
    CHECK(to_hex(out) == "77 77 77 77 aa bb cc ff 77 77 77 77");
}

// ---------------------------------------------------------------------------
// Decoder: malformed input

TEST_CASE("ClearCodec decoder rejects bad headers")
{
    CHECK(decode_error({}, 1, 1) == Errc::truncated);
    CHECK(decode_error(hex("00"), 1, 1) == Errc::truncated);
    CHECK(decode_error(hex("00 00"), 1, 1) == Errc::truncated);  // no composite payload
    CHECK(decode_error(hex("00 00 00 00 00 00 00 00 00 00 00"), 1, 1) == Errc::truncated);
    CHECK(decode_error(composite("11 22 33 01", "", "", 0x08), 1, 1) == Errc::invalid_value);  // unknown flag
    CHECK(decode_error(composite("11 22 33 01", "", "", 0, 1), 1, 1) == Errc::invalid_value);  // seqNumber 1 first
    CHECK(decode_error(hex("02 00 00 00"), 1, 1) == Errc::invalid_value);  // GLYPH_HIT without GLYPH_INDEX
    CHECK(decode_error(hex("03 00 a0 0f"), 1, 1) == Errc::invalid_value);  // glyphIndex 4000
    CHECK(decode_error(hex("03 00 00 00"), 1, 1) == Errc::invalid_value);  // empty glyph slot
    CHECK(decode_error(hex("03 00 00"), 1, 1) == Errc::truncated);
    // GLYPH_INDEX without a glyphIndex: the byte counts are read as one and
    // come out misaligned.
    CHECK(decode_error(composite("11 22 33 ff 00 04", "", "", clear::flag_glyph_index), 32, 32) == Errc::truncated);

    // A glyph of 1024 pixels (one residual run), stored in slot 0.
    auto glyph = hex("01 00 00 00");
    const auto body = composite("11 22 33 ff 00 04", "", "");
    glyph.insert(glyph.end(), body.begin() + 2, body.end());
    CHECK(decode_error(glyph, 33, 32) == Errc::invalid_value);  // 1056 pixels
    clear::Decoder decoder;
    std::vector<std::byte> out(32 * 32 * 4);
    REQUIRE(decoder.decode(glyph, 32, 32, out, 32 * 4).has_value());
    // The decoder takes each seqNumber as soon as the header is valid.
    CHECK(decode_error(decoder, hex("03 01 00 00"), 32, 33) == Errc::invalid_value);     // above 1024 pixels
    CHECK(decode_error(decoder, hex("03 02 00 00 00"), 32, 32) == Errc::trailing_data);  // payload after a hit
    CHECK(decode_error(decoder, hex("03 02 00 00"), 32, 32) == Errc::invalid_value);     // seqNumber 3 expected
    static_cast<void>(decode_ok(decoder, hex("03 03 00 00"), 16, 64));

    const auto valid = composite("11 22 33 01", "", "");
    CHECK(decode_error(valid, 0, 1) == Errc::invalid_value);
    CHECK(decode_error(valid, 1, 0) == Errc::invalid_value);
    CHECK(decode_error(valid, clear::max_dimension + 1, 1) == Errc::limit_exceeded);
    CHECK(decode_error(valid, 1, clear::max_dimension + 1) == Errc::limit_exceeded);
    auto trailing = valid;
    trailing.push_back(std::byte{0});
    CHECK(decode_error(trailing, 1, 1) == Errc::trailing_data);
    auto short_count = valid;
    short_count[2] = std::byte{5};  // residualByteCount 5 of 4
    CHECK(decode_error(short_count, 1, 1) == Errc::truncated);
}

TEST_CASE("ClearCodec decoder rejects bad residual data")
{
    CHECK(decode_error(composite("11 22 33 00", "", ""), 1, 1) == Errc::invalid_value);   // run of 0
    CHECK(decode_error(composite("11 22 33 02", "", ""), 1, 1) == Errc::invalid_length);  // overrun
    CHECK(decode_error(composite("11 22 33 01", "", ""), 2, 1) == Errc::invalid_length);  // underfill
    CHECK(decode_error(composite("11 22 33 01 44", "", ""), 2, 1) == Errc::truncated);
    CHECK(decode_error(composite("11 22 33 ff 01", "", ""), 16, 16) == Errc::truncated);
    CHECK(decode_error(composite("11 22 33 ff ff ff 00 00 01", "", ""), 256, 256) == Errc::truncated);
    clear::Decoder decoder;
    static_cast<void>(decode_ok(decoder, composite("11 22 33 ff 00 01", "", ""), 16, 16));
    static_cast<void>(decode_ok(decoder, composite("11 22 33 ff ff ff 00 00 01 00", "", "", 0, 1), 256, 256));
}

TEST_CASE("ClearCodec decoder rejects bad bands")
{
    const auto bands_error = [](std::string_view bands, std::uint32_t width, std::uint32_t height) {
        return decode_error(composite("", bands, ""), width, height);
    };
    CHECK(bands_error("01 00 00 00 00 00 00 00 ff ff ff 00 00", 4, 4) == Errc::invalid_value);  // xEnd < xStart
    CHECK(bands_error("00 00 00 00 01 00 00 00 ff ff ff 00 00", 4, 4) == Errc::invalid_value);  // yEnd < yStart
    CHECK(bands_error("00 00 04 00 00 00 00 00 ff ff ff", 4, 4) == Errc::invalid_value);        // x outside
    CHECK(bands_error("00 00 00 00 00 00 04 00 ff ff ff", 4, 4) == Errc::invalid_value);        // y outside
    CHECK(bands_error("00 00 00 00 00 00 34 00 ff ff ff", 1, 60) == Errc::invalid_value);       // 53 rows
    CHECK(bands_error("00 00 00 00 00 00 00 00 ff ff", 1, 1) == Errc::truncated);
    CHECK(bands_error("00 00 00 00 00 00 00 00 ff ff ff 00", 1, 1) == Errc::truncated);
    CHECK(bands_error("00 00 00 00 00 00 00 00 ff ff ff 00 80", 1, 1) == Errc::invalid_value);     // empty V-bar
    CHECK(bands_error("00 00 00 00 00 00 00 00 ff ff ff 00 40 00", 1, 1) == Errc::invalid_value);  // empty short
    CHECK(bands_error("00 00 00 00 00 00 00 00 ff ff ff 00 40", 1, 1) == Errc::truncated);
    CHECK(bands_error("00 00 00 00 00 00 00 00 ff ff ff 01 00", 1, 1) == Errc::invalid_value);  // yOff < yOn
    CHECK(bands_error("00 00 00 00 00 00 00 00 ff ff ff 00 02 11 22 33 44 55 66", 1, 1) ==
          Errc::invalid_value);  // yOff past the band
    CHECK(bands_error("00 00 00 00 00 00 00 00 ff ff ff 00 01 11 22", 1, 1) == Errc::truncated);
    CHECK(bands_error("00 00 01 00 00 00 00 00 ff ff ff 00 00", 2, 1) == Errc::truncated);  // one V-bar short

    // A V-bar hit must name a V-bar of the band's height.
    clear::Decoder decoder;
    static_cast<void>(decode_ok(decoder, composite("", "00 00 00 00 00 00 01 00 ff ff ff 00 00", ""), 1, 2));
    CHECK(decode_error(decoder, composite("", "00 00 00 00 00 00 00 00 ff ff ff 00 80", "", 0, 1), 1, 1) ==
          Errc::invalid_value);
}

TEST_CASE("ClearCodec decoder rejects bad subcodecs")
{
    const auto subcodec_error = [](std::string_view subcodecs, std::uint32_t width, std::uint32_t height) {
        return decode_error(composite("", "", subcodecs), width, height);
    };
    // Header: xStart, yStart, width, height, bitmapDataByteCount, subCodecId.
    CHECK(subcodec_error("01 00 00 00 04 00 01 00 0c 00 00 00 00", 4, 1) == Errc::invalid_value);  // outside
    CHECK(subcodec_error("00 00 00 00 01 00 02 00 06 00 00 00 00", 1, 1) == Errc::invalid_value);  // outside
    CHECK(subcodec_error("00 00 00 00 01 00 01 00 04 00 00 00 02 01 11 22 33", 1, 1) ==
          Errc::invalid_length);  // byteCount > 3 * width * height
    CHECK(subcodec_error("00 00 00 00 01 00 01 00 02 00 00 00 00 11 22", 1, 1) == Errc::invalid_length);
    CHECK(subcodec_error("00 00 00 00 01 00 01 00 03 00 00 00 01 11 22 33", 1, 1) == Errc::unsupported);  // NSCodec
    CHECK(subcodec_error("00 00 00 00 01 00 01 00 03 00 00 00 03 11 22 33", 1, 1) == Errc::invalid_value);
    CHECK(subcodec_error("00 00 00 00 01 00 01 00 03 00 00 00 00 11 22", 1, 1) == Errc::truncated);
    CHECK(subcodec_error("00 00 00 00 01 00 01 00 03 00 00", 1, 1) == Errc::truncated);
    // RLEX.
    CHECK(subcodec_error("00 00 00 00 01 00 01 00 01 00 00 00 02 00", 1, 1) == Errc::invalid_value);    // 0 colours
    CHECK(subcodec_error("00 00 00 00 10 00 10 00 01 00 00 00 02 80", 16, 16) == Errc::invalid_value);  // 128
    CHECK(subcodec_error("00 00 00 00 02 00 01 00 05 00 00 00 02 01 11 22 33", 2, 1) == Errc::truncated);
    CHECK(subcodec_error("00 00 00 00 01 00 01 00 03 00 00 00 02 01 11 22", 1, 1) == Errc::truncated);
    CHECK(subcodec_error("00 00 00 00 02 00 01 00 06 00 00 00 02 01 11 22 33 01 00", 2, 1) ==
          Errc::invalid_value);  // stopIndex 1 of 1 colour
    CHECK(subcodec_error("00 00 00 00 04 00 01 00 08 00 00 00 02 02 11 22 33 44 55 66 02", 4, 1) ==
          Errc::truncated);  // runLengthFactor1 missing
    CHECK(subcodec_error("00 00 00 00 04 00 01 00 09 00 00 00 02 02 11 22 33 44 55 66 02 00", 4, 1) ==
          Errc::invalid_value);  // suiteDepth 1 > stopIndex 0
    CHECK(subcodec_error("00 00 00 00 02 00 02 00 06 00 00 00 02 01 11 22 33 00 04", 2, 2) ==
          Errc::invalid_length);  // run 4 + suite 1 > 4 pixels
    CHECK(subcodec_error("00 00 00 00 02 00 02 00 06 00 00 00 02 01 11 22 33 00 01", 2, 2) ==
          Errc::invalid_length);  // 2 of 4 pixels
    CHECK(subcodec_error("00 00 00 00 02 00 02 00 08 00 00 00 02 01 11 22 33 00 ff 03", 2, 2) == Errc::truncated);
}

TEST_CASE("ClearCodec decoder rejects every truncation of a valid stream")
{
    for (const Pattern pattern : {Pattern::text, Pattern::ui, Pattern::checker, Pattern::noise}) {
        for (const bool glyphs : {true, false}) {
            const auto image = make_image(pattern, 30, 20, 4);
            clear::Encoder encoder;
            const auto stream = encoder.encode(image.view(), {.glyph_cache = glyphs});
            for (std::size_t length = 0; length < stream.size(); ++length) {
                INFO("pattern " << static_cast<int>(pattern) << ", glyphs " << glyphs << ", length " << length);
                clear::Decoder decoder;
                std::vector<std::byte> out(30 * 20 * 4);
                CHECK_FALSE(decoder.decode(std::span(stream).first(length), 30, 20, out, 30 * 4).has_value());
            }
        }
    }
}

TEST_CASE("ClearCodec RLEX index width")
{
    CHECK(clear::rlex_index_bits(1) == 1);
    CHECK(clear::rlex_index_bits(2) == 1);
    CHECK(clear::rlex_index_bits(3) == 2);
    CHECK(clear::rlex_index_bits(4) == 2);
    CHECK(clear::rlex_index_bits(5) == 3);
    CHECK(clear::rlex_index_bits(14) == 4);  // [MS-RDPEGFX] 4.1.1.2
    CHECK(clear::rlex_index_bits(64) == 6);
    CHECK(clear::rlex_index_bits(65) == 7);
    CHECK(clear::rlex_index_bits(127) == 7);
}
