// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/codec/image.hpp>
#include <farland/codec/yuv.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <span>
#include <vector>

using farland::codec::ImageView;
using farland::codec::Yuv420Frame;

namespace codec = farland::codec;

namespace {

struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

/// BGRX pixels with a padded stride; unused bytes hold 0xEE.
struct TestImage {
    std::vector<std::byte> data;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t stride = 0;

    TestImage(std::uint32_t w, std::uint32_t h) : width(w), height(h), stride((std::size_t{w} * 4) + 12)
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

    [[nodiscard]] ImageView view() const { return {.data = data, .width = width, .height = height, .stride = stride}; }
};

TestImage random_image(std::uint32_t w, std::uint32_t h, std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, 255);
    TestImage image(w, h);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            image.set(x, y,
                      {static_cast<std::uint8_t>(dist(rng)), static_cast<std::uint8_t>(dist(rng)),
                       static_cast<std::uint8_t>(dist(rng))});
        }
    }
    return image;
}

std::uint8_t at(std::span<const std::byte> plane, std::size_t stride, std::size_t x, std::size_t y)
{
    return std::to_integer<std::uint8_t>(plane[(y * stride) + x]);
}

// FreeRDP's AVC420 encoder, libfreerdp/primitives/prim_YUV.c
// general_RGBToYUV420_BGRX with prim_internal.h RGB2Y/RGB2U/RGB2V, for even
// sizes: Y per pixel, U and V from the 2x2 sums shifted right by 2.
int freerdp_y(int r, int g, int b)
{
    return ((54 * r) + (183 * g) + (18 * b)) >> 8;
}
int freerdp_u(int r, int g, int b)
{
    return (((-29 * r) - (99 * g) + (128 * b)) >> 8) + 128;
}
int freerdp_v(int r, int g, int b)
{
    return (((128 * r) - (116 * g) - (12 * b)) >> 8) + 128;
}

}  // namespace

TEST_CASE("AVC colour conversion is full-range BT.709")
{
    // [MS-RDPEGFX] 3.3.8.3.1: full-range BT.709. Kr = 0.2126, Kb = 0.0722.
    constexpr double kr = 0.2126;
    constexpr double kb = 0.0722;
    constexpr double kg = 1.0 - kr - kb;
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> dist(0, 255);
    for (int i = 0; i < 2000; ++i) {
        const auto r = static_cast<std::uint8_t>(dist(rng));
        const auto g = static_cast<std::uint8_t>(dist(rng));
        const auto b = static_cast<std::uint8_t>(dist(rng));
        const double y = (kr * r) + (kg * g) + (kb * b);
        const double u = ((b - y) / (2 * (1 - kb))) + 128;
        const double v = ((r - y) / (2 * (1 - kr))) + 128;
        // The 8-bit coefficients and the truncating shift cost at most 2.
        CHECK(std::abs(codec::rgb_to_y(r, g, b) - y) < 2.0);
        CHECK(std::abs(codec::rgb_to_u(r, g, b) - u) < 2.0);
        CHECK(std::abs(codec::rgb_to_v(r, g, b) - v) < 2.0);
    }
    // Neither BT.601 (green would be 150) nor limited range (black 16, white 235).
    CHECK(codec::rgb_to_y(0, 255, 0) == 182);
    CHECK(codec::rgb_to_y(0, 0, 0) == 0);
    CHECK(codec::rgb_to_y(255, 255, 255) == 254);
}

TEST_CASE("AVC colour conversion of primaries matches FreeRDP")
{
    struct Case {
        Rgb rgb;
        std::uint8_t y;
        std::uint8_t u;
        std::uint8_t v;
    };
    // Worked from prim_internal.h RGB2Y/RGB2U/RGB2V.
    const Case cases[] = {
        {{255, 255, 255}, 254, 128, 128}, {{0, 0, 0}, 0, 128, 128},    {{255, 0, 0}, 53, 99, 255},
        {{0, 255, 0}, 182, 29, 12},       {{0, 0, 255}, 17, 255, 116}, {{128, 128, 128}, 127, 128, 128},
    };
    for (const auto& c : cases) {
        CAPTURE(c.rgb.r, c.rgb.g, c.rgb.b);
        CHECK(codec::rgb_to_y(c.rgb.r, c.rgb.g, c.rgb.b) == c.y);
        CHECK(codec::rgb_to_u(c.rgb.r, c.rgb.g, c.rgb.b) == c.u);
        CHECK(codec::rgb_to_v(c.rgb.r, c.rgb.g, c.rgb.b) == c.v);
    }
    // FreeRDP's decoder (YUV2R/YUV2G/YUV2B) inverts that.
    CHECK(codec::yuv_to_r(53, 99, 255) == 252);
    CHECK(codec::yuv_to_g(53, 99, 255) == 0);
    CHECK(codec::yuv_to_b(53, 99, 255) == 0);
}

TEST_CASE("AVC colour conversion round-trips through FreeRDP's decoder")
{
    std::mt19937 rng(11);
    std::uniform_int_distribution<int> dist(0, 255);
    int worst = 0;
    for (int i = 0; i < 20000; ++i) {
        const auto r = static_cast<std::uint8_t>(dist(rng));
        const auto g = static_cast<std::uint8_t>(dist(rng));
        const auto b = static_cast<std::uint8_t>(dist(rng));
        const auto y = codec::rgb_to_y(r, g, b);
        const auto u = codec::rgb_to_u(r, g, b);
        const auto v = codec::rgb_to_v(r, g, b);
        worst = std::max({worst, std::abs(codec::yuv_to_r(y, u, v) - r), std::abs(codec::yuv_to_g(y, u, v) - g),
                          std::abs(codec::yuv_to_b(y, u, v) - b)});
    }
    CHECK(worst <= 4);
}

TEST_CASE("bgrx_to_yuv420 equals FreeRDP's encoder on even images")
{
    const auto image = random_image(34, 18, 1);
    Yuv420Frame out(34, 18);
    codec::bgrx_to_yuv420(image.view(), out);
    const auto yuv = out.view();

    for (std::uint32_t y = 0; y < 18; ++y) {
        for (std::uint32_t x = 0; x < 34; ++x) {
            const Rgb c = image.get(x, y);
            REQUIRE(at(yuv.y, yuv.y_stride, x, y) == freerdp_y(c.r, c.g, c.b));
        }
    }
    for (std::uint32_t by = 0; by < 9; ++by) {
        for (std::uint32_t bx = 0; bx < 17; ++bx) {
            int r = 0;
            int g = 0;
            int b = 0;
            for (const auto& [dx, dy] : {std::pair{0U, 0U}, {1U, 0U}, {0U, 1U}, {1U, 1U}}) {
                const Rgb c = image.get((2 * bx) + dx, (2 * by) + dy);
                r += c.r;
                g += c.g;
                b += c.b;
            }
            REQUIRE(at(yuv.u, yuv.uv_stride, bx, by) == freerdp_u(r >> 2, g >> 2, b >> 2));
            REQUIRE(at(yuv.v, yuv.uv_stride, bx, by) == freerdp_v(r >> 2, g >> 2, b >> 2));
        }
    }
}

TEST_CASE("bgrx_to_yuv420 pads to the coded size by repeating the edges")
{
    // A 5x3 image in a 16x16 picture: odd sides repeat the last pixel inside
    // the 2x2 blocks, the rest repeats the last column and row.
    const auto image = random_image(5, 3, 2);
    Yuv420Frame out(16, 16);
    codec::bgrx_to_yuv420(image.view(), out);
    const auto yuv = out.view();

    auto expected_y = [&](std::uint32_t x, std::uint32_t y) {
        const Rgb c = image.get(std::min(x, 4U), std::min(y, 2U));
        return freerdp_y(c.r, c.g, c.b);
    };
    for (std::uint32_t y = 0; y < 16; ++y) {
        for (std::uint32_t x = 0; x < 16; ++x) {
            CAPTURE(x, y);
            REQUIRE(at(yuv.y, yuv.y_stride, x, y) == expected_y(x, y));
        }
    }
    auto expected_uv = [&](std::uint32_t bx, std::uint32_t by, bool v_plane) {
        bx = std::min(bx, 2U);
        by = std::min(by, 1U);
        int r = 0;
        int g = 0;
        int b = 0;
        for (const auto& [dx, dy] : {std::pair{0U, 0U}, {1U, 0U}, {0U, 1U}, {1U, 1U}}) {
            const Rgb c = image.get(std::min((2 * bx) + dx, 4U), std::min((2 * by) + dy, 2U));
            r += c.r;
            g += c.g;
            b += c.b;
        }
        return v_plane ? freerdp_v(r >> 2, g >> 2, b >> 2) : freerdp_u(r >> 2, g >> 2, b >> 2);
    };
    for (std::uint32_t by = 0; by < 8; ++by) {
        for (std::uint32_t bx = 0; bx < 8; ++bx) {
            CAPTURE(bx, by);
            REQUIRE(at(yuv.u, yuv.uv_stride, bx, by) == expected_uv(bx, by, false));
            REQUIRE(at(yuv.v, yuv.uv_stride, bx, by) == expected_uv(bx, by, true));
        }
    }
}

TEST_CASE("yuv420_to_bgrx crops and converts")
{
    TestImage image(6, 4);
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 6; ++x) {
            image.set(x, y, {200, 100, 50});
        }
    }
    Yuv420Frame yuv(16, 16);
    codec::bgrx_to_yuv420(image.view(), yuv);
    std::vector<std::byte> out(6 * 4 * 4);
    codec::yuv420_to_bgrx(yuv.view(), 6, 4, out);
    for (std::size_t i = 0; i < out.size(); i += 4) {
        CHECK(std::abs(std::to_integer<int>(out[i]) - 50) <= 3);
        CHECK(std::abs(std::to_integer<int>(out[i + 1]) - 100) <= 3);
        CHECK(std::abs(std::to_integer<int>(out[i + 2]) - 200) <= 3);
        CHECK(out[i + 3] == std::byte{0xFF});
    }
}
