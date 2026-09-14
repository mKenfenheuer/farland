// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/writer.hpp>
#include <farland/codec/dib.hpp>
#include <farland/codec/png.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using farland::Errc;
using farland::Writer;
using farland::test::hex;
using namespace farland::codec;

namespace {

using Bytes = std::vector<std::byte>;

RgbaImage gradient(std::uint32_t width, std::uint32_t height, bool alpha)
{
    RgbaImage image{width, height, {}};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            image.pixels.push_back(static_cast<std::uint8_t>(x * 13));
            image.pixels.push_back(static_cast<std::uint8_t>(y * 7));
            image.pixels.push_back(static_cast<std::uint8_t>((x + y) * 5));
            image.pixels.push_back(alpha ? static_cast<std::uint8_t>(255 - x - y) : 255);
        }
    }
    return image;
}

/// BITMAPINFOHEADER for a packed DIB.
void info_header(Writer& w, std::int32_t width, std::int32_t height, std::uint16_t bpp, std::uint32_t compression,
                 std::uint32_t colors = 0)
{
    w.u32le(40);
    w.u32le(static_cast<std::uint32_t>(width));
    w.u32le(static_cast<std::uint32_t>(height));
    w.u16le(1);
    w.u16le(bpp);
    w.u32le(compression);
    w.u32le(0);
    w.u32le(0);
    w.u32le(0);
    w.u32le(colors);
    w.u32le(0);
}

Bytes read_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    const std::vector<char> content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    Bytes bytes(content.size());
    std::ranges::transform(content, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

}  // namespace

TEST_CASE("DIB: 32-bit round trips")
{
    const auto alpha = gradient(5, 3, true);
    const auto v5 = encode_dib(alpha, DibHeader::v5);
    CHECK(v5.size() == 124 + (5 * 3 * 4));
    CHECK(decode_dib(v5).value() == alpha);

    const auto opaque = gradient(4, 2, false);
    const auto info = encode_dib(opaque, DibHeader::info);
    CHECK(info.size() == 40 + (4 * 2 * 4));
    CHECK(decode_dib(info).value() == opaque);
    // BI_RGB has no alpha: CF_DIB of an image with alpha comes back opaque.
    const auto flattened = decode_dib(encode_dib(alpha, DibHeader::info)).value();
    CHECK(flattened.opaque());
    CHECK(flattened.pixels[0] == alpha.pixels[0]);
}

TEST_CASE("DIB: 24-bit bottom-up and top-down")
{
    Writer w;
    info_header(w, 2, 2, 24, 0);
    // Bottom row first: blue, white; then red, green. Rows pad to 4 bytes.
    w.bytes(hex("ff 00 00 ff ff ff 00 00"));
    w.bytes(hex("00 00 ff 00 ff 00 00 00"));
    const auto image = decode_dib(w.view()).value();
    CHECK(image.pixels ==
          std::vector<std::uint8_t>{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255});

    Writer top;
    info_header(top, 2, -2, 24, 0);
    top.bytes(hex("00 00 ff 00 ff 00 00 00"));
    top.bytes(hex("ff 00 00 ff ff ff 00 00"));
    CHECK(decode_dib(top.view()).value() == image);
}

TEST_CASE("DIB: palettes and bit fields")
{
    SECTION("1 bit")
    {
        Writer w;
        info_header(w, 3, 1, 1, 0);
        w.bytes(hex("00 00 00 00 ff ff ff 00"));
        w.bytes(hex("a0 00 00 00"));  // 1 0 1
        CHECK(decode_dib(w.view()).value().pixels ==
              std::vector<std::uint8_t>{255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255});
    }
    SECTION("8 bits with a short table")
    {
        Writer w;
        info_header(w, 2, 1, 8, 0, 2);
        w.bytes(hex("10 20 30 00 40 50 60 00"));
        w.bytes(hex("01 00 00 00"));
        CHECK(decode_dib(w.view()).value().pixels ==
              std::vector<std::uint8_t>{0x60, 0x50, 0x40, 255, 0x30, 0x20, 0x10, 255});
        Writer bad;
        info_header(bad, 1, 1, 8, 0, 2);
        bad.bytes(hex("10 20 30 00 40 50 60 00"));
        bad.bytes(hex("05 00 00 00"));
        CHECK(decode_dib(bad.view()).error().code == Errc::invalid_value);
    }
    SECTION("16-bit 5-6-5 with masks after the header")
    {
        Writer w;
        info_header(w, 2, 1, 16, 3);
        w.u32le(0xF800);
        w.u32le(0x07E0);
        w.u32le(0x001F);
        w.bytes(hex("00 f8 1f 00"));  // red, blue
        CHECK(decode_dib(w.view()).value().pixels == std::vector<std::uint8_t>{255, 0, 0, 255, 0, 0, 255, 255});
    }
    SECTION("16-bit 5-5-5")
    {
        Writer w;
        info_header(w, 1, 1, 16, 0);
        w.bytes(hex("e0 03 00 00"));  // green
        CHECK(decode_dib(w.view()).value().pixels == std::vector<std::uint8_t>{0, 255, 0, 255});
    }
    SECTION("an empty alpha mask means opaque")
    {
        auto v5 = encode_dib(gradient(2, 2, true), DibHeader::v5);
        for (std::size_t i = 124 + 3; i < v5.size(); i += 4) {
            v5[i] = std::byte{0};
        }
        CHECK(decode_dib(v5).value().opaque());
    }
}

TEST_CASE("DIB: malformed headers")
{
    const auto check = [](std::int32_t width, std::int32_t height, std::uint16_t bpp, std::uint32_t compression,
                          Errc expected) {
        Writer w;
        info_header(w, width, height, bpp, compression);
        w.zeros(64);
        CHECK(decode_dib(w.view()).error().code == expected);
    };
    check(0, 1, 32, 0, Errc::invalid_value);
    check(1, 0, 32, 0, Errc::invalid_value);
    check(1, 1, 7, 0, Errc::invalid_value);
    check(1, 1, 8, 1, Errc::unsupported);  // BI_RLE8
    check(1, 1, 24, 3, Errc::invalid_value);
    check(40000, 1, 32, 0, Errc::limit_exceeded);
    check(20000, 20000, 32, 0, Errc::limit_exceeded);
    check(100, 100, 32, 0, Errc::truncated);
    CHECK(decode_dib(hex("0c 00 00 00 01 00 01 00 01 00 18 00")).error().code == Errc::unsupported);
    CHECK(decode_dib(hex("28 00 00")).error().code == Errc::truncated);

    Writer masks;
    info_header(masks, 1, 1, 32, 3);
    masks.u32le(0x00FF00FF);  // not contiguous
    masks.u32le(0x0000FF00);
    masks.u32le(0x000000FF);
    masks.zeros(4);
    CHECK(decode_dib(masks.view()).error().code == Errc::invalid_value);
}

TEST_CASE("DIB and BMP files")
{
    const auto dib = encode_dib(gradient(3, 2, true), DibHeader::v5);
    const auto bmp = dib_to_bmp(dib).value();
    CHECK(bmp.size() == dib.size() + 14);
    CHECK(Bytes(bmp.begin(), bmp.begin() + 14) == hex("42 4d a2 00 00 00 00 00 00 00 8a 00 00 00"));
    CHECK(bmp_to_dib(bmp).value() == dib);

    SECTION("a gap before the pixels")
    {
        Writer w;
        w.bytes(hex("42 4d 00 00 00 00 00 00 00 00"));
        w.u32le(14 + 40 + 6);
        info_header(w, 1, 1, 24, 0);
        w.bytes(hex("aa bb cc dd ee ff"));
        w.bytes(hex("01 02 03 00"));
        const auto packed = bmp_to_dib(w.view()).value();
        CHECK(packed.size() == 44);
        CHECK(decode_dib(packed).value().pixels == std::vector<std::uint8_t>{3, 2, 1, 255});
    }
    CHECK(bmp_to_dib(hex("42 4e 00 00")).error().code == Errc::invalid_value);
    auto short_bmp = bmp;
    short_bmp.resize(bmp.size() - 1);
    CHECK_FALSE(bmp_to_dib(short_bmp).has_value());
}

TEST_CASE("PNG: round trips")
{
    if (!png_supported()) {
        CHECK(encode_png(gradient(1, 1, false)).error().code == Errc::unsupported);
        SKIP("built without zlib");
    }
    const bool alpha = GENERATE(false, true);
    const auto image = gradient(37, 11, alpha);
    const auto png = encode_png(image).value();
    CHECK(png[25] == std::byte{static_cast<std::uint8_t>(alpha ? 6 : 2)});  // IHDR colour type
    const auto decoded = decode_png(png);
    if (!decoded) {
        FAIL(decoded.error().message());
    }
    CHECK(*decoded == image);
    CHECK(decode_png(png, 37 * 11 - 1).error().code == Errc::limit_exceeded);
}

TEST_CASE("PNG: every colour type, depth and interlacing")
{
    if (!png_supported()) {
        SKIP("built without zlib");
    }
    const std::string name = GENERATE("gray1", "gray16-trns", "rgb8-trns", "rgb16", "pal2-trns", "pal8", "graya8",
                                      "rgba8-adam7", "gray4-adam7", "rgba16");
    INFO(name);
    const auto dir = std::filesystem::path(FARLAND_TEST_DATA_DIR) / "png";
    const auto png = read_file(dir / (name + ".png"));
    const auto expected = read_file(dir / (name + ".rgba"));
    const auto image = decode_png(png);
    if (!image) {
        FAIL(image.error().message());
    }
    const auto u32 = [&](std::size_t at) {
        return std::to_integer<std::uint32_t>(expected[at]) | (std::to_integer<std::uint32_t>(expected[at + 1]) << 8U) |
               (std::to_integer<std::uint32_t>(expected[at + 2]) << 16U) |
               (std::to_integer<std::uint32_t>(expected[at + 3]) << 24U);
    };
    CHECK(image->width == u32(0));
    CHECK(image->height == u32(4));
    std::vector<std::uint8_t> pixels;
    for (std::size_t i = 8; i < expected.size(); ++i) {
        pixels.push_back(std::to_integer<std::uint8_t>(expected[i]));
    }
    CHECK(image->pixels == pixels);
}

TEST_CASE("PNG: malformed files")
{
    if (!png_supported()) {
        SKIP("built without zlib");
    }
    const auto good = encode_png(gradient(4, 4, true)).value();
    CHECK(decode_png(hex("89 50 4e 47 0d 0a 1a 0b")).error().code == Errc::invalid_value);
    auto bad_crc = good;
    bad_crc[29] ^= std::byte{1};
    CHECK(decode_png(bad_crc).error().code == Errc::invalid_value);
    auto truncated = good;
    truncated.resize(good.size() - 12);  // no IEND
    CHECK(decode_png(truncated).error().code == Errc::truncated);
    CHECK(decode_png(Bytes(good.begin(), good.begin() + 20)).error().code == Errc::truncated);
}
