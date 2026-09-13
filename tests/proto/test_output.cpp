// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Fast-path output framing and bitmap updates.

#include <farland/base/hexdump.hpp>
#include <farland/proto/bitmap.hpp>
#include <farland/proto/fastpath.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace proto = farland::proto;
namespace fastpath = farland::proto::fastpath;
using farland::Errc;
using farland::Reader;
using farland::to_hex;
using farland::Writer;
using farland::test::hex;

namespace {

/// Splits concatenated fast-path PDUs and reassembles their updates.
std::vector<fastpath::Reassembler::Update> reassemble(std::span<const std::byte> stream, std::size_t max_size)
{
    fastpath::Reassembler reassembler(max_size);
    std::vector<fastpath::Reassembler::Update> updates;
    Reader r(stream);
    while (!r.empty()) {
        const auto rest = r.rest();
        const std::size_t length =
            ((std::to_integer<std::size_t>(rest[1]) & 0x7FU) << 8U) | std::to_integer<std::size_t>(rest[2]);
        Reader pdu = r.sub(length).value();
        for (const auto& fragment : fastpath::decode_output_pdu(pdu).value()) {
            if (auto update = reassembler.add(fragment).value()) {
                updates.push_back(std::move(*update));
            }
        }
    }
    return updates;
}

}  // namespace

TEST_CASE("A small update is one fast-path PDU ([MS-RDPBCGR] 2.2.9.1.2)")
{
    const auto data = hex("aa bb cc");
    Writer w;
    fastpath::encode_update(w, fastpath::update_code::synchronize, data);
    CHECK(to_hex(w.view()) == "00 80 09 03 03 00 aa bb cc");
    Reader r(w.view());
    const auto updates = fastpath::decode_output_pdu(r).value();
    REQUIRE(updates.size() == 1);
    CHECK(updates[0].code == fastpath::update_code::synchronize);
    CHECK(updates[0].fragmentation == fastpath::Fragmentation::single);
    CHECK(to_hex(updates[0].data) == "aa bb cc");
}

TEST_CASE("Large updates are fragmented and reassemble")
{
    std::vector<std::byte> data(40000);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::byte>(i * 7);
    }
    Writer w;
    fastpath::encode_update(w, fastpath::update_code::bitmap, data);
    const auto updates = reassemble(w.view(), 65536);
    REQUIRE(updates.size() == 1);
    CHECK(updates[0].code == fastpath::update_code::bitmap);
    CHECK(updates[0].data == data);

    // The reassembler enforces the negotiated maximum.
    fastpath::Reassembler small(20000);
    Reader r(w.view());
    bool failed = false;
    while (!r.empty() && !failed) {
        const auto rest = r.rest();
        const std::size_t length =
            ((std::to_integer<std::size_t>(rest[1]) & 0x7FU) << 8U) | std::to_integer<std::size_t>(rest[2]);
        Reader pdu = r.sub(length).value();
        for (const auto& fragment : fastpath::decode_output_pdu(pdu).value()) {
            const auto result = small.add(fragment);
            if (!result) {
                CHECK(result.error().code == Errc::limit_exceeded);
                failed = true;
            }
        }
    }
    CHECK(failed);
}

TEST_CASE("Fragments out of order are rejected")
{
    fastpath::Reassembler reassembler(1000);
    const auto data = hex("01");
    CHECK_FALSE(reassembler.add({fastpath::update_code::bitmap, fastpath::Fragmentation::next, data}).has_value());
    CHECK(reassembler.add({fastpath::update_code::bitmap, fastpath::Fragmentation::first, data}).has_value());
    CHECK_FALSE(reassembler.add({fastpath::update_code::bitmap, fastpath::Fragmentation::single, data}).has_value());
}

TEST_CASE("Encrypted or compressed fast-path output is rejected")
{
    const auto encrypted = hex("80 06 01 00 00 00");
    Reader r1(encrypted);
    CHECK(fastpath::decode_output_pdu(r1).error().code == Errc::invalid_value);
    const auto compressed = hex("00 07 81 20 00 00 00");
    Reader r2(compressed);
    CHECK(fastpath::decode_output_pdu(r2).error().code == Errc::unsupported);
}

TEST_CASE("Bitmap updates round-trip with and without TS_CD_HEADER ([MS-RDPBCGR] 2.2.9.1.1.3.1.2)")
{
    const auto raw = hex("01 02 03 04 05 06 07 08");
    const auto planar = hex("20 aa bb");
    const std::vector<proto::BitmapData> rects{
        {0, 0, 1, 0, 2, 1, 32, 0, raw},
        {64, 0, 127, 63, 64, 64, 32, proto::bitmap_flags::compression | proto::bitmap_flags::no_bitmap_compression_hdr,
         planar},
        {0, 64, 63, 127, 64, 64, 32, proto::bitmap_flags::compression, planar},
    };
    Writer w;
    proto::encode_bitmap_update(w, rects);
    std::size_t expected_size = 4;
    for (const auto& rect : rects) {
        expected_size += proto::encoded_size(rect);
    }
    CHECK(w.size() == expected_size);

    Reader r(w.view());
    const auto decoded = proto::decode_bitmap_update(r).value();
    REQUIRE(decoded.size() == 3);
    CHECK(to_hex(decoded[0].data) == to_hex(raw));
    CHECK(decoded[1].dest_left == 64);
    CHECK(to_hex(decoded[1].data) == "20 aa bb");
    CHECK(to_hex(decoded[2].data) == "20 aa bb");
    CHECK(decoded[2].flags == proto::bitmap_flags::compression);
}

TEST_CASE("FreeRDP's bitmapLength convention (header not counted) is accepted")
{
    // One compressed rectangle, bitmapLength 3 although an 8-byte TS_CD_HEADER precedes the data.
    const auto bytes = hex("01 00 01 00  00 00 00 00 3f 00 3f 00 40 00 40 00 20 00 01 00 03 00"
                           "00 00 03 00 00 01 00 40  20 aa bb");
    Reader r(bytes);
    const auto decoded = proto::decode_bitmap_update(r).value();
    CHECK(to_hex(decoded.at(0).data) == "20 aa bb");
}
