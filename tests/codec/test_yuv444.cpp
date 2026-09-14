// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// YUV444 and the AVC444 v1/v2 split. The layouts are checked against
// transcriptions of FreeRDP's encoder and decoder loops (prim_YUV.c), which
// index the planes differently from farland's code.

#include <farland/codec/image.hpp>
#include <farland/codec/yuv.hpp>
#include <farland/codec/yuv444.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <span>
#include <vector>

namespace codec = farland::codec;
using codec::Avc444Version;
using codec::ChromaFilter;
using codec::Yuv420Frame;
using codec::Yuv444Frame;
using Rect16 = farland::codec::avc::Rect16;

namespace {

struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

/// Tightly packed BGRX.
struct Image {
    std::vector<std::byte> data;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    Image(std::uint32_t w, std::uint32_t h) : data(std::size_t{w} * h * 4, std::byte{0xFF}), width(w), height(h) {}

    void set(std::uint32_t x, std::uint32_t y, Rgb c)
    {
        const std::size_t at = ((std::size_t{y} * width) + x) * 4;
        data[at] = std::byte{c.b};
        data[at + 1] = std::byte{c.g};
        data[at + 2] = std::byte{c.r};
    }

    [[nodiscard]] codec::ImageView view() const
    {
        return {.data = data, .width = width, .height = height, .stride = std::size_t{width} * 4};
    }
};

Image random_image(std::uint32_t w, std::uint32_t h, std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, 255);
    Image image(w, h);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            image.set(x, y,
                      {static_cast<std::uint8_t>(dist(rng)), static_cast<std::uint8_t>(dist(rng)),
                       static_cast<std::uint8_t>(dist(rng))});
        }
    }
    return image;
}

/// Coloured text on coloured backgrounds: one-pixel strokes and dots in
/// saturated colours, where 4:2:0 smears the colour.
Image text_image(std::uint32_t w, std::uint32_t h)
{
    const Rgb backgrounds[] = {{30, 60, 200}, {250, 240, 90}, {20, 150, 40}, {255, 255, 255}};
    const Rgb inks[] = {{255, 40, 40}, {10, 10, 120}, {250, 0, 250}, {0, 170, 255}};
    Image image(w, h);
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t band = (y / 16) % 4;
            // A 5x7 "glyph" cell with a vertical stem, a bar and a dot.
            const std::uint32_t cx = x % 6;
            const std::uint32_t cy = y % 8;
            const bool ink = (cx == 1 && cy < 7) || (cy == 3 && cx < 5) || (cx == 4 && cy == 6);
            image.set(x, y, ink ? inks[band] : backgrounds[(band + (x / 48)) % 4]);
        }
    }
    return image;
}

std::uint8_t sample(std::span<const std::byte> plane, std::size_t stride, std::size_t x, std::size_t y)
{
    return std::to_integer<std::uint8_t>(plane[(y * stride) + x]);
}

std::uint8_t sample(std::span<std::byte> plane, std::size_t stride, std::size_t x, std::size_t y)
{
    return std::to_integer<std::uint8_t>(plane[(y * stride) + x]);
}

struct Views {
    Yuv444Frame yuv;
    Yuv420Frame main;
    Yuv420Frame aux;
};

Views split(const Image& image, std::uint32_t w, std::uint32_t h, Avc444Version version)
{
    Views v{Yuv444Frame(w, h), Yuv420Frame(w, h), Yuv420Frame(w, h)};
    codec::bgrx_to_avc444(image.view(), version, v.yuv, v.main, v.aux);
    return v;
}

// FreeRDP's encoder, general_RGBToAVC444YUV_BGRX: the B4/B5 row for the
// line pair i is n = (i & ~7) + i, and B5 is 8 rows below B4.
std::size_t freerdp_v1_b4_row(std::size_t pair)
{
    return (pair & ~std::size_t{7}) + pair;
}

/// FreeRDP's decoder for a whole picture, transcribed from prim_YUV.c:
/// general_LumaToYUV444, general_ChromaV1ToYUV444 (row counters uY/vY) and
/// general_ChromaV2ToYUV444 (nTotalWidth = the width rounded up to 32).
Yuv444Frame freerdp_combine(const Yuv420Frame& main_frame, const Yuv420Frame& aux_frame, Avc444Version version)
{
    const auto main = main_frame.view();
    const auto aux = aux_frame.view();
    const std::size_t w = main.width;
    const std::size_t h = main.height;
    Yuv444Frame out(main.width, main.height);
    const auto oy = out.y();
    const auto ou = out.u();
    const auto ov = out.v();
    auto put = [&](std::span<std::byte> plane, std::size_t x, std::size_t y, std::byte value) {
        plane[(y * w) + x] = value;
    };
    // Luma: B1, B2 and B3 spread over 2x2.
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            put(oy, x, y, main.y[(y * main.y_stride) + x]);
            put(ou, x, y, main.u[((y / 2) * main.uv_stride) + (x / 2)]);
            put(ov, x, y, main.v[((y / 2) * main.uv_stride) + (x / 2)]);
        }
    }
    if (version == Avc444Version::v1) {
        std::size_t u_y = 0;
        std::size_t v_y = 0;
        const std::size_t pad_height = h + 16 - (h % 16);
        for (std::size_t y = 0; y < pad_height; ++y) {
            std::size_t pos = 0;
            std::span<std::byte> target;
            if (y % 16 < 8) {
                pos = (2 * u_y++) + 1;
                target = ou;
            } else {
                pos = (2 * v_y++) + 1;
                target = ov;
            }
            if (pos >= h) {
                continue;
            }
            for (std::size_t x = 0; x < w; ++x) {
                put(target, x, pos, aux.y[(y * aux.y_stride) + x]);
            }
        }
        for (std::size_t y = 0; y < h / 2; ++y) {
            for (std::size_t x = 0; x < w / 2; ++x) {
                put(ou, (2 * x) + 1, 2 * y, aux.u[(y * aux.uv_stride) + x]);
                put(ov, (2 * x) + 1, 2 * y, aux.v[(y * aux.uv_stride) + x]);
            }
        }
    } else {
        const std::size_t total = w + ((w % 32 != 0) ? 32 - (w % 32) : 0);
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < (w + 1) / 2; ++x) {
                put(ou, (2 * x) + 1, y, aux.y[(y * aux.y_stride) + x]);
                put(ov, (2 * x) + 1, y, aux.y[(y * aux.y_stride) + (total / 2) + x]);
            }
        }
        for (std::size_t y = 0; y < (h + 1) / 2; ++y) {
            for (std::size_t x = 0; x < (w + 3) / 4; ++x) {
                put(ou, 4 * x, (2 * y) + 1, aux.u[(y * aux.uv_stride) + x]);
                put(ov, 4 * x, (2 * y) + 1, aux.u[(y * aux.uv_stride) + (total / 4) + x]);
                put(ou, (4 * x) + 2, (2 * y) + 1, aux.v[(y * aux.uv_stride) + x]);
                put(ov, (4 * x) + 2, (2 * y) + 1, aux.v[(y * aux.uv_stride) + (total / 4) + x]);
            }
        }
    }
    return out;
}

Yuv444Frame combine(const Yuv420Frame& main, const Yuv420Frame& aux, Avc444Version version)
{
    Yuv444Frame out(main.width(), main.height());
    const Rect16 all{0, 0, static_cast<std::uint16_t>(main.width()), static_cast<std::uint16_t>(main.height())};
    codec::apply_main_view(main.view(), all, out);
    codec::apply_aux_view(aux.view(), version, all, out);
    return out;
}

/// Largest difference between two planes over the top-left w x h.
int max_difference(std::span<const std::byte> a, std::span<const std::byte> b, std::size_t stride, std::size_t w,
                   std::size_t h)
{
    int worst = 0;
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            worst = std::max(worst, std::abs(sample(a, stride, x, y) - sample(b, stride, x, y)));
        }
    }
    return worst;
}

double psnr_bgrx(std::span<const std::byte> a, std::span<const std::byte> b)
{
    REQUIRE(a.size() == b.size());
    double sum = 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (i % 4 == 3) {
            continue;
        }
        const double d = std::to_integer<int>(a[i]) - std::to_integer<int>(b[i]);
        sum += d * d;
        ++count;
    }
    const double mse = sum / static_cast<double>(count);
    return mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

int max_rgb_difference(std::span<const std::byte> a, std::span<const std::byte> b)
{
    int worst = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (i % 4 != 3) {
            worst = std::max(worst, std::abs(std::to_integer<int>(a[i]) - std::to_integer<int>(b[i])));
        }
    }
    return worst;
}

}  // namespace

TEST_CASE("bgrx_to_yuv444 converts every pixel and pads the edges")
{
    const auto image = random_image(21, 13, 3);
    Yuv444Frame out(32, 16);
    codec::bgrx_to_yuv444(image.view(), out);
    for (std::uint32_t y = 0; y < 16; ++y) {
        for (std::uint32_t x = 0; x < 32; ++x) {
            const std::size_t at = ((std::size_t{std::min(y, 12U)} * 21) + std::min(x, 20U)) * 4;
            const auto b = std::to_integer<std::uint8_t>(image.data[at]);
            const auto g = std::to_integer<std::uint8_t>(image.data[at + 1]);
            const auto r = std::to_integer<std::uint8_t>(image.data[at + 2]);
            CAPTURE(x, y);
            REQUIRE(sample(out.y(), 32, x, y) == codec::rgb_to_y(r, g, b));
            REQUIRE(sample(out.u(), 32, x, y) == codec::rgb_to_u(r, g, b));
            REQUIRE(sample(out.v(), 32, x, y) == codec::rgb_to_v(r, g, b));
        }
    }
}

TEST_CASE("AVC444 main view: luma and the rounded 2x2 chroma average")
{
    for (const auto version : {Avc444Version::v1, Avc444Version::v2}) {
        const auto image = random_image(32, 32, 4);
        auto v = split(image, 32, 32, version);
        for (std::size_t y = 0; y < 32; ++y) {
            for (std::size_t x = 0; x < 32; ++x) {
                REQUIRE(sample(v.main.y(), 32, x, y) == sample(v.yuv.y(), 32, x, y));
            }
        }
        for (std::size_t y = 0; y < 16; ++y) {
            for (std::size_t x = 0; x < 16; ++x) {
                for (int plane = 0; plane < 2; ++plane) {
                    const auto full = plane == 0 ? v.yuv.u() : v.yuv.v();
                    const unsigned sum = unsigned{sample(full, 32, 2 * x, 2 * y)} +
                                         unsigned{sample(full, 32, (2 * x) + 1, 2 * y)} +
                                         unsigned{sample(full, 32, 2 * x, (2 * y) + 1)} +
                                         unsigned{sample(full, 32, (2 * x) + 1, (2 * y) + 1)};
                    const auto half = plane == 0 ? v.main.u() : v.main.v();
                    REQUIRE(sample(half, 16, x, y) == (sum + 2) / 4);
                }
            }
        }
    }
}

TEST_CASE("AVC444 v1 auxiliary view matches FreeRDP's encoder layout")
{
    // 48x48: three macroblock rows, so the B4/B5 interleave repeats.
    auto v = split(random_image(48, 48, 5), 48, 48, Avc444Version::v1);
    const std::size_t w = 48;
    for (std::size_t pair = 0; pair < 24; ++pair) {
        const std::size_t b4 = freerdp_v1_b4_row(pair);
        for (std::size_t x = 0; x < w; ++x) {
            CAPTURE(pair, x);
            REQUIRE(sample(v.aux.y(), w, x, b4) == sample(v.yuv.u(), w, x, (2 * pair) + 1));      // B4
            REQUIRE(sample(v.aux.y(), w, x, b4 + 8) == sample(v.yuv.v(), w, x, (2 * pair) + 1));  // B5
        }
        for (std::size_t x = 0; x < w / 2; ++x) {
            REQUIRE(sample(v.aux.u(), w / 2, x, pair) == sample(v.yuv.u(), w, (2 * x) + 1, 2 * pair));  // B6
            REQUIRE(sample(v.aux.v(), w / 2, x, pair) == sample(v.yuv.v(), w, (2 * x) + 1, 2 * pair));  // B7
        }
    }
}

TEST_CASE("AVC444 v2 auxiliary view matches FreeRDP's encoder layout")
{
    auto v = split(random_image(64, 32, 6), 64, 32, Avc444Version::v2);
    const std::size_t w = 64;
    for (std::size_t y = 0; y < 32; ++y) {
        for (std::size_t x = 0; x < w / 2; ++x) {
            CAPTURE(x, y);
            REQUIRE(sample(v.aux.y(), w, x, y) == sample(v.yuv.u(), w, (2 * x) + 1, y));            // B4
            REQUIRE(sample(v.aux.y(), w, (w / 2) + x, y) == sample(v.yuv.v(), w, (2 * x) + 1, y));  // B5
        }
    }
    for (std::size_t y = 0; y < 16; ++y) {
        for (std::size_t x = 0; x < w / 4; ++x) {
            CAPTURE(x, y);
            REQUIRE(sample(v.aux.u(), w / 2, x, y) == sample(v.yuv.u(), w, 4 * x, (2 * y) + 1));                  // B6
            REQUIRE(sample(v.aux.u(), w / 2, (w / 4) + x, y) == sample(v.yuv.v(), w, 4 * x, (2 * y) + 1));        // B7
            REQUIRE(sample(v.aux.v(), w / 2, x, y) == sample(v.yuv.u(), w, (4 * x) + 2, (2 * y) + 1));            // B8
            REQUIRE(sample(v.aux.v(), w / 2, (w / 4) + x, y) == sample(v.yuv.v(), w, (4 * x) + 2, (2 * y) + 1));  // B9
        }
    }
}

TEST_CASE("AVC444 split and combine reproduce the YUV444 picture")
{
    for (const auto version : {Avc444Version::v1, Avc444Version::v2}) {
        CAPTURE(version == Avc444Version::v1 ? "v1" : "v2");
        // 64 wide: a multiple of 32, where FreeRDP's v2 decoder agrees.
        const auto image = random_image(64, 48, 7);
        auto v = split(image, 64, 48, version);
        const auto combined = combine(v.main, v.aux, version);
        const auto expected = v.yuv.view();
        const auto got = combined.view();

        // Everything but U444 and V444 at (2x, 2y) comes back exactly...
        CHECK(max_difference(expected.y, got.y, 64, 64, 48) == 0);
        for (std::size_t y = 0; y < 48; ++y) {
            for (std::size_t x = 0; x < 64; ++x) {
                if (x % 2 == 0 && y % 2 == 0) {
                    continue;
                }
                CAPTURE(x, y);
                REQUIRE(sample(expected.u, 64, x, y) == sample(got.u, 64, x, y));
                REQUIRE(sample(expected.v, 64, x, y) == sample(got.v, 64, x, y));
            }
        }
        // ...and FreeRDP's decoder puts every sample in the same place.
        const auto reference = freerdp_combine(v.main, v.aux, version);
        CHECK(max_difference(reference.view().y, got.y, 64, 64, 48) == 0);
        CHECK(max_difference(reference.view().u, got.u, 64, 64, 48) == 0);
        CHECK(max_difference(reference.view().v, got.v, 64, 64, 48) == 0);

        // The reverse filter recovers (2x, 2y) to within the rounding of the average.
        std::vector<std::byte> source(image.data.size());
        std::vector<std::byte> shown(image.data.size());
        codec::yuv444_to_bgrx(expected, 64, 48, ChromaFilter::none, source);
        codec::yuv444_to_bgrx(got, 64, 48, ChromaFilter::reverse, shown);
        CHECK(max_rgb_difference(source, shown) <= 5);
    }
}

TEST_CASE("AVC444 keeps coloured text sharp where AVC420 smears it")
{
    constexpr std::uint32_t w = 128;
    constexpr std::uint32_t h = 64;
    const auto image = text_image(w, h);

    // What YUV444 itself allows: the source through the colour conversion.
    Yuv444Frame yuv(w, h);
    codec::bgrx_to_yuv444(image.view(), yuv);
    std::vector<std::byte> reference(image.data.size());
    codec::yuv444_to_bgrx(yuv.view(), w, h, ChromaFilter::none, reference);

    Yuv420Frame i420(w, h);
    codec::bgrx_to_yuv420(image.view(), i420);
    std::vector<std::byte> as_420(image.data.size());
    codec::yuv420_to_bgrx(i420.view(), w, h, as_420);
    const double psnr_420 = psnr_bgrx(reference, as_420);

    for (const auto version : {Avc444Version::v1, Avc444Version::v2}) {
        auto v = split(image, w, h, version);
        const auto combined = combine(v.main, v.aux, version);
        std::vector<std::byte> reverse(image.data.size());
        std::vector<std::byte> freerdp(image.data.size());
        codec::yuv444_to_bgrx(combined.view(), w, h, ChromaFilter::reverse, reverse);
        codec::yuv444_to_bgrx(combined.view(), w, h, ChromaFilter::freerdp, freerdp);
        const double psnr_reverse = psnr_bgrx(reference, reverse);
        const double psnr_freerdp = psnr_bgrx(reference, freerdp);
        CAPTURE(psnr_420, psnr_reverse, psnr_freerdp);
        CHECK(max_rgb_difference(reference, reverse) <= 5);
        CHECK(psnr_reverse >= 45.0);
        // FreeRDP's threshold keeps the 2x2 average for chroma steps below
        // 30, which costs detail at faint edges; 4:2:0 is far worse still.
        CHECK(psnr_freerdp >= 30.0);
        CHECK(psnr_freerdp > psnr_420 + 10.0);
    }
}

TEST_CASE("AVC444 luma subframes show 4:2:0 until their chroma arrives")
{
    constexpr std::uint32_t w = 64;
    constexpr std::uint32_t h = 32;
    const auto image = text_image(w, h);
    for (const auto version : {Avc444Version::v1, Avc444Version::v2}) {
        auto v = split(image, w, h, version);
        Yuv444Frame client(w, h);
        const Rect16 left{0, 0, 32, 32};

        // A luma subframe for the left half: 4:2:0 there, nothing on the right.
        codec::apply_main_view(v.main.view(), left, client);
        CHECK(sample(client.u(), w, 1, 0) == sample(v.main.u(), w / 2, 0, 0));
        CHECK(sample(client.u(), w, 40, 0) == 0);

        // Its chroma subframe completes the left half only.
        codec::apply_aux_view(v.aux.view(), version, left, client);
        for (std::size_t y = 0; y < h; ++y) {
            for (std::size_t x = 0; x < w; ++x) {
                const bool main_sample = x % 2 == 0 && y % 2 == 0;
                if (x < 32 && !main_sample) {
                    REQUIRE(sample(client.u(), w, x, y) == sample(v.yuv.u(), w, x, y));
                    REQUIRE(sample(client.v(), w, x, y) == sample(v.yuv.v(), w, x, y));
                } else if (x >= 32) {
                    REQUIRE(sample(client.u(), w, x, y) == 0);
                }
            }
        }
        // A rectangle whose left edge is not a multiple of 4 (v2 B6..B9).
        const Rect16 odd{34, 2, 50, 30};
        codec::apply_main_view(v.main.view(), odd, client);
        codec::apply_aux_view(v.aux.view(), version, odd, client);
        for (std::size_t y = 2; y < 30; ++y) {
            for (std::size_t x = 34; x < 50; ++x) {
                if (x % 2 != 0 || y % 2 != 0) {
                    CAPTURE(x, y);
                    REQUIRE(sample(client.u(), w, x, y) == sample(v.yuv.u(), w, x, y));
                    REQUIRE(sample(client.v(), w, x, y) == sample(v.yuv.v(), w, x, y));
                }
            }
        }
        CHECK(sample(client.u(), w, 33, 1) == 0);
        CHECK(sample(client.u(), w, 50, 1) == 0);
    }
}

TEST_CASE("The client's reverse filter recovers large chroma steps only")
{
    // One 2x2 block of U: the main view carries the average.
    Yuv444Frame yuv(2, 2);
    const auto set_block = [&](int u00, int u01, int u10, int u11, int average) {
        const auto u = yuv.u();
        u[0] = std::byte{static_cast<std::uint8_t>(average)};
        u[1] = std::byte{static_cast<std::uint8_t>(u01)};
        u[2] = std::byte{static_cast<std::uint8_t>(u10)};
        u[3] = std::byte{static_cast<std::uint8_t>(u11)};
        static_cast<void>(u00);
        std::ranges::fill(yuv.v(), std::byte{128});
        std::ranges::fill(yuv.y(), std::byte{128});
    };
    std::vector<std::byte> out(16);
    const auto blue_of_first = [&](ChromaFilter filter) {
        codec::yuv444_to_bgrx(yuv.view(), 2, 2, filter, out);
        return std::to_integer<int>(out[0]);
    };
    // U00 = 40 among 200s: the average is 160, the reverse gives 40 back.
    set_block(40, 200, 200, 200, 160);
    CHECK(blue_of_first(ChromaFilter::reverse) == codec::yuv_to_b(128, 40, 128));
    CHECK(blue_of_first(ChromaFilter::freerdp) == codec::yuv_to_b(128, 40, 128));
    CHECK(blue_of_first(ChromaFilter::none) == codec::yuv_to_b(128, 160, 128));
    // U00 = 100 among 110s: the step of 10 stays filtered for FreeRDP.
    set_block(100, 110, 110, 110, 108);
    CHECK(blue_of_first(ChromaFilter::reverse) == codec::yuv_to_b(128, 102, 128));
    CHECK(blue_of_first(ChromaFilter::freerdp) == codec::yuv_to_b(128, 108, 128));
}
