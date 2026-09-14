// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/codec/planar.hpp>
#include <farland/proto/bitmap.hpp>
#include <farland/server/frame_encoder.hpp>
#include <farland/server/test_pattern.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

namespace proto = farland::proto;
using farland::Reader;
using farland::codec::ImageView;
using farland::server::BitmapCodec;
using farland::server::FrameEncoder;

namespace {

struct Frame {
    std::uint32_t width;
    std::uint32_t height;
    std::vector<std::byte> pixels;

    Frame(std::uint32_t w, std::uint32_t h) : width(w), height(h), pixels(static_cast<std::size_t>(w) * h * 4)
    {
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = static_cast<std::byte>((i * 31U) ^ (i >> 7U));
        }
    }
    [[nodiscard]] ImageView view() const { return {pixels, width, height, static_cast<std::size_t>(width) * 4}; }
    std::byte& at(std::uint32_t x, std::uint32_t y, std::uint32_t channel)
    {
        return pixels.at(((static_cast<std::size_t>(y) * width) + x) * 4 + channel);
    }
};

/// Decodes every rectangle of every update and paints it into `canvas`.
std::size_t paint(const std::vector<std::vector<std::byte>>& updates, Frame& canvas, BitmapCodec codec)
{
    std::size_t rects = 0;
    for (const auto& update : updates) {
        Reader r(update);
        // Named: GCC before 15 lacks C++23's lifetime extension in range-for.
        const auto update_rects = proto::decode_bitmap_update(r).value();
        for (const auto& rect : update_rects) {
            ++rects;
            std::vector<std::byte> decoded(static_cast<std::size_t>(rect.width) * rect.height * 4);
            if (codec == BitmapCodec::planar) {
                REQUIRE((rect.flags & proto::bitmap_flags::compression) != 0);
                REQUIRE(farland::codec::planar::decode(rect.data, rect.width, rect.height,
                                                       farland::codec::planar::Orientation::bottom_up, decoded)
                            .has_value());
            } else {
                // 32 bpp raw: bottom-up BGRA rows.
                for (std::uint32_t row = 0; row < rect.height; ++row) {
                    const auto src = rect.data.subspan(static_cast<std::size_t>(rect.height - 1 - row) * rect.width * 4,
                                                       static_cast<std::size_t>(rect.width) * 4);
                    std::copy(src.begin(), src.end(),
                              decoded.begin() + static_cast<std::ptrdiff_t>(row * rect.width * 4));
                }
            }
            for (std::uint32_t y = rect.dest_top; y <= rect.dest_bottom; ++y) {
                for (std::uint32_t x = rect.dest_left; x <= rect.dest_right; ++x) {
                    for (std::uint32_t ch = 0; ch < 3; ++ch) {
                        canvas.at(x, y, ch) = decoded.at(
                            ((static_cast<std::size_t>(y - rect.dest_top) * rect.width) + (x - rect.dest_left)) * 4 +
                            ch);
                    }
                }
            }
        }
    }
    return rects;
}

bool same_rgb(const Frame& a, const Frame& b)
{
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        if (i % 4 != 3 && a.pixels[i] != b.pixels[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("Frames round-trip through planar bitmap updates, edge tiles included")
{
    for (const auto codec : {BitmapCodec::planar, BitmapCodec::uncompressed}) {
        INFO("codec " << static_cast<int>(codec));
        Frame frame(130, 70);  // three tiles across, two down; widths 64, 64 and 2
        FrameEncoder encoder(frame.width, frame.height, 32, codec, true);
        Frame canvas(130, 70);
        std::ranges::fill(canvas.pixels, std::byte{0});

        const auto first = encoder.encode(frame.view(), 0x10000);
        CHECK(paint(first, canvas, codec) == 6);
        CHECK(same_rgb(frame, canvas));

        CHECK(encoder.encode(frame.view(), 0x10000).empty());  // nothing changed

        frame.at(70, 10, 1) = std::byte{0x42};  // one pixel in tile (1, 0)
        const auto second = encoder.encode(frame.view(), 0x10000);
        CHECK(paint(second, canvas, codec) == 1);
        CHECK(same_rgb(frame, canvas));

        encoder.invalidate(proto::Rectangle16{128, 64, 129, 69});  // the bottom-right tile
        CHECK(paint(encoder.encode(frame.view(), 0x10000), canvas, codec) == 1);
    }
}

TEST_CASE("Updates respect the maximum update size")
{
    Frame frame(256, 128);
    FrameEncoder encoder(frame.width, frame.height, 32, BitmapCodec::uncompressed, true);
    const std::size_t limit = 20000;  // one raw 64x64x4 tile is 16402 bytes with its header
    const auto updates = encoder.encode(frame.view(), limit);
    CHECK(updates.size() == 8);
    for (const auto& update : updates) {
        CHECK(update.size() <= limit);
    }
}

TEST_CASE("The test pattern animates and follows the pointer")
{
    farland::server::TestPattern pattern(320, 240);
    const auto a = pattern.render(0);
    const std::vector<std::byte> first(a.data.begin(), a.data.end());
    const auto b = pattern.render(10);
    CHECK_FALSE(std::ranges::equal(first, b.data));

    pattern.apply(proto::MouseEvent{proto::ptr_flags::move, 200, 150});
    const auto c = pattern.render(10);
    const auto pixel = c.data.subspan((150U * 320U + 200U) * 4U, 3);
    CHECK(std::ranges::all_of(pixel, [](std::byte v) { return v == std::byte{0xFF}; }));

    CHECK(farland::server::TestPattern::describe(proto::KeyboardEvent{proto::kbd_flags::release, 0x1e}) ==
          "key 0x1e up");
}
