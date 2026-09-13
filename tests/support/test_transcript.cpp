// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/reader.hpp>

#include "support/transcript.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <stdexcept>

using farland::test::Direction;
using farland::test::parse_transcript;

TEST_CASE("Transcript records, continuation lines and comments")
{
    const auto records = parse_transcript("# comment\n"
                                          "C> 01 02  # trailing comment\n"
                                          "   03\n"
                                          "\n"
                                          "S> ff\n");
    REQUIRE(records.size() == 2);
    CHECK(records[0].direction == Direction::client_to_server);
    CHECK(records[0].bytes == std::vector{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}});
    CHECK(records[0].line == 2);
    CHECK(records[1].direction == Direction::server_to_client);
    CHECK(records[1].bytes == std::vector{std::byte{0xff}});
}

TEST_CASE("Malformed transcripts report the line number")
{
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(parse_transcript("   01\n"), ContainsSubstring("line 1"));
    CHECK_THROWS_WITH(parse_transcript("C> 01\nX> 02\n"), ContainsSubstring("line 2"));
    CHECK_THROWS_WITH(parse_transcript("C> 0\n"), ContainsSubstring("odd number"));
}

TEST_CASE("The X.224 Connection Request sample transcript loads")
{
    const auto records =
        farland::test::load_transcript(FARLAND_TEST_DATA_DIR "/transcripts/x224-connection-request.txt");
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].direction == Direction::client_to_server);

    // TPKT header, [MS-RDPBCGR] 2.2.1.1 / RFC 1006: version 3, then the total length.
    farland::Reader r(records[0].bytes);
    CHECK(r.u8().value() == 3);
    CHECK(r.u8().value() == 0);
    CHECK(r.u16be().value() == records[0].bytes.size());
}
