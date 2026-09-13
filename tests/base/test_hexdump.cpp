// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/hexdump.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using farland::Errc;

TEST_CASE("to_hex and from_hex round-trip, ignoring whitespace")
{
    const auto bytes = farland::from_hex("03 00\n00 13\tAB");
    REQUIRE(bytes.has_value());
    CHECK(farland::to_hex(*bytes) == "03 00 00 13 ab");
    CHECK(farland::to_hex({}).empty());
}

TEST_CASE("from_hex reports the character position of errors")
{
    const auto bad_digit = farland::from_hex("00 0g");
    REQUIRE_FALSE(bad_digit.has_value());
    CHECK(bad_digit.error().code == Errc::invalid_value);
    CHECK(bad_digit.error().offset == 4);

    const auto odd = farland::from_hex("ab c");
    REQUIRE_FALSE(odd.has_value());
    CHECK(odd.error().code == Errc::invalid_length);
    CHECK(odd.error().offset == 3);
}

TEST_CASE("hexdump prints offsets, hex and ASCII")
{
    std::vector<std::byte> data;
    for (unsigned i = 0; i < 16; ++i) {
        data.push_back(static_cast<std::byte>(0x41 + i));
    }
    data.push_back(std::byte{0x00});

    const std::string expected = "00000100  41 42 43 44 45 46 47 48  49 4a 4b 4c 4d 4e 4f 50  |ABCDEFGHIJKLMNOP|\n"
                                 "00000110  00 " +
                                 std::string(7 * 3 + 1 + 8 * 3, ' ') + " |.|\n";
    CHECK(farland::hexdump(data, 0x100) == expected);
}
