// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/server/compositor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <vector>

using farland::codec::ImageView;
using farland::server::Compositor;
using farland::server::DisplayLayout;
using farland::server::ScaledPicture;
using Bytes = std::vector<std::byte>;

namespace {

/// A picture whose pixel at x,y is (x, y, x + y) in B, G, R.
Bytes gradient(std::uint32_t width, std::uint32_t height)
{
    Bytes pixels(std::size_t{width} * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t at = ((std::size_t{y} * width) + x) * 4;
            pixels[at] = static_cast<std::byte>(x);
            pixels[at + 1] = static_cast<std::byte>(y);
            pixels[at + 2] = static_cast<std::byte>(x + y);
            pixels[at + 3] = std::byte{0xFF};
        }
    }
    return pixels;
}

int channel(const ImageView& image, std::uint32_t x, std::uint32_t y, std::size_t c)
{
    return std::to_integer<int>(image.data[(y * image.stride) + (std::size_t{x} * 4) + c]);
}

}  // namespace

TEST_CASE("Compositor: halving averages 2x2 blocks")
{
    const auto pixels = gradient(8, 6);
    const ImageView source{pixels, 8, 6, 32};
    ScaledPicture scaled;
    const auto half = scaled.scale(source, 4, 3);
    REQUIRE((half.width == 4 && half.height == 3));
    // Block (2,1) covers x 4..5, y 2..3: B averages 4.5, G 2.5, R 7.
    CHECK(channel(half, 2, 1, 0) == 5);
    CHECK(channel(half, 2, 1, 1) == 3);
    CHECK(channel(half, 2, 1, 2) == 7);
    // The same size is the picture itself.
    CHECK(scaled.scale(source, 8, 6).data.data() == source.data.data());
}

TEST_CASE("Compositor: screens at their targets, scaled where needed, black around them")
{
    // Monitors 16x8 and 8x8; screen 0 is 32x16 (scaled to 16x8), screen 1 is 4x4 (centred).
    const auto layout = DisplayLayout::from_disp({{{.flags = 1, .left = 0, .top = 0, .width = 200, .height = 200},
                                                   {.flags = 0, .left = 200, .top = 0, .width = 200, .height = 200}}},
                                                 {})
                            .value();
    const std::array sizes{std::pair<std::uint32_t, std::uint32_t>{400, 200},
                           std::pair<std::uint32_t, std::uint32_t>{100, 100}};
    const auto out = layout.place(sizes);
    const auto big = gradient(400, 200);
    const auto small = gradient(100, 100);
    const std::array<std::optional<ImageView>, 2> pictures{ImageView{big, 400, 200, 1600},
                                                           ImageView{small, 100, 100, 400}};
    Compositor compositor;
    const auto desktop = compositor.compose(out, pictures);
    REQUIRE((desktop.width == 400 && desktop.height == 200));
    // Screen 0: 200x100 at 0,50; its pixel 10,10 is the average of 20..21, 20..21.
    CHECK(channel(desktop, 10, 60, 0) == 21);
    CHECK(channel(desktop, 10, 60, 1) == 21);
    CHECK(channel(desktop, 10, 10, 0) == 0);  // bar above
    // Screen 1: 100x100 at 250,50, unscaled.
    CHECK(channel(desktop, 250 + 7, 50 + 9, 0) == 7);
    CHECK(channel(desktop, 250 + 7, 50 + 9, 1) == 9);
    CHECK(channel(desktop, 240, 60, 2) == 0);  // bar to the left

    // One unscaled screen covering the desktop is passed through.
    const auto single = DisplayLayout::single(100, 100);
    const std::array one{std::pair<std::uint32_t, std::uint32_t>{100, 100}};
    const std::array<std::optional<ImageView>, 1> only{ImageView{small, 100, 100, 400}};
    CHECK(compositor.compose(single.place(one), only).data.data() == small.data());
}
