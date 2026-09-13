// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/proto/framing.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

using farland::Errc;
using farland::proto::FrameKind;
using farland::proto::peek_frame;
using farland::test::hex;

TEST_CASE("TPKT frames report the TPKT length")
{
    const auto frame = peek_frame(hex("03 00 00 13 0e e0")).value();
    REQUIRE(frame.has_value());
    CHECK(frame->kind == FrameKind::tpkt);
    CHECK(frame->length == 0x13);
}

TEST_CASE("Fast-path frames use a one- or two-octet length ([MS-RDPBCGR] 2.2.8.1.2)")
{
    const auto one = peek_frame(hex("44 09 01 02")).value();
    REQUIRE(one.has_value());
    CHECK(one->kind == FrameKind::fastpath);
    CHECK(one->length == 9);

    const auto two = peek_frame(hex("00 81 2c")).value();
    REQUIRE(two.has_value());
    CHECK(two->length == 0x12c);
}

TEST_CASE("Frames wait for enough bytes to know the length")
{
    CHECK_FALSE(peek_frame({}).value().has_value());
    CHECK_FALSE(peek_frame(hex("03 00 00")).value().has_value());
    CHECK_FALSE(peek_frame(hex("00")).value().has_value());
    CHECK_FALSE(peek_frame(hex("00 81")).value().has_value());
}

TEST_CASE("Malformed frame headers are rejected")
{
    CHECK(peek_frame(hex("03 00 00 06")).error().code == Errc::invalid_length);
    CHECK(peek_frame(hex("00 01")).error().code == Errc::invalid_length);
    CHECK(peek_frame(hex("00 80 01")).error().code == Errc::invalid_length);
    CHECK(peek_frame(hex("01 05")).error().code == Errc::invalid_value);
}
