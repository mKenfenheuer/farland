// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/reader.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

using farland::Errc;
using farland::Reader;
using namespace farland::test::literals;

namespace {

farland::Result<std::uint32_t> sum_of_two_u16be(Reader& r)
{
    FARLAND_TRY(const auto a, r.u16be());
    FARLAND_TRY(const auto b, r.u16be());
    return std::uint32_t{a} + b;
}

}  // namespace

TEST_CASE("Reader decodes little- and big-endian integers")
{
    const auto data = "01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10 11 12 13 14 15 16 17 18 19"_hex;
    Reader r(data);
    CHECK(r.u8().value() == 0x01);
    CHECK(r.u16le().value() == 0x0302);
    CHECK(r.u16be().value() == 0x0405);
    CHECK(r.u32le().value() == 0x09080706U);
    CHECK(r.u32be().value() == 0x0a0b0c0dU);
    CHECK(r.u64le().value() == 0x1514131211100f0eULL);
    CHECK(r.remaining() == 4);
    CHECK(r.peek_u8().value() == 0x16);
    CHECK(r.remaining() == 4);
}

TEST_CASE("A failed read reports the offset and consumes nothing")
{
    const auto data = "aa bb cc"_hex;
    Reader r(data);
    REQUIRE(r.skip(2).has_value());

    const auto value = r.u16le();
    REQUIRE_FALSE(value.has_value());
    CHECK(value.error().code == Errc::truncated);
    CHECK(value.error().offset == 2);
    CHECK(r.position() == 2);

    CHECK(r.u8().value() == 0xcc);
    CHECK(r.empty());
    CHECK_FALSE(r.peek_u8().has_value());
}

TEST_CASE("Sub-readers are bounded and report absolute offsets")
{
    const auto data = "00 01 02 03 04 05"_hex;
    Reader r(data);
    REQUIRE(r.skip(1).has_value());

    auto sub = r.sub(3);
    REQUIRE(sub.has_value());
    CHECK(r.position() == 4);
    CHECK(sub->size() == 3);
    CHECK(sub->u16be().value() == 0x0102);
    CHECK(sub->offset() == 3);

    const auto past_end = sub->u16be();
    REQUIRE_FALSE(past_end.has_value());
    CHECK(past_end.error().offset == 3);

    const auto too_long = r.sub(3);
    REQUIRE_FALSE(too_long.has_value());
    CHECK(too_long.error().code == Errc::truncated);
}

TEST_CASE("expect_end rejects trailing bytes")
{
    const auto data = "01 02"_hex;
    Reader r(data);
    REQUIRE(r.u8().has_value());

    const auto result = r.expect_end("test structure");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == farland::Error{Errc::trailing_data, "test structure", 1});

    REQUIRE(r.u8().has_value());
    CHECK(r.expect_end("test structure").has_value());
}

TEST_CASE("FARLAND_TRY propagates the first error")
{
    const auto complete = "00 01 00 02"_hex;
    Reader ok(complete);
    CHECK(sum_of_two_u16be(ok).value() == 3);

    const auto truncated = "00 01 00"_hex;
    Reader short_input(truncated);
    const auto result = sum_of_two_u16be(short_input);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Errc::truncated);
    CHECK(result.error().offset == 2);
}
