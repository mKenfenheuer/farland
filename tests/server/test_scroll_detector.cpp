// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/server/scroll_detector.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

using farland::codec::ImageView;
using farland::server::apply_scroll;
using farland::server::detect_vertical_scroll;
using farland::server::PixelRect;
using farland::server::ScrollDetectorConfig;
using farland::server::ScrollMove;

constexpr std::uint32_t width = 1024;
constexpr std::uint32_t height = 512;
constexpr std::uint32_t content_left = 256;
constexpr std::uint32_t content_right = 960;
constexpr PixelRect everything{0, 0, width, height};

std::uint32_t mix(std::uint32_t a, std::uint32_t b, std::uint32_t c)
{
    std::uint32_t h = (a * 0x9E3779B1U) ^ ((b + 0x7F4A7C15U) * 0x85EBCA6BU) ^ (c * 0xC2B2AE35U);
    h ^= h >> 15U;
    h *= 0x2C1B3C6DU;
    h ^= h >> 12U;
    return h;
}

struct Screen {
    std::size_t stride;
    std::vector<std::byte> pixels;

    explicit Screen(std::size_t row_stride) : stride(row_stride), pixels(row_stride * height) {}

    void put(std::uint32_t x, std::uint32_t y, std::uint32_t value)
    {
        const std::size_t at = (std::size_t{y} * stride) + (std::size_t{x} * 4);
        for (std::size_t i = 0; i < 4; ++i) {
            pixels[at + i] = static_cast<std::byte>(value >> (8 * i));
        }
    }

    [[nodiscard]] ImageView view() const { return {pixels, width, height, stride}; }
};

/// A desktop: a static sidebar, a document showing its lines from
/// `first_line` on (every fifth line blank), and a scrollbar that differs in
/// every `frame`.
Screen desktop(std::uint32_t first_line, std::uint32_t frame, std::size_t stride = std::size_t{width} * 4,
               std::uint32_t document = 1)
{
    Screen screen(stride);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint32_t line = first_line + y;
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint32_t value = 0;
            if (x < content_left) {
                value = mix(x, y, 7);
            } else if (x < content_right) {
                value = line % 5 == 0 ? 0xFFFFFFFFU : mix(x, line, document);
            } else {
                value = mix(x, y, 100 + frame);
            }
            screen.put(x, y, value);
        }
    }
    return screen;
}

/// Every destination row of `move` in `current` equals its source row in `previous`.
bool move_is_exact(const Screen& previous, const Screen& current, const ScrollMove& move)
{
    const auto destination = move.destination();
    for (std::uint32_t i = 0; i < move.source.height; ++i) {
        const auto from = std::span(previous.pixels)
                              .subspan((std::size_t{move.source.y + i} * previous.stride) + (std::size_t{move.source.x} * 4),
                                       std::size_t{move.source.width} * 4);
        const auto to = std::span(current.pixels)
                            .subspan((std::size_t{destination.y + i} * current.stride) + (std::size_t{destination.x} * 4),
                                     std::size_t{move.source.width} * 4);
        if (!std::ranges::equal(from, to)) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("A document scrolled up is one move beside a static sidebar and a changing scrollbar", "[server][scroll]")
{
    const Screen previous = desktop(100, 0);
    const Screen current = desktop(137, 1, (std::size_t{width} * 4) + 64);  // another stride, too

    const auto move = detect_vertical_scroll(previous.view(), current.view(), everything);

    REQUIRE(move.has_value());
    CHECK(move->dy == -37);
    CHECK(move->source == PixelRect{content_left, 37, content_right - content_left, height - 37});
    CHECK(move->destination() == PixelRect{content_left, 0, content_right - content_left, height - 37});
    CHECK(move_is_exact(previous, current, *move));
}

TEST_CASE("A document scrolled down is one move", "[server][scroll]")
{
    const Screen previous = desktop(100, 0);
    const Screen current = desktop(60, 1);

    const auto move = detect_vertical_scroll(previous.view(), current.view(), everything);

    REQUIRE(move.has_value());
    CHECK(move->dy == 40);
    CHECK(move->source == PixelRect{content_left, 0, content_right - content_left, height - 40});
    CHECK(move_is_exact(previous, current, *move));
}

TEST_CASE("Changes that are not a scroll give no move", "[server][scroll]")
{
    const Screen previous = desktop(100, 0);

    SECTION("identical frames")
    {
        CHECK_FALSE(detect_vertical_scroll(previous.view(), previous.view(), everything).has_value());
    }
    SECTION("a different document")
    {
        const Screen current = desktop(100, 1, std::size_t{width} * 4, 2);
        CHECK_FALSE(detect_vertical_scroll(previous.view(), current.view(), everything).has_value());
    }
    SECTION("a blank document scrolled: nothing to tell the rows apart")
    {
        Screen blank_before(std::size_t{width} * 4);
        Screen blank_after(std::size_t{width} * 4);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                blank_before.put(x, y, x < content_right ? 0xFFFFFFFFU : mix(x, y, 1));
                blank_after.put(x, y, x < content_right ? 0xFFFFFFFFU : mix(x, y, 2));
            }
        }
        CHECK_FALSE(detect_vertical_scroll(blank_before.view(), blank_after.view(), everything).has_value());
    }
}

TEST_CASE("Scroll detection keeps to its limits", "[server][scroll]")
{
    const Screen previous = desktop(100, 0);
    const Screen current = desktop(137, 1);

    SECTION("farther than max_distance")
    {
        CHECK_FALSE(
            detect_vertical_scroll(previous.view(), current.view(), everything, ScrollDetectorConfig{.max_distance = 20})
                .has_value());
    }
    SECTION("an area too short for min_rows of moved content")
    {
        CHECK_FALSE(
            detect_vertical_scroll(previous.view(), current.view(), PixelRect{content_left, 0, 704, 64}).has_value());
    }
    SECTION("only inside the area")
    {
        const PixelRect area{content_left, 128, 512, 256};
        const auto move = detect_vertical_scroll(previous.view(), current.view(), area);
        REQUIRE(move.has_value());
        CHECK(move->dy == -37);
        CHECK(move->source == PixelRect{content_left, 128 + 37, 512, 256 - 37});
        CHECK(move_is_exact(previous, current, *move));
    }
}

TEST_CASE("apply_scroll turns the previous frame into the current one where the move is", "[server][scroll]")
{
    for (const std::uint32_t first_line : {137U, 60U}) {
        Screen previous = desktop(100, 0);
        const Screen current = desktop(first_line, 1);
        const auto move = detect_vertical_scroll(previous.view(), current.view(), everything);
        REQUIRE(move.has_value());

        const Screen original = previous;
        apply_scroll(previous.pixels, previous.stride, *move);

        CHECK(move_is_exact(original, previous, *move));
        CHECK(move_is_exact(original, current, *move));
        // The sidebar, outside the move, is untouched.
        CHECK(std::ranges::equal(std::span(previous.pixels).first(std::size_t{content_left} * 4),
                                 std::span(original.pixels).first(std::size_t{content_left} * 4)));
    }
}
