// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/hexdump.hpp>
#include <farland/base/writer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility>

using farland::to_hex;
using farland::Writer;

TEST_CASE("Writer encodes integers in both byte orders")
{
    Writer w;
    w.u8(0x01);
    w.u16le(0x0302);
    w.u16be(0x0405);
    w.u32le(0x09080706U);
    w.u32be(0x0a0b0c0dU);
    w.u64le(0x1514131211100f0eULL);
    CHECK(to_hex(w.view()) == "01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10 11 12 13 14 15");
}

TEST_CASE("Length fields can be patched after the content is written")
{
    Writer w;
    w.u8(0x03);
    w.u8(0x00);
    const auto length_pos = w.size();
    w.u16be(0);  // TPKT length, patched below
    w.zeros(3);
    w.patch_u16be(length_pos, static_cast<std::uint16_t>(w.size()));
    CHECK(to_hex(w.view()) == "03 00 00 07 00 00 00");

    w.patch_u16le(length_pos, 0x1234);
    w.patch_u8(0, 0xff);
    CHECK(to_hex(w.view()) == "ff 00 34 12 00 00 00");

    const auto bytes = std::move(w).take();
    CHECK(bytes.size() == 7);
}
