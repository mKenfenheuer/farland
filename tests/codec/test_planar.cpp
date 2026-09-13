// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/error.hpp>
#include <farland/base/hexdump.hpp>
#include <farland/codec/image.hpp>
#include <farland/codec/planar.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace planar = farland::codec::planar;
using farland::Errc;
using farland::to_hex;
using farland::codec::ImageView;
using farland::test::hex;
using planar::Mode;
using planar::Orientation;

namespace {

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

    void set(std::uint32_t x, std::uint32_t y, std::uint8_t r, std::uint8_t g, std::uint8_t b)
    {
        const std::size_t at = (y * stride) + (std::size_t{x} * 4);
        data[at] = std::byte{b};
        data[at + 1] = std::byte{g};
        data[at + 2] = std::byte{r};
    }

    [[nodiscard]] ImageView view() const { return {.data = data, .width = width, .height = height, .stride = stride}; }
};

struct Rgb {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
};

/// Pixels given top-down, left to right.
TestImage from_rgb(std::uint32_t width, std::uint32_t height, std::initializer_list<Rgb> pixels)
{
    REQUIRE(pixels.size() == std::size_t{width} * height);
    TestImage image(width, height);
    std::uint32_t i = 0;
    for (const Rgb& p : pixels) {
        image.set(i % width, i / width, p.r, p.g, p.b);
        ++i;
    }
    return image;
}

enum class Pattern { solid, gradient, stripes, random };

TestImage make_image(Pattern pattern, std::uint32_t width, std::uint32_t height, std::size_t stride = 0)
{
    TestImage image(width, height, stride);
    std::uint32_t lcg = 0x1234'5678;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            switch (pattern) {
            case Pattern::solid:
                image.set(x, y, 0x80, 0x40, 0x20);
                break;
            case Pattern::gradient:
                image.set(x, y, static_cast<std::uint8_t>(x + y), static_cast<std::uint8_t>(y * 4),
                          static_cast<std::uint8_t>(x * 4));
                break;
            case Pattern::stripes: {
                const bool dark = (x / 3) % 2 == 0;
                image.set(x, y, dark ? 0x10 : 0xF0, dark ? 0x20 : 0xE0, dark ? 0x30 : 0xD0);
                break;
            }
            case Pattern::random: {
                std::array<std::uint8_t, 3> rgb{};
                for (auto& c : rgb) {
                    lcg = (lcg * 1664525U) + 1013904223U;  // Numerical Recipes LCG
                    c = static_cast<std::uint8_t>(lcg >> 24U);
                }
                image.set(x, y, rgb[0], rgb[1], rgb[2]);
                break;
            }
            }
        }
    }
    return image;
}

/// What decode() yields for `image`: top-down B, G, R, 0xFF, stride width * 4.
std::vector<std::byte> expected_pixels(const TestImage& image, bool flipped = false)
{
    std::vector<std::byte> out;
    for (std::uint32_t row = 0; row < image.height; ++row) {
        const std::uint32_t y = flipped ? image.height - 1 - row : row;
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::size_t at = (y * image.stride) + (std::size_t{x} * 4);
            out.insert(out.end(), {image.data[at], image.data[at + 1], image.data[at + 2], std::byte{0xFF}});
        }
    }
    return out;
}

std::vector<std::byte> decode_ok(std::span<const std::byte> stream, std::uint32_t width, std::uint32_t height,
                                 Orientation orientation)
{
    std::vector<std::byte> out(std::size_t{width} * height * 4);
    const auto result = planar::decode(stream, width, height, orientation, out);
    INFO((result.has_value() ? std::string("ok") : result.error().message()));
    REQUIRE(result.has_value());
    return out;
}

Errc decode_error(std::span<const std::byte> stream, std::uint32_t width, std::uint32_t height,
                  Orientation orientation = Orientation::bottom_up)
{
    std::vector<std::byte> out(std::size_t{width} * height * 4);
    const auto result = planar::decode(stream, width, height, orientation, out);
    REQUIRE_FALSE(result.has_value());
    return result.error().code;
}

std::string encode_hex(const TestImage& image, Mode mode, Orientation orientation = Orientation::bottom_up)
{
    return to_hex(planar::encode(image.view(), {.mode = mode, .orientation = orientation}));
}

/// Checks that `wire` is what the encoder produces and that it decodes back.
void check_vector(const TestImage& image, Mode mode, Orientation orientation, std::string_view wire)
{
    const auto bytes = hex(wire);
    CHECK(encode_hex(image, mode, orientation) == to_hex(bytes));
    CHECK(decode_ok(bytes, image.width, image.height, orientation) == expected_pixels(image));
}

constexpr std::array all_modes{Mode::raw, Mode::rle, Mode::automatic};
constexpr std::array all_orientations{Orientation::top_down, Orientation::bottom_up};

}  // namespace

TEST_CASE("Planar raw stream: header, R, G, B planes, pad ([MS-RDPEGDI] 2.2.2.5.1)")
{
    const auto image = from_rgb(4, 1, {{0x11, 0x22, 0x33}, {0x44, 0x55, 0x66}, {0x77, 0x88, 0x99}, {0xaa, 0xbb, 0xcc}});
    // 20: FormatHeader with NA (no alpha plane), no RLE, CLL 0, no CS.
    // Red, Green and Blue planes, one byte per pixel, then the pad byte.
    check_vector(image, Mode::raw, Orientation::bottom_up, "20 11 44 77 aa 22 55 88 bb 33 66 99 cc 00");
    // All values differ, so RLE (1 + 3 * 5 bytes) loses to raw (14 bytes).
    CHECK(encode_hex(image, Mode::automatic) == encode_hex(image, Mode::raw));
    CHECK(planar::encode(image.view(), {.mode = Mode::rle}).size() == 16);
}

TEST_CASE("Planar raw stream: scanline order follows the orientation")
{
    // Top row R 10 20 30, bottom row R 12 1e 30.
    const auto image = from_rgb(3, 2,
                                {{0x10, 0x05, 0xff},
                                 {0x20, 0x05, 0x80},
                                 {0x30, 0x05, 0x7f},
                                 {0x12, 0x05, 0x00},
                                 {0x1e, 0x05, 0x00},
                                 {0x30, 0x05, 0x00}});
    check_vector(image, Mode::raw, Orientation::bottom_up,
                 "20 12 1e 30 10 20 30  05 05 05 05 05 05  00 00 00 ff 80 7f  00");
    check_vector(image, Mode::raw, Orientation::top_down,
                 "20 10 20 30 12 1e 30  05 05 05 05 05 05  ff 80 7f 00 00 00  00");
}

TEST_CASE("Planar RLE: raw values and runs of the last value (4x1)")
{
    const auto image = from_rgb(4, 1, {{0xff, 0x00, 0x01}, {0xff, 0x00, 0x02}, {0xff, 0x00, 0x02}, {0xff, 0x00, 0x02}});
    // 30: FormatHeader with NA and RLE.
    // Red ff ff ff ff:  13 ff = 1 raw value (ff), then a run of 3 repeating it.
    // Green 00 00 00 00: 04 = no raw values, a run of 4 repeating the implicit 0.
    // Blue 01 02 02 02: 40 01 02 02 02 = 4 raw values; 02 02 02 would be one raw
    //   value plus a run of 2, and runs of 1 or 2 cannot be coded.
    check_vector(image, Mode::rle, Orientation::bottom_up, "30 13 ff 04 40 01 02 02 02");
    CHECK(encode_hex(image, Mode::automatic) == "30 13 ff 04 40 01 02 02 02");
}

TEST_CASE("Planar RLE: later scanlines are sign-magnitude deltas ([MS-RDPEGDI] 3.1.9)")
{
    // Top row:    R 10 20 30, G 05 05 05, B ff 80 7f
    // Bottom row: R 12 1e 30, G 05 05 05, B 00 00 00
    const auto image = from_rgb(3, 2,
                                {{0x10, 0x05, 0xff},
                                 {0x20, 0x05, 0x80},
                                 {0x30, 0x05, 0x7f},
                                 {0x12, 0x05, 0x00},
                                 {0x1e, 0x05, 0x00},
                                 {0x30, 0x05, 0x00}});

    // Bottom-up: the stream starts with the bottom row.
    // Red:   30 12 1e 30   first scanline, three raw values
    //        30 03 04 00   10-12 = -2 -> 2*2-1 = 03; 20-1e = +2 -> 2*2 = 04; 0 -> 00
    // Green: 30 05 05 05   a run must follow a raw value, and 05 + run 2 is not codable
    //        03            deltas 00 00 00: run of 3 of the implicit 0
    // Blue:  03            values 00 00 00: run of 3
    //        30 01 ff fe   ff-00 = -1 -> 01; 80-00 = -128 -> 2*128-1 = ff; 7f-00 = +127 -> fe
    check_vector(image, Mode::rle, Orientation::bottom_up,
                 "30  30 12 1e 30 30 03 04 00  30 05 05 05 03  03 30 01 ff fe");

    // Top-down: the stream starts with the top row.
    // Red:   30 10 20 30, then 12-10 = +2 -> 04; 1e-20 = -2 -> 03; 0 -> 00
    // Blue:  30 ff 80 7f, then 00-ff = +1 (mod 256) -> 02; 00-80 = -128 -> ff; 00-7f = -127 -> 2*127-1 = fd
    check_vector(image, Mode::rle, Orientation::top_down,
                 "30  30 10 20 30 30 04 03 00  30 05 05 05 03  30 ff 80 7f 30 02 ff fd");

    // Automatic mode: bottom-up RLE (19 bytes) beats raw (20 bytes); top-down
    // RLE (22 bytes, the blue deltas are not a run) does not.
    CHECK(encode_hex(image, Mode::automatic, Orientation::bottom_up) ==
          encode_hex(image, Mode::rle, Orientation::bottom_up));
    CHECK(encode_hex(image, Mode::automatic, Orientation::top_down) ==
          encode_hex(image, Mode::raw, Orientation::top_down));
}

TEST_CASE("Planar RLE: long runs use the 16+n and 32+n escapes")
{
    SECTION("50x1")
    {
        TestImage image(50, 1);
        for (std::uint32_t x = 0; x < 50; ++x) {
            image.set(x, 0, 0xaa, 0x00, x < 20 ? 0x00 : 0x01);
        }
        // Red, aa x 50: 1f aa = raw aa + run 15; 22 = escape 2 with cRawBytes 2: run 32+2 = 34.
        // Green, 00 x 50: f2 = escape 2 with cRawBytes 15: run 47; 03 = run 3.
        // Blue, 00 x 20 then 01 x 30: 41 = escape 1 with cRawBytes 4: run 16+4 = 20 of 0;
        //   1f 01 = raw 01 + run 15; 0e = run 14.
        check_vector(image, Mode::rle, Orientation::bottom_up, "30  1f aa 22  f2 03  41 1f 01 0e");
    }
    SECTION("49x1: no segment is left with a run of 1 or 2")
    {
        const auto image = make_image(Pattern::solid, 49, 1);  // R 80, G 40, B 20
        // Each plane is one raw value + run 48: 1f 80 = raw + run 15 inline;
        // 12 = escape 2 with cRawBytes 1: run 33.
        check_vector(image, Mode::rle, Orientation::bottom_up, "30  1f 80 12  1f 40 12  1f 20 12");

        TestImage black(49, 1);
        for (std::uint32_t x = 0; x < 49; ++x) {
            black.set(x, 0, 0, 0, 0);
        }
        // Run 49 = 46 + 3 (47 would leave 2): e2 = escape 2 with 14: run 46; 03 = run 3.
        check_vector(black, Mode::rle, Orientation::bottom_up, "30  e2 03  e2 03  e2 03");
    }
    SECTION("17x1: an inline run shortened to leave 3")
    {
        const auto image = make_image(Pattern::solid, 17, 1);
        // Raw value + run 16: 1d 80 = raw + run 13; 03 = run 3.
        check_vector(image, Mode::rle, Orientation::bottom_up, "30  1d 80 03  1d 40 03  1d 20 03");
    }
}

TEST_CASE("Planar RLE: more than 15 raw values take several segments")
{
    TestImage image(17, 1);
    for (std::uint32_t x = 0; x < 17; ++x) {
        image.set(x, 0, static_cast<std::uint8_t>(x), 0, 0);
    }
    // Red 00..10: f0 + 15 values, 20 + 2 values. Green, Blue: 11 = escape 1 with 1: run 17.
    check_vector(image, Mode::rle, Orientation::bottom_up,
                 "30  f0 00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 20 0f 10  11  11");
}

TEST_CASE("Planar decoder: alpha plane first when NA is clear")
{
    // 2x1, alpha 80 ff, R 01 02, G 03 04, B 05 06.
    const auto expected = hex("05 03 01 80 06 04 02 ff");
    CHECK(decode_ok(hex("00 80 ff 01 02 03 04 05 06 00"), 2, 1, Orientation::bottom_up) == expected);
    CHECK(decode_ok(hex("00 80 ff 01 02 03 04 05 06"), 2, 1, Orientation::bottom_up) == expected);  // no pad
    CHECK(decode_ok(hex("10 20 80 ff 20 01 02 20 03 04 20 05 06"), 2, 1, Orientation::bottom_up) == expected);
    // RLE alpha with a run: 13 80 = 80 x 4, then all-zero colour planes.
    CHECK(decode_ok(hex("10 13 80 04 04 04"), 4, 1, Orientation::top_down) ==
          hex("00 00 00 80 00 00 00 80 00 00 00 80 00 00 00 80"));
}

TEST_CASE("Planar round trip in every mode and orientation")
{
    constexpr std::array<std::array<std::uint32_t, 2>, 6> sizes{{{1, 1}, {3, 7}, {4, 4}, {64, 64}, {65, 33}, {257, 3}}};
    constexpr std::array patterns{Pattern::solid, Pattern::gradient, Pattern::stripes, Pattern::random};

    for (const auto [width, height] : sizes) {
        for (const Pattern pattern : patterns) {
            const auto image = make_image(pattern, width, height);
            const auto expected = expected_pixels(image);
            const auto flipped = expected_pixels(image, true);
            const std::size_t raw_size = 1 + (std::size_t{3} * width * height) + 1;
            for (const Mode mode : all_modes) {
                for (const Orientation orientation : all_orientations) {
                    INFO("size " << width << "x" << height << ", pattern " << static_cast<int>(pattern) << ", mode "
                                 << static_cast<int>(mode) << ", orientation " << static_cast<int>(orientation));
                    const auto stream = planar::encode(image.view(), {.mode = mode, .orientation = orientation});
                    REQUIRE_FALSE(stream.empty());
                    if (mode == Mode::raw) {
                        CHECK(stream.front() == std::byte{0x20});
                        CHECK(stream.size() == raw_size);
                    } else if (mode == Mode::rle) {
                        CHECK(stream.front() == std::byte{0x30});
                    } else {
                        CHECK(stream.size() <= raw_size);
                    }
                    CHECK(decode_ok(stream, width, height, orientation) == expected);
                    // Decoding with the other orientation mirrors the image vertically.
                    const auto other =
                        orientation == Orientation::top_down ? Orientation::bottom_up : Orientation::top_down;
                    CHECK(decode_ok(stream, width, height, other) == flipped);
                }
            }
        }
    }
}

TEST_CASE("Planar encoder honours a stride larger than width * 4")
{
    for (const Pattern pattern : {Pattern::gradient, Pattern::random}) {
        const auto image = make_image(pattern, 5, 4, 32);
        const auto packed = make_image(pattern, 5, 4);
        for (const Mode mode : all_modes) {
            for (const Orientation orientation : all_orientations) {
                const auto stream = planar::encode(image.view(), {.mode = mode, .orientation = orientation});
                CHECK(stream == planar::encode(packed.view(), {.mode = mode, .orientation = orientation}));
                CHECK(decode_ok(stream, 5, 4, orientation) == expected_pixels(image));
            }
        }
    }
    // The last row needs only width * 4 bytes, not a whole stride.
    auto image = make_image(Pattern::gradient, 5, 4, 32);
    image.data.resize((3 * 32) + (5 * 4));
    CHECK(decode_ok(planar::encode(image.view()), 5, 4, Orientation::bottom_up) == expected_pixels(image));
}

TEST_CASE("Planar compression ratio")
{
    const auto solid = make_image(Pattern::solid, 64, 64);
    const std::size_t raw_size = planar::encode(solid.view(), {.mode = Mode::raw}).size();
    CHECK(raw_size == 1 + (3 * 64 * 64) + 1);
    // Per plane: 1f 80 d2 03 for the first row (raw + run 15 + 45 + 3), f2 11 for
    // each of the 63 delta rows (47 + 17): 3 * (4 + 63 * 2) + 1 = 391 bytes.
    const auto rle = planar::encode(solid.view(), {.mode = Mode::rle});
    CHECK(rle.size() == 391);
    CHECK(rle.size() * 20 < raw_size);

    // Noise does not compress, and automatic falls back to raw planes (header + planes + pad).
    const auto noise = make_image(Pattern::random, 64, 64);
    CHECK(planar::encode(noise.view(), {.mode = Mode::rle}).size() > raw_size);
    CHECK(planar::encode(noise.view()).size() == raw_size);
}

TEST_CASE("Planar decoder rejects truncated streams")
{
    CHECK(decode_error({}, 4, 1) == Errc::truncated);
    CHECK(decode_error(hex("30"), 4, 1) == Errc::truncated);
    CHECK(decode_error(hex("30 13"), 4, 1) == Errc::truncated);  // raw value missing
    CHECK(decode_error(hex("20 01 02 03"), 4, 1) == Errc::truncated);

    for (const Pattern pattern : {Pattern::solid, Pattern::random, Pattern::stripes}) {
        const auto image = make_image(pattern, 13, 11);
        for (const Mode mode : {Mode::raw, Mode::rle}) {
            const auto stream = planar::encode(image.view(), {.mode = mode});
            // A raw stream without its pad byte is still complete.
            const std::size_t complete = mode == Mode::raw ? stream.size() - 1 : stream.size();
            for (std::size_t length = 0; length < complete; ++length) {
                INFO("pattern " << static_cast<int>(pattern) << ", mode " << static_cast<int>(mode) << ", length "
                                << length);
                CHECK(decode_error(std::span(stream).first(length), 13, 11) == Errc::truncated);
            }
        }
    }
}

TEST_CASE("Planar decoder rejects segments that overrun a scanline")
{
    CHECK(decode_error(hex("30 05 04 04"), 4, 1) == Errc::invalid_length);                 // run 5 in a row of 4
    CHECK(decode_error(hex("30 50 01 02 03 04 05 04 04"), 4, 1) == Errc::invalid_length);  // 5 raw values
    CHECK(decode_error(hex("30 23 01 02 04 04"), 4, 1) == Errc::invalid_length);           // 2 raw + run 3
    CHECK(decode_error(hex("30 01 04 04"), 4, 1) == Errc::invalid_length);                 // escape: run 16
    CHECK(decode_error(hex("30 02 04 04"), 4, 1) == Errc::invalid_length);                 // escape: run 32
    // The second scanline of 4x2 overruns after a valid first one.
    CHECK(decode_error(hex("30 04 14 00"), 4, 2) == Errc::invalid_length);  // 1 raw + run 4
    // Segments must not carry on into the next scanline either: 3 + 3 in rows of 2.
    CHECK(decode_error(hex("30 03 03"), 2, 3) == Errc::invalid_length);
}

TEST_CASE("Planar decoder rejects trailing data")
{
    const auto image = make_image(Pattern::gradient, 4, 4);
    auto rle = planar::encode(image.view(), {.mode = Mode::rle});
    rle.push_back(std::byte{0});  // RLE streams have no pad byte
    CHECK(decode_error(rle, 4, 4) == Errc::trailing_data);

    auto raw = planar::encode(image.view(), {.mode = Mode::raw});
    raw.push_back(std::byte{0});
    CHECK(decode_error(raw, 4, 4) == Errc::trailing_data);
}

TEST_CASE("Planar decoder rejects color loss, chroma subsampling and bad sizes")
{
    CHECK(decode_error(hex("21 00 00 00 00"), 1, 1) == Errc::unsupported);  // CLL 1
    CHECK(decode_error(hex("37 01 01 01"), 1, 1) == Errc::unsupported);     // CLL 7, RLE
    CHECK(decode_error(hex("2b 00 00 00 00"), 1, 1) == Errc::unsupported);  // CLL 3 with CS
    CHECK(decode_error(hex("28 00 00 00 00"), 1, 1) == Errc::unsupported);  // CS without CLL

    const auto valid = hex("30 10 00 10 00 10 00");  // 1x1, each plane one raw value 00
    CHECK(decode_error(valid, 0, 1) == Errc::invalid_value);
    CHECK(decode_error(valid, 1, 0) == Errc::invalid_value);
    CHECK(decode_error(valid, planar::max_dimension + 1, 1) == Errc::limit_exceeded);
    CHECK(decode_error(valid, 1, planar::max_dimension + 1) == Errc::limit_exceeded);
    CHECK(decode_ok(valid, 1, 1, Orientation::bottom_up) == hex("00 00 00 ff"));
}
