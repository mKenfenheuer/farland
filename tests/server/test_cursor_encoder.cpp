// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The cursor encoder: platform cursor updates in, pointer updates out. The
// output is checked the way a client sees it, by encoding, decoding and
// drawing it with farland's own pointer decoder.

#include <farland/proto/pointer.hpp>
#include <farland/server/connection.hpp>
#include <farland/server/cursor_encoder.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace proto = farland::proto;
namespace ptr = farland::proto::pointer;
namespace caps = farland::proto::caps;
using farland::Reader;
using farland::Writer;
using farland::platform::CursorImage;
using farland::platform::CursorUpdate;
using farland::server::CursorEncoder;

namespace {

using Bytes = std::vector<std::byte>;
using Updates = std::vector<proto::PointerUpdate>;

/// Varied colors and alpha, with fully transparent pixels among them.
CursorImage pattern(std::uint32_t width, std::uint32_t height, std::int32_t hx = 0, std::int32_t hy = 0,
                    std::uint32_t seed = 0)
{
    CursorImage image{width, height, hx, hy, {}};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t v = (x * 31U) + (y * 17U) + (seed * 101U);
            const auto alpha = static_cast<std::uint8_t>(v % 7U == 0 ? 0U : 0x40U + (v % 0xC0U));
            image.pixels.push_back(std::byte{static_cast<std::uint8_t>(v)});
            image.pixels.push_back(std::byte{static_cast<std::uint8_t>(v >> 3U)});
            image.pixels.push_back(std::byte{static_cast<std::uint8_t>(x ^ y ^ seed)});
            image.pixels.push_back(std::byte{alpha});
        }
    }
    return image;
}

CursorImage solid(std::uint32_t width, std::uint32_t height, std::uint32_t argb)
{
    CursorImage image{width, height, 0, 0, {}};
    for (std::uint32_t i = 0; i < width * height; ++i) {
        for (const unsigned shift : {0U, 8U, 16U, 24U}) {
            image.pixels.push_back(std::byte{static_cast<std::uint8_t>(argb >> shift)});
        }
    }
    return image;
}

CursorUpdate show(CursorImage image)
{
    CursorUpdate update;
    update.shape = std::move(image);
    return update;
}

CursorUpdate hide()
{
    CursorUpdate update;
    update.visible = false;
    return update;
}

CursorUpdate move(std::int32_t x, std::int32_t y)
{
    CursorUpdate update;
    update.position.emplace(x, y);
    return update;
}

CursorEncoder::Config config(std::uint16_t cache = 25)
{
    CursorEncoder::Config c;
    c.pointer_cache_size = cache;
    c.color_pointer_cache_size = cache;
    c.max_size = ptr::max_size;
    c.max_update_size = std::size_t{1} << 20U;
    return c;
}

const ptr::Shape& shape_of(const proto::PointerUpdate& update)
{
    if (const auto* p = std::get_if<ptr::NewPointer>(&update)) {
        return p->shape;
    }
    if (const auto* p = std::get_if<ptr::LargePointer>(&update)) {
        return p->shape;
    }
    return std::get<ptr::ColorPointer>(update).shape;
}

/// What a client draws for a shape update: through the wire and back.
Bytes drawn(const proto::PointerUpdate& update)
{
    Writer w;
    const std::uint8_t code = ptr::encode_fastpath(w, update);
    Reader r(w.view());
    const auto decoded = ptr::decode_fastpath(code, r).value();
    CHECK(decoded == update);
    return ptr::shape_to_bgra(shape_of(decoded)).value();
}

/// The input as a 32 bpp pointer shows it: fully transparent pixels lose their color.
Bytes expected32(const CursorImage& image)
{
    Bytes out = image.pixels;
    for (std::size_t i = 0; i < out.size(); i += 4) {
        if (out[i + 3] == std::byte{0}) {
            std::fill_n(out.begin() + static_cast<std::ptrdiff_t>(i), 4, std::byte{0});
        }
    }
    return out;
}

std::optional<std::uint16_t> new_index(const Updates& out)
{
    if (out.size() != 1 || !std::holds_alternative<ptr::NewPointer>(out[0])) {
        return std::nullopt;
    }
    return std::get<ptr::NewPointer>(out[0]).shape.cache_index;
}

std::optional<std::uint16_t> cached_index(const Updates& out)
{
    if (out.size() != 1 || !std::holds_alternative<ptr::CachedPointer>(out[0])) {
        return std::nullopt;
    }
    return std::get<ptr::CachedPointer>(out[0]).cache_index;
}

}  // namespace

TEST_CASE("A new shape is a 32 bpp New Pointer the client draws exactly")
{
    CursorEncoder encoder(config());
    const auto image = pattern(20, 13, 3, 4);
    const auto out = encoder.encode(show(image));
    REQUIRE(out.size() == 1);
    REQUIRE(std::holds_alternative<ptr::NewPointer>(out[0]));
    const auto& shape = shape_of(out[0]);
    CHECK(shape.xor_bpp == 32);
    CHECK(shape.cache_index == 0);
    CHECK(shape.width == 20);
    CHECK(shape.height == 13);
    CHECK(shape.hotspot_x == 3);
    CHECK(shape.hotspot_y == 4);
    CHECK(drawn(out[0]) == expected32(image));

    Writer w;
    ptr::encode_slow_path(w, out[0]);
    Reader r(w.view());
    CHECK(ptr::decode_slow_path(r).value() == out[0]);
}

TEST_CASE("Shapes the client has are Cached Pointer Updates; the cache is LRU")
{
    CursorEncoder encoder(config(2));
    const auto a = pattern(8, 8, 0, 0, 1);
    const auto b = pattern(8, 8, 0, 0, 2);
    const auto c = pattern(8, 8, 0, 0, 3);

    CHECK(new_index(encoder.encode(show(a))) == 0);
    CHECK(encoder.encode(show(a)).empty());  // already showing
    CHECK(new_index(encoder.encode(show(b))) == 1);
    CHECK(cached_index(encoder.encode(show(a))) == 0);
    CHECK(new_index(encoder.encode(show(c))) == 1);  // evicts b
    CHECK(new_index(encoder.encode(show(b))) == 0);  // evicts a
    CHECK(new_index(encoder.encode(show(a))) == 1);  // evicts c
    CHECK(cached_index(encoder.encode(show(b))) == 0);

    // The hotspot is part of the shape: a new entry, replacing a (the LRU one).
    auto moved = a;
    moved.hotspot_x = 5;
    CHECK(new_index(encoder.encode(show(moved))) == 1);
}

TEST_CASE("Hiding and showing the cursor")
{
    CursorEncoder encoder(config());
    CHECK(encoder.encode(CursorUpdate{}).empty());  // nothing known yet: the client's default stays
    CHECK(encoder.encode(hide()) == Updates{ptr::Hidden{}});
    CHECK(encoder.encode(hide()).empty());
    CHECK(encoder.encode(CursorUpdate{}) == Updates{ptr::Default{}});

    const auto a = pattern(8, 8, 0, 0, 1);
    CHECK(new_index(encoder.encode(show(a))) == 0);
    CHECK(encoder.encode(hide()) == Updates{ptr::Hidden{}});
    CHECK(cached_index(encoder.encode(CursorUpdate{})) == 0);

    // A shape that arrives while hidden waits until the cursor shows again.
    auto hidden_shape = hide();
    hidden_shape.shape = pattern(8, 8, 0, 0, 2);
    CHECK(encoder.encode(hide()) == Updates{ptr::Hidden{}});
    CHECK(encoder.encode(hidden_shape).empty());
    CHECK(new_index(encoder.encode(CursorUpdate{})) == 1);
}

TEST_CASE("Large shapes: Large Pointer Updates when negotiated, scaled down otherwise")
{
    const auto big = pattern(200, 150, 199, 75);

    auto large_config = config();
    large_config.large_pointers = true;
    CursorEncoder large(large_config);
    auto out = large.encode(show(big));
    REQUIRE(out.size() == 1);
    REQUIRE(std::holds_alternative<ptr::LargePointer>(out[0]));
    CHECK(shape_of(out[0]).hotspot_x == 199);
    CHECK(shape_of(out[0]).hotspot_y == 75);
    CHECK(drawn(out[0]) == expected32(big));
    // Up to 96 x 96 stays a New Pointer; above 384 is scaled to 384.
    CHECK(new_index(large.encode(show(pattern(96, 96)))) == 1);
    out = large.encode(show(pattern(500, 250)));
    REQUIRE(std::holds_alternative<ptr::LargePointer>(out.at(0)));
    CHECK(shape_of(out[0]).width == 384);
    CHECK(shape_of(out[0]).height == 192);

    CursorEncoder scaled(config());
    out = scaled.encode(show(big));
    REQUIRE(out.size() == 1);
    REQUIRE(std::holds_alternative<ptr::NewPointer>(out[0]));
    const auto& shape = shape_of(out[0]);
    CHECK(shape.width == 96);
    CHECK(shape.height == 72);
    CHECK(shape.hotspot_x == 95);
    CHECK(shape.hotspot_y == 36);

    // Area averaging keeps uniform color and alpha.
    const auto flat = solid(200, 150, 0x80604020);
    out = scaled.encode(show(flat));
    const auto pixels = drawn(out.at(0));
    CHECK(pixels == solid(96, 72, 0x80604020).pixels);

    // Without LARGE_POINTER_FLAG_96x96 the limit is 32 x 32.
    auto legacy_config = config();
    legacy_config.max_size = ptr::max_legacy_size;
    CursorEncoder legacy(legacy_config);
    out = legacy.encode(show(pattern(64, 64, 63, 0)));
    CHECK(shape_of(out.at(0)).width == 32);
    CHECK(shape_of(out.at(0)).height == 32);
    CHECK(shape_of(out.at(0)).hotspot_x == 31);
}

TEST_CASE("Shapes are scaled until they fit the update size limit")
{
    auto slow_path = config();
    slow_path.max_update_size = 0x3FFF - 64;  // Connection::max_update_size() without fast-path
    CursorEncoder encoder(slow_path);
    const auto out = encoder.encode(show(pattern(96, 96)));
    REQUIRE(out.size() == 1);
    CHECK(shape_of(out[0]).width == 62);
    CHECK(shape_of(out[0]).height == 62);
    CHECK(ptr::fastpath_size(out[0]) + 4 <= slow_path.max_update_size);
}

TEST_CASE("Without a pointer cache: 24 bpp Color Pointers; without any cache: the default pointer")
{
    auto color_config = config();
    color_config.pointer_cache_size = 0;
    color_config.color_pointer_cache_size = 20;
    CursorEncoder color(color_config);
    // Opaque, half-transparent (kept, alpha >= 128), mostly transparent, clear.
    auto image = solid(4, 1, 0);
    image.pixels = Bytes{std::byte{1},  std::byte{2},    std::byte{3},  std::byte{0xFF}, std::byte{4}, std::byte{5},
                         std::byte{6},  std::byte{0x80}, std::byte{7},  std::byte{8},    std::byte{9}, std::byte{0x7F},
                         std::byte{10}, std::byte{11},   std::byte{12}, std::byte{0}};
    const auto out = color.encode(show(image));
    REQUIRE(out.size() == 1);
    REQUIRE(std::holds_alternative<ptr::ColorPointer>(out[0]));
    CHECK(shape_of(out[0]).xor_bpp == 24);
    CHECK(drawn(out[0]) == Bytes{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{0xFF}, std::byte{4}, std::byte{5},
                                 std::byte{6}, std::byte{0xFF}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                                 std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}});

    CursorEncoder none(config(0));
    CHECK(none.encode(hide()) == Updates{ptr::Hidden{}});
    CHECK(none.encode(show(pattern(8, 8))) == Updates{ptr::Default{}});
}

TEST_CASE("Positions are sent only when asked for, and only when they change")
{
    CursorEncoder quiet(config());
    CHECK(quiet.encode(move(10, 20)).empty());
    CHECK(quiet.warp(5, 6) == proto::PointerUpdate{ptr::Position{5, 6}});

    auto loud_config = config();
    loud_config.send_positions = true;
    CursorEncoder loud(loud_config);
    CHECK(loud.encode(move(10, 20)) == Updates{ptr::Position{10, 20}});
    CHECK(loud.encode(move(10, 20)).empty());
    loud.client_moved(30, 40);
    CHECK(loud.encode(move(30, 40)).empty());  // only the echo of the client's own move
    CHECK(loud.encode(move(-5, 70000)) == Updates{ptr::Position{0, 65535}});

    // Shape first, then position.
    auto both = show(pattern(4, 4));
    both.position.emplace(1, 2);
    const auto out = loud.encode(both);
    REQUIRE(out.size() == 2);
    CHECK(std::holds_alternative<ptr::NewPointer>(out[0]));
    CHECK(out[1] == proto::PointerUpdate{ptr::Position{1, 2}});
}

TEST_CASE("Hotspots are clamped; empty and broken shapes are handled")
{
    CursorEncoder encoder(config());
    auto out = encoder.encode(show(pattern(10, 10, 100, -5)));
    CHECK(shape_of(out.at(0)).hotspot_x == 9);
    CHECK(shape_of(out.at(0)).hotspot_y == 0);

    out = encoder.encode(show(CursorImage{}));
    REQUIRE(out.size() == 1);
    CHECK(shape_of(out[0]).width == 1);
    CHECK(drawn(out[0]) == Bytes(4));

    auto broken = pattern(4, 4);
    broken.pixels.pop_back();
    CHECK(encoder.encode(show(broken)).empty());  // ignored; the last shape stays
}

TEST_CASE("reset() restores the cursor with an empty client cache")
{
    auto c = config();
    c.send_positions = true;
    CursorEncoder encoder(c);
    static_cast<void>(encoder.encode(show(pattern(8, 8, 0, 0, 1))));
    static_cast<void>(encoder.encode(show(pattern(8, 8, 0, 0, 2))));
    static_cast<void>(encoder.encode(move(7, 8)));
    auto out = encoder.reset(c);
    REQUIRE(out.size() == 2);
    CHECK(shape_of(out[0]).cache_index == 0);
    CHECK(drawn(out[0]) == expected32(pattern(8, 8, 0, 0, 2)));
    CHECK(out[1] == proto::PointerUpdate{ptr::Position{7, 8}});

    static_cast<void>(encoder.encode(hide()));
    c.send_positions = false;
    CHECK(encoder.reset(c) == Updates{ptr::Hidden{}});
}

TEST_CASE("The negotiated encoder configuration")
{
    farland::server::Session session;
    session.pointer = {20, 21, caps::large_pointer_flags::size_96x96 | caps::large_pointer_flags::size_384x384};
    session.fastpath_output = true;
    auto c = CursorEncoder::Config::negotiated(session, 1000000);
    CHECK(c.pointer_cache_size == 21);
    CHECK(c.color_pointer_cache_size == 20);
    CHECK(c.max_size == 96);
    CHECK(c.large_pointers);
    CHECK(c.max_update_size == 1000000);
    CHECK_FALSE(c.send_positions);

    session.fastpath_output = false;  // Large Pointer Updates are fast-path only
    CHECK_FALSE(CursorEncoder::Config::negotiated(session, 16000).large_pointers);
    session.fastpath_output = true;
    session.pointer.pointer_cache_size = 0;  // and live in the pointer cache
    CHECK_FALSE(CursorEncoder::Config::negotiated(session, 16000).large_pointers);
    session.pointer.large_pointer_flags = 0;
    CHECK(CursorEncoder::Config::negotiated(session, 16000).max_size == 32);
}
