// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Pointer updates, [MS-RDPBCGR] 2.2.9.1.1.4 and 2.2.9.1.2.1.4 - 2.2.9.1.2.1.11.
// Section 4 has no pointer examples; the byte layouts below follow the
// structure diagrams, including the padding examples of 2.2.9.1.1.4.4.

#include <farland/base/hexdump.hpp>
#include <farland/proto/capabilities.hpp>
#include <farland/proto/fastpath.hpp>
#include <farland/proto/pointer.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace ptr = farland::proto::pointer;
namespace caps = farland::proto::caps;
namespace code = farland::proto::fastpath::update_code;
using farland::Errc;
using farland::Reader;
using farland::to_hex;
using farland::Writer;
using farland::test::hex;

namespace {

using Bytes = std::vector<std::byte>;

/// Straight-alpha BGRA pixels from 0xAARRGGBB values.
Bytes bgra(std::initializer_list<std::uint32_t> argb)
{
    Bytes out;
    for (const std::uint32_t p : argb) {
        out.push_back(std::byte{static_cast<std::uint8_t>(p)});
        out.push_back(std::byte{static_cast<std::uint8_t>(p >> 8U)});
        out.push_back(std::byte{static_cast<std::uint8_t>(p >> 16U)});
        out.push_back(std::byte{static_cast<std::uint8_t>(p >> 24U)});
    }
    return out;
}

/// Encodes on fast-path, checks the code, the size and the round trip.
std::string fastpath_hex(const ptr::Update& update, std::uint8_t expected_code)
{
    Writer w;
    CHECK(ptr::encode_fastpath(w, update) == expected_code);
    CHECK(w.size() == ptr::fastpath_size(update));
    Reader r(w.view());
    const auto decoded = ptr::decode_fastpath(expected_code, r);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == update);
    return to_hex(w.view());
}

std::string slow_path_hex(const ptr::Update& update)
{
    Writer w;
    ptr::encode_slow_path(w, update);
    Reader r(w.view());
    const auto decoded = ptr::decode_slow_path(r);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == update);
    return to_hex(w.view());
}

farland::Result<ptr::Update> decode_fp(std::uint8_t update_code, std::string_view text)
{
    const auto bytes = hex(text);
    Reader r(bytes);
    return ptr::decode_fastpath(update_code, r);
}

farland::Result<ptr::Update> decode_slow(std::string_view text)
{
    const auto bytes = hex(text);
    Reader r(bytes);
    return ptr::decode_slow_path(r);
}

}  // namespace

TEST_CASE("System pointer, position and cached pointer updates on both paths")
{
    CHECK(fastpath_hex(ptr::Hidden{}, code::pointer_hidden).empty());  // size MUST be zero
    CHECK(fastpath_hex(ptr::Default{}, code::pointer_default).empty());
    CHECK(fastpath_hex(ptr::Position{0x0102, 0x0304}, code::pointer_position) == "02 01 04 03");
    CHECK(fastpath_hex(ptr::CachedPointer{7}, code::cached_pointer) == "07 00");

    // messageType, pad2Octets, then the attribute (2.2.9.1.1.4).
    CHECK(slow_path_hex(ptr::Hidden{}) == "01 00 00 00 00 00 00 00");
    CHECK(slow_path_hex(ptr::Default{}) == "01 00 00 00 00 7f 00 00");
    CHECK(slow_path_hex(ptr::Position{0x0102, 0x0304}) == "03 00 00 00 02 01 04 03");
    CHECK(slow_path_hex(ptr::CachedPointer{7}) == "07 00 00 00 07 00");
}

TEST_CASE("Scan lines are padded to two bytes (2.2.9.1.1.4.4 examples)")
{
    CHECK(ptr::xor_stride(3, 24) == 10);  // "a 3x3 pixel cursor ... 10 bytes"
    CHECK(ptr::and_stride(7) == 2);       // "a 7x7 pixel cursor ... 2 bytes"
    CHECK(ptr::and_stride(17) == 4);
    CHECK(ptr::xor_stride(3, 32) == 12);
    CHECK(ptr::xor_stride(3, 1) == 2);
    CHECK(ptr::xor_stride(5, 16) == 10);
    CHECK(ptr::xor_stride(96, 32) == 384);
}

TEST_CASE("A 24 bpp Color Pointer: bottom-up masks, 1-bit transparency")
{
    // Top-down: red, green, blue / white, black, clear / clear, clear, alpha 0x7f.
    const auto pixels = bgra(
        {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF, 0xFF000000, 0x00000000, 0x00000000, 0x00000000, 0x7F123456});
    ptr::Shape shape = ptr::shape_from_bgra(pixels, 3, 3, 24);
    shape.cache_index = 2;
    shape.hotspot_x = 1;
    shape.hotspot_y = 1;
    const std::string attribute = "02 00 01 00 01 00 03 00 03 00 06 00 1e 00 "
                                  // XOR, bottom row first, each line padded to 10 bytes
                                  "00 00 00 00 00 00 00 00 00 00 "
                                  "ff ff ff 00 00 00 00 00 00 00 "
                                  "00 00 ff 00 ff 00 ff 00 00 00 "
                                  // AND, bottom row first
                                  "e0 00 20 00 00 00 "
                                  // pad
                                  "00";
    const ptr::Update update = ptr::ColorPointer{shape};
    CHECK(fastpath_hex(update, code::color_pointer) == attribute);
    CHECK(slow_path_hex(update) == "06 00 00 00 " + attribute);

    CHECK(ptr::shape_to_bgra(shape).value() ==
          bgra({0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF, 0xFF000000, 0, 0, 0, 0}));
}

TEST_CASE("A 32 bpp New Pointer keeps alpha; clear pixels set the AND bit")
{
    // Top-down: half-transparent red, clear (with color bits) / opaque black, opaque white.
    const auto pixels = bgra({0x80FF0000, 0x00563412, 0xFF000000, 0xFFFFFFFF});
    ptr::Shape shape = ptr::shape_from_bgra(pixels, 2, 2, 32);
    shape.hotspot_x = 1;
    const std::string attribute = "20 00 00 00 01 00 00 00 02 00 02 00 04 00 10 00 "
                                  "00 00 00 ff ff ff ff ff "
                                  "00 00 ff 80 00 00 00 00 "
                                  "00 00 40 00 "
                                  "00";
    const ptr::Update update = ptr::NewPointer{shape};
    CHECK(fastpath_hex(update, code::new_pointer) == attribute);
    CHECK(slow_path_hex(update) == "08 00 00 00 " + attribute);

    // The client draws the input, with the clear pixel's color bits dropped.
    CHECK(ptr::shape_to_bgra(shape).value() == bgra({0x80FF0000, 0, 0xFF000000, 0xFFFFFFFF}));
}

TEST_CASE("A 384x384 Large Pointer round-trips; larger ones are rejected")
{
    constexpr std::uint16_t side = ptr::max_large_size;
    Bytes pixels;
    Bytes expected;
    for (std::uint32_t y = 0; y < side; ++y) {
        for (std::uint32_t x = 0; x < side; ++x) {
            const auto a = static_cast<std::uint8_t>((x + y) % 5 == 0 ? 0 : 1 + ((x * 7 + y) % 255));
            const Bytes p{std::byte{static_cast<std::uint8_t>(x)}, std::byte{static_cast<std::uint8_t>(y)},
                          std::byte{static_cast<std::uint8_t>(x ^ y)}, std::byte{a}};
            pixels.insert(pixels.end(), p.begin(), p.end());
            if (a == 0) {
                expected.insert(expected.end(), 4, std::byte{0});
            } else {
                expected.insert(expected.end(), p.begin(), p.end());
            }
        }
    }
    ptr::Shape shape = ptr::shape_from_bgra(pixels, side, side, 32);
    shape.cache_index = 5;
    shape.hotspot_x = 7;
    shape.hotspot_y = 8;
    const ptr::Update update = ptr::LargePointer{shape};
    Writer w;
    CHECK(ptr::encode_fastpath(w, update) == code::large_pointer);
    CHECK(w.size() == 20 + (384U * 1536U) + (48U * 384U) + 1);
    CHECK(to_hex(w.view().first(20)) == "20 00 05 00 07 00 08 00 80 01 80 01 00 48 00 00 00 00 09 00");
    Reader r(w.view());
    const auto decoded = ptr::decode_fastpath(code::large_pointer, r).value();
    CHECK(decoded == update);
    CHECK(ptr::shape_to_bgra(std::get<ptr::LargePointer>(decoded).shape).value() == expected);

    CHECK(decode_fp(code::large_pointer, "20 00 00 00 00 00 00 00 81 01 01 00 00 00 00 00 00 00 00 00").error().code ==
          Errc::limit_exceeded);
    // Lengths are checked against the size before anything is read or allocated.
    CHECK(decode_fp(code::large_pointer, "20 00 00 00 00 00 00 00 80 01 80 01 00 48 00 00 ff ff ff ff").error().code ==
          Errc::invalid_length);
}

TEST_CASE("Pointer decoders reject malformed updates")
{
    const std::string one_pixel_header = "20 00 00 00 00 00 00 00 01 00 01 00 02 00 04 00 ";
    const std::string one_pixel = one_pixel_header + "11 22 33 44 80 00";
    CHECK(decode_fp(code::new_pointer, one_pixel).has_value());
    CHECK(decode_fp(code::new_pointer, one_pixel + " 00").has_value());  // pad
    CHECK(decode_fp(code::new_pointer, one_pixel + " 00 00").error().code == Errc::trailing_data);
    CHECK(decode_fp(code::new_pointer, one_pixel_header + "11 22 33").error().code == Errc::truncated);
    CHECK(decode_fp(code::new_pointer, "20 00 00 00 00 00 00 00 01 00 01 00 02 00 06 00 00 00 00 00 00 00 00 00")
              .error()
              .code == Errc::invalid_length);
    CHECK(decode_fp(code::new_pointer, "20 00 00 00 00 00 00 00 01 00 01 00 04 00 04 00 00 00 00 00 00 00 00 00")
              .error()
              .code == Errc::invalid_length);
    // 97 pixels wide: more than New Pointer Updates allow.
    CHECK(decode_fp(code::new_pointer, "20 00 00 00 00 00 00 00 61 00 01 00 0e 00 84 01").error().code ==
          Errc::limit_exceeded);
    CHECK(decode_fp(code::new_pointer, "20 00 00 00 00 00 00 00 00 00 01 00 00 00 00 00").error().code ==
          Errc::invalid_value);
    CHECK(decode_fp(code::new_pointer, "07 00 00 00 00 00 00 00 01 00 01 00 02 00 02 00 00 00 00 00").error().code ==
          Errc::invalid_value);
    CHECK(decode_fp(code::color_pointer, "00 00 00 00 00 00 61 00 01 00 0e 00 24 01").error().code ==
          Errc::limit_exceeded);
    CHECK(decode_fp(code::pointer_hidden, "00").error().code == Errc::trailing_data);
    CHECK(decode_fp(code::pointer_position, "01 00 02").error().code == Errc::truncated);
    CHECK(decode_fp(code::cached_pointer, "01 00 02").error().code == Errc::trailing_data);
    CHECK(decode_fp(code::bitmap, "").error().code == Errc::invalid_value);

    CHECK(decode_slow("01 00 00 00 05 00 00 00").error().code == Errc::invalid_value);
    // FreeRDP's private slow-path large pointer (messageType 9) is not RDP.
    CHECK(decode_slow("09 00 00 00").error().code == Errc::invalid_value);
    CHECK(decode_slow("01 00").error().code == Errc::truncated);

    // A hotspot outside the image is kept as sent.
    const auto odd = decode_fp(code::new_pointer, "20 00 00 00 05 00 06 00 01 00 01 00 02 00 04 00 00 00 00 00 00 00");
    REQUIRE(odd.has_value());
    CHECK(std::get<ptr::NewPointer>(*odd).shape.hotspot_x == 5);
    CHECK(std::get<ptr::NewPointer>(*odd).shape.hotspot_y == 6);
}

TEST_CASE("Other depths render the way FreeRDP draws them")
{
    // 1 bpp (read top-down, as FreeRDP does): black, white, inverted, clear.
    const ptr::Shape mono{1, 0, 0, 0, 4, 1, hex("60 00"), hex("30 00")};
    CHECK(ptr::shape_to_bgra(mono).value() == bgra({0xFF000000, 0xFFFFFFFF, 0xFFFFFFFF, 0}));

    // 16 bpp is RGB555.
    const ptr::Shape rgb555{16, 0, 0, 0, 2, 1, hex("00 7c e0 03"), hex("00 00")};
    CHECK(ptr::shape_to_bgra(rgb555).value() == bgra({0xFFFF0000, 0xFF00FF00}));

    // 32 bpp with AND bits: black becomes clear, white inverts, others stay.
    const ptr::Shape classic{32, 0, 0, 0, 3, 1, hex("00 00 00 ff ff ff ff ff 00 00 ff ff"), hex("e0 00")};
    CHECK(ptr::shape_to_bgra(classic).value() == bgra({0, 0xFF000000, 0xFFFF0000}));

    const ptr::Shape palette{8, 0, 0, 0, 1, 1, hex("05 00"), hex("00 00")};
    CHECK(ptr::shape_to_bgra(palette).error().code == Errc::unsupported);
}

TEST_CASE("Pointer and Large Pointer capability sets (2.2.7.1.5, 2.2.7.2.7)")
{
    // Old clients leave pointerCacheSize out: no New Pointer Updates for them.
    const auto bytes = hex("08 00 08 00 01 00 14 00 1b 00 06 00 03 00");
    Reader r(bytes);
    const auto sets = caps::decode_capability_sets(r, 2).value();
    REQUIRE(sets.pointer.has_value());
    CHECK(sets.pointer->color_pointer_cache_size == 20);
    CHECK_FALSE(sets.pointer->pointer_cache_size.has_value());
    REQUIRE(sets.large_pointer.has_value());
    CHECK(sets.large_pointer->support_flags ==
          (caps::large_pointer_flags::size_96x96 | caps::large_pointer_flags::size_384x384));
    Writer w;
    CHECK(caps::encode_capability_sets(w, sets) == 2);
    CHECK(to_hex(w.view()) == to_hex(bytes));
}
