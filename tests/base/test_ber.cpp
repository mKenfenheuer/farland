// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/ber.hpp>
#include <farland/base/hexdump.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <string>
#include <string_view>

namespace ber = farland::ber;
using ber::Rules;
using farland::Errc;
using farland::Reader;
using farland::to_hex;
using farland::Writer;
using farland::test::hex;

namespace {

std::string encode_integer(std::int64_t value)
{
    Writer w;
    ber::write_integer(w, value);
    return to_hex(w.view());
}

std::string encode_unsigned(std::uint64_t value)
{
    Writer w;
    ber::write_unsigned(w, value);
    return to_hex(w.view());
}

}  // namespace

TEST_CASE("INTEGER is minimally encoded and round-trips (X.690 8.3)")
{
    struct Case {
        std::int64_t value;
        std::string_view der;
    };
    const std::array cases{
        Case{0, "02 01 00"},
        Case{127, "02 01 7f"},
        Case{128, "02 02 00 80"},
        Case{256, "02 02 01 00"},
        Case{-1, "02 01 ff"},
        Case{-128, "02 01 80"},
        Case{-129, "02 02 ff 7f"},
        Case{std::numeric_limits<std::int64_t>::min(), "02 08 80 00 00 00 00 00 00 00"},
        Case{std::numeric_limits<std::int64_t>::max(), "02 08 7f ff ff ff ff ff ff ff"},
    };
    for (const auto& c : cases) {
        INFO("value " << c.value);
        CHECK(encode_integer(c.value) == c.der);
        const auto bytes = hex(c.der);
        Reader r(bytes);
        CHECK(ber::read_integer(r, Rules::der).value() == c.value);
        CHECK(r.empty());
    }
}

TEST_CASE("Unsigned INTEGER keeps a sign octet when the top bit is set")
{
    struct Case {
        std::uint64_t value;
        std::string_view der;
    };
    const std::array cases{
        Case{0, "02 01 00"},
        Case{6, "02 01 06"},
        Case{0xFFFFFFFFU, "02 05 00 ff ff ff ff"},
        Case{std::numeric_limits<std::uint64_t>::max(), "02 09 00 ff ff ff ff ff ff ff ff"},
    };
    for (const auto& c : cases) {
        INFO("value " << c.value);
        CHECK(encode_unsigned(c.value) == c.der);
        const auto bytes = hex(c.der);
        Reader r(bytes);
        CHECK(ber::read_unsigned(r, Rules::der).value() == c.value);
    }

    const auto negative = hex("02 01 ff");
    Reader r(negative);
    CHECK(ber::read_unsigned(r, Rules::ber).error().code == Errc::invalid_value);

    const auto too_large = hex("02 09 01 00 00 00 00 00 00 00 00");
    Reader big(too_large);
    CHECK(ber::read_integer(big, Rules::ber).error().code == Errc::limit_exceeded);
}

TEST_CASE("DER rejects non-canonical encodings that BER accepts")
{
    const auto check = [](std::string_view input, auto&& read) {
        const auto bytes = hex(input);
        Reader der(bytes);
        Reader lax(bytes);
        const auto strict = read(der, Rules::der);
        REQUIRE_FALSE(strict.has_value());
        CHECK(strict.error().offset <= 2);
        CHECK(read(lax, Rules::ber).has_value());
    };
    check("02 02 00 7f", [](Reader& r, Rules rules) { return ber::read_integer(r, rules); });
    check("04 81 01 aa", [](Reader& r, Rules rules) { return ber::read_octet_string(r, rules); });
    check("82 00 81", [](Reader& r, Rules rules) { return ber::read_length(r, rules); });
    check("01 01 01", [](Reader& r, Rules rules) { return ber::read_boolean(r, rules); });

    const auto lax_true = hex("01 01 01");
    Reader r(lax_true);
    CHECK(ber::read_boolean(r, Rules::ber).value());
}

TEST_CASE("Malformed TLVs are rejected with the right category")
{
    const auto check = [](std::string_view input, Errc expected) {
        INFO("input " << input);
        const auto bytes = hex(input);
        Reader r(bytes);
        const auto tlv = ber::read_tlv(r, Rules::ber);
        REQUIRE_FALSE(tlv.has_value());
        CHECK(tlv.error().code == expected);
    };
    check("", Errc::truncated);
    check("30 80 00 00", Errc::unsupported);              // indefinite length
    check("04 85 00 00 00 00 01", Errc::limit_exceeded);  // five length octets
    check("04 05 01 02", Errc::truncated);                // content beyond the input
    check("1f 1e 00", Errc::invalid_value);               // tag 30 in high-tag-number form
    check("1f 80 65 00", Errc::invalid_value);            // leading zero in the tag number
    check("1f ff ff ff 7f 00", Errc::limit_exceeded);     // tag number over 21 bits

    const auto wrong_tag = hex("04 01 00");
    Reader r(wrong_tag);
    const auto tlv = ber::expect_tlv(r, ber::tags::integer, Rules::der);
    REQUIRE_FALSE(tlv.has_value());
    CHECK(tlv.error().code == Errc::invalid_value);
    CHECK(tlv.error().offset == 0);
}

TEST_CASE("High tag numbers: MCS Connect-Initial is [APPLICATION 101]")
{
    Writer w;
    ber::write_tag(w, ber::application(101));
    CHECK(to_hex(w.view()) == "7f 65");

    Writer big;
    ber::write_tag(big, ber::context(0x4000, false));
    CHECK(to_hex(big.view()) == "9f 81 80 00");

    const auto bytes = hex("7f 65 00 9f 81 80 00 00");
    Reader r(bytes);
    CHECK(ber::peek_tag(r, Rules::der).value() == ber::application(101));
    CHECK(r.position() == 0);
    CHECK(ber::read_tlv(r, Rules::der).value().tag == ber::application(101));
    CHECK(ber::read_tlv(r, Rules::der).value().tag == ber::context(0x4000, false));
    CHECK(r.empty());
}

TEST_CASE("Long-form lengths")
{
    Writer w;
    ber::write_length(w, 0x7f);
    ber::write_length(w, 0x80);
    ber::write_length(w, 0x1234);
    ber::write_length(w, 0x10000);
    CHECK(to_hex(w.view()) == "7f 81 80 82 12 34 83 01 00 00");

    Reader r(w.view());
    CHECK(ber::read_length(r, Rules::der).value() == 0x7f);
    CHECK(ber::read_length(r, Rules::der).value() == 0x80);
    CHECK(ber::read_length(r, Rules::der).value() == 0x1234);
    CHECK(ber::read_length(r, Rules::der).value() == 0x10000);
    CHECK(r.empty());
}

TEST_CASE("Nested constructed values: TSRequest version ([MS-CSSP] 2.2.1)")
{
    Writer w;
    ber::write_constructed(w, ber::tags::sequence, [](Writer& request) {
        ber::write_constructed(request, ber::context(0), [](Writer& version) { ber::write_integer(version, 6); });
    });
    CHECK(to_hex(w.view()) == "30 05 a0 03 02 01 06");

    Reader r(w.view());
    auto request = ber::read_constructed(r, ber::tags::sequence, Rules::der);
    REQUIRE(request.has_value());
    auto version = ber::read_constructed(*request, ber::context(0), Rules::der);
    REQUIRE(version.has_value());
    CHECK(ber::read_integer(*version, Rules::der).value() == 6);
    CHECK(version->expect_end("version").has_value());
    CHECK(request->expect_end("TSRequest").has_value());
    CHECK(r.empty());
}

TEST_CASE("Nested constructed values: MCS Connect-Initial prefix ([MS-RDPBCGR] 2.2.1.3)")
{
    constexpr std::string_view wire = "7f 65 09 04 01 01 04 01 01 01 01 ff";
    const auto bytes = hex(wire);
    Reader r(bytes);
    auto body = ber::read_constructed(r, ber::application(101), Rules::ber);
    REQUIRE(body.has_value());
    CHECK(r.empty());
    CHECK(to_hex(ber::read_octet_string(*body, Rules::ber).value()) == "01");  // callingDomainSelector
    CHECK(to_hex(ber::read_octet_string(*body, Rules::ber).value()) == "01");  // calledDomainSelector
    CHECK(ber::read_boolean(*body, Rules::ber).value());                       // upwardFlag
    CHECK(body->expect_end("Connect-Initial").has_value());

    Writer w;
    ber::write_constructed(w, ber::application(101), [](Writer& initial) {
        const std::array selector{std::byte{0x01}};
        ber::write_octet_string(initial, selector);
        ber::write_octet_string(initial, selector);
        ber::write_boolean(initial, true);
    });
    CHECK(to_hex(w.view()) == wire);
}
