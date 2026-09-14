// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/platform/portal/pixel_formats.hpp>

#include <catch2/catch_test_macros.hpp>
#include <spa/param/video/raw.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

using farland::platform::Rect;
using namespace farland::platform::portal;

namespace {

/// Memory byte order of each layout, as in its name.
std::string_view byte_order(PixelLayout layout)
{
    switch (layout) {
    case PixelLayout::bgrx:
        return "BGRX";
    case PixelLayout::bgra:
        return "BGRA";
    case PixelLayout::rgbx:
        return "RGBX";
    case PixelLayout::rgba:
        return "RGBA";
    case PixelLayout::xrgb:
        return "XRGB";
    case PixelLayout::argb:
        return "ARGB";
    case PixelLayout::xbgr:
        return "XBGR";
    case PixelLayout::abgr:
        return "ABGR";
    }
    return "";
}

void put_pixel(std::vector<std::byte>& out, PixelLayout layout, std::uint8_t r, std::uint8_t g, std::uint8_t b,
               std::uint8_t a)
{
    for (const char c : byte_order(layout)) {
        switch (c) {
        case 'R':
            out.push_back(std::byte{r});
            break;
        case 'G':
            out.push_back(std::byte{g});
            break;
        case 'B':
            out.push_back(std::byte{b});
            break;
        default:
            out.push_back(std::byte{a});
            break;
        }
    }
}

}  // namespace

TEST_CASE("Pixel layouts map to SPA and DRM formats", "[portal][pixel]")
{
    for (const PixelLayout layout : all_pixel_layouts) {
        CHECK(layout_from_spa(to_spa_format(layout)) == layout);
    }
    CHECK_FALSE(layout_from_spa(SPA_VIDEO_FORMAT_NV12).has_value());
    // DRM_FORMAT_XRGB8888 ('XR24') is PipeWire's BGRx on little-endian.
    CHECK(drm_fourcc(PixelLayout::bgrx) == 0x34325258U);
    CHECK(drm_fourcc(PixelLayout::abgr) == 0x34324152U);  // 'RA24'
}

TEST_CASE("Every layout converts to BGRX, honouring the stride", "[portal][pixel]")
{
    constexpr std::uint32_t width = 3;
    constexpr std::uint32_t height = 2;
    constexpr std::size_t stride = (width * 4) + 5;
    for (const PixelLayout layout : all_pixel_layouts) {
        CAPTURE(byte_order(layout));
        std::vector<std::byte> src;
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                put_pixel(src, layout, static_cast<std::uint8_t>(10 + x), static_cast<std::uint8_t>(20 + y),
                          static_cast<std::uint8_t>(30 + x + y), 0x80);
            }
            src.resize(src.size() + (stride - (width * 4)), std::byte{0xee});
        }
        std::vector<std::byte> dst(std::size_t{width} * height * 4);
        convert_to_bgrx(layout, src, stride, width, height, dst);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t i = ((y * width) + x) * 4;
                CHECK(dst[i + 0] == std::byte(30 + x + y));
                CHECK(dst[i + 1] == std::byte(20 + y));
                CHECK(dst[i + 2] == std::byte(10 + x));
            }
        }
    }
}

TEST_CASE("Cursor bitmaps become straight-alpha BGRA", "[portal][pixel]")
{
    std::vector<std::byte> src;
    put_pixel(src, PixelLayout::rgba, 0x40, 0x20, 0x10, 0x80);  // premultiplied, half transparent
    put_pixel(src, PixelLayout::rgba, 0xff, 0x00, 0x00, 0xff);  // opaque red
    put_pixel(src, PixelLayout::rgba, 0x12, 0x34, 0x56, 0x00);  // transparent (garbage colour)

    const auto straight = convert_cursor(PixelLayout::rgba, src, 12, 3, 1, true);
    const std::vector<std::byte> expected{
        std::byte{0x20}, std::byte{0x40}, std::byte{0x80}, std::byte{0x80},  // 0x10*255/0x80 = 0x20 ...
        std::byte{0x00}, std::byte{0x00}, std::byte{0xff}, std::byte{0xff},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
    };
    CHECK(straight == expected);

    const auto untouched = convert_cursor(PixelLayout::rgba, src, 12, 3, 1, false);
    CHECK(untouched[0] == std::byte{0x10});
    CHECK(untouched[3] == std::byte{0x80});

    std::vector<std::byte> opaque;
    put_pixel(opaque, PixelLayout::xrgb, 1, 2, 3, 0);
    CHECK(convert_cursor(PixelLayout::xrgb, opaque, 4, 1, 1, true) ==
          std::vector<std::byte>{std::byte{3}, std::byte{2}, std::byte{1}, std::byte{0xff}});
}

TEST_CASE("Rectangles intersect", "[portal][pixel]")
{
    CHECK(intersect(Rect{-5, -5, 10, 10}, Rect{0, 0, 100, 100}) == Rect{0, 0, 5, 5});
    CHECK(intersect(Rect{90, 10, 20, 5}, Rect{0, 0, 100, 100}) == Rect{90, 10, 10, 5});
    CHECK_FALSE(intersect(Rect{100, 0, 5, 5}, Rect{0, 0, 100, 100}).has_value());
    CHECK_FALSE(intersect(Rect{0, 0, 0, 5}, Rect{0, 0, 100, 100}).has_value());
}

TEST_CASE("Damage accumulates, dedupes and collapses", "[portal][pixel]")
{
    DamageAccumulator damage;
    CHECK(damage.empty());
    damage.add(Rect{0, 0, 10, 10});
    damage.add(Rect{2, 2, 3, 3});  // inside the first
    damage.add(Rect{20, 20, 5, 5});
    CHECK(damage.take() == std::vector<Rect>{Rect{0, 0, 10, 10}, Rect{20, 20, 5, 5}});
    CHECK(damage.empty());

    damage.add(Rect{1, 1, 1, 1});
    damage.add(Rect{0, 0, 4, 4});  // swallows the first
    CHECK(damage.take() == std::vector<Rect>{Rect{0, 0, 4, 4}});

    damage.add(Rect{1, 1, 1, 1});
    damage.add_full();
    damage.add(Rect{5, 5, 1, 1});
    CHECK(damage.full());
    CHECK(damage.take().empty());

    for (std::int32_t i = 0; i <= static_cast<std::int32_t>(DamageAccumulator::max_rects); ++i) {
        damage.add(Rect{i * 10, i, 2, 2});
    }
    const auto bounding = damage.take();
    REQUIRE(bounding.size() == 1);
    CHECK(bounding[0] == Rect{0, 0, (static_cast<std::int32_t>(DamageAccumulator::max_rects) * 10) + 2,
                              static_cast<std::int32_t>(DamageAccumulator::max_rects) + 2});
}
