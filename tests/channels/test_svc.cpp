// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/channels/svc.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <vector>

using farland::Errc;
using farland::Reader;
using farland::Writer;
using farland::test::hex;
namespace svc = farland::channels::svc;

namespace {

using Bytes = std::vector<std::byte>;

Bytes pattern(std::size_t size)
{
    Bytes bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>((i * 7U) & 0xFFU);
    }
    return bytes;
}

Bytes chunk(std::uint32_t length, std::uint32_t flags, const Bytes& data)
{
    Writer w;
    svc::encode(w, svc::ChunkHeader{length, flags});
    w.bytes(data);
    return std::move(w).take();
}

svc::ChunkHeader header_of(const Bytes& pdu)
{
    Reader r(pdu);
    return svc::decode_header(r).value();
}

constexpr std::uint32_t first_last = svc::flag::first | svc::flag::last;

}  // namespace

TEST_CASE("CHANNEL_PDU_HEADER round-trips ([MS-RDPBCGR] 2.2.6.1.1)")
{
    Writer w;
    svc::encode(w, svc::ChunkHeader{0x80E, svc::flag::first | svc::flag::show_protocol});
    CHECK(Bytes(w.view().begin(), w.view().end()) == hex("0e 08 00 00 11 00 00 00"));
    Reader r(w.view());
    CHECK(svc::decode_header(r).value() == svc::ChunkHeader{0x80E, 0x11});
    const auto short_bytes = hex("0e 08 00 00 11 00 00");
    Reader short_input(short_bytes);
    CHECK(svc::decode_header(short_input).error().code == Errc::truncated);
}

TEST_CASE("The chunk size follows both Virtual Channel Capability Sets ([MS-RDPBCGR] 3.1.5.2.1)")
{
    CHECK(svc::negotiated_chunk_length(std::nullopt, std::nullopt) == 1600);
    CHECK(svc::negotiated_chunk_length(8192, std::nullopt) == 1600);
    CHECK(svc::negotiated_chunk_length(std::nullopt, 8192) == 1600);
    CHECK(svc::negotiated_chunk_length(8192, 1600) == 8192);
    CHECK(svc::negotiated_chunk_length(100, 1600) == 1600);
    CHECK(svc::negotiated_chunk_length(100000, 1600) == 16256);
}

TEST_CASE("A message that fits one chunk carries FIRST and LAST")
{
    const auto chunks = svc::encode_chunks(hex("50 00 02 00"));
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0] == hex("04 00 00 00 03 00 00 00 50 00 02 00"));

    const auto empty = svc::encode_chunks({});
    REQUIRE(empty.size() == 1);
    CHECK(empty[0] == hex("00 00 00 00 03 00 00 00"));
}

TEST_CASE("The chunking example of [MS-RDPBCGR] 3.1.5.2.2.1 (2062 bytes, chunk size 1000)")
{
    const auto message = pattern(2062);
    const auto chunks = svc::encode_chunks(message, 1000);
    REQUIRE(chunks.size() == 3);
    CHECK(header_of(chunks[0]) == svc::ChunkHeader{2062, svc::flag::first | svc::flag::show_protocol});
    CHECK(header_of(chunks[1]) == svc::ChunkHeader{2062, svc::flag::show_protocol});
    CHECK(header_of(chunks[2]) == svc::ChunkHeader{2062, svc::flag::last | svc::flag::show_protocol});
    CHECK(chunks[0].size() == 8 + 1000);
    CHECK(chunks[1].size() == 8 + 1000);
    CHECK(chunks[2].size() == 8 + 62);

    svc::Reassembler reassembler(4096, 1000);
    CHECK_FALSE(reassembler.add(chunks[0]).value().has_value());
    CHECK(reassembler.in_progress());
    CHECK_FALSE(reassembler.add(chunks[1]).value().has_value());
    const auto done = reassembler.add(chunks[2]).value();
    REQUIRE(done.has_value());
    CHECK(*done == message);
    CHECK_FALSE(reassembler.in_progress());
}

TEST_CASE("Messages of exactly N chunks and of one byte more")
{
    for (const std::size_t size : {std::size_t{1600}, std::size_t{1601}, std::size_t{3200}, std::size_t{3201}}) {
        const auto message = pattern(size);
        const auto chunks = svc::encode_chunks(message);
        CHECK(chunks.size() == (size + 1599) / 1600);
        svc::Reassembler reassembler(size);
        std::optional<Bytes> result;
        for (const auto& c : chunks) {
            REQUIRE_FALSE(result.has_value());
            result = reassembler.add(c).value();
        }
        REQUIRE(result.has_value());
        CHECK(*result == message);
    }
}

TEST_CASE("Client-only flags and unused flags are ignored")
{
    svc::Reassembler reassembler(100);
    const auto flags =
        first_last | svc::flag::suspend | svc::flag::resume | svc::flag::shadow_persistent | svc::flag::show_protocol;
    CHECK(reassembler.add(chunk(2, flags, hex("aa bb"))).value() == hex("aa bb"));
}

TEST_CASE("Malformed static channel chunks are rejected")
{
    svc::Reassembler reassembler(100, 10);

    SECTION("compressed data (MPPC is never negotiated)")
    {
        CHECK(reassembler.add(chunk(2, first_last | svc::flag::packet_compressed, hex("aa bb"))).error().code ==
              Errc::unsupported);
    }
    SECTION("a header without data is fine, a short header is not")
    {
        CHECK(reassembler.add(hex("00 00 00 00 03 00 00")).error().code == Errc::truncated);
    }
    SECTION("single chunk with the wrong length")
    {
        CHECK(reassembler.add(chunk(3, first_last, hex("aa bb"))).error().code == Errc::invalid_length);
        CHECK(reassembler.add(chunk(1, first_last, hex("aa bb"))).error().code == Errc::invalid_length);
    }
    SECTION("first chunk that already holds the whole message")
    {
        CHECK(reassembler.add(chunk(2, svc::flag::first, hex("aa bb"))).error().code == Errc::invalid_length);
    }
    SECTION("middle or last chunk without a first one")
    {
        CHECK(reassembler.add(chunk(4, 0, hex("aa bb"))).error().code == Errc::invalid_value);
        CHECK(reassembler.add(chunk(4, svc::flag::last, hex("aa bb"))).error().code == Errc::invalid_value);
    }
    SECTION("first chunk inside a message")
    {
        REQUIRE(reassembler.add(chunk(4, svc::flag::first, hex("aa bb"))).has_value());
        CHECK(reassembler.add(chunk(4, svc::flag::first, hex("aa bb"))).error().code == Errc::invalid_value);
        CHECK_FALSE(reassembler.in_progress());
    }
    SECTION("length that changes between chunks")
    {
        REQUIRE(reassembler.add(chunk(4, svc::flag::first, hex("aa bb"))).has_value());
        CHECK(reassembler.add(chunk(5, svc::flag::last, hex("cc dd"))).error().code == Errc::invalid_length);
    }
    SECTION("chunks that overrun the length")
    {
        REQUIRE(reassembler.add(chunk(4, svc::flag::first, hex("aa bb"))).has_value());
        CHECK(reassembler.add(chunk(4, 0, hex("cc dd ee"))).error().code == Errc::invalid_length);
    }
    SECTION("last chunk that ends early")
    {
        REQUIRE(reassembler.add(chunk(4, svc::flag::first, hex("aa bb"))).has_value());
        CHECK(reassembler.add(chunk(4, svc::flag::last, hex("cc"))).error().code == Errc::invalid_length);
    }
    SECTION("limits")
    {
        CHECK(reassembler.add(chunk(101, svc::flag::first, hex("aa"))).error().code == Errc::limit_exceeded);
        CHECK(reassembler.add(chunk(11, first_last, Bytes(11))).error().code == Errc::limit_exceeded);
    }

    // The reassembler starts afresh after an error.
    CHECK(reassembler.add(chunk(1, first_last, hex("42"))).value() == hex("42"));
}

TEST_CASE("A middle chunk may complete the data before an empty last chunk")
{
    svc::Reassembler reassembler(100);
    REQUIRE(reassembler.add(chunk(4, svc::flag::first, hex("aa bb"))).has_value());
    CHECK_FALSE(reassembler.add(chunk(4, 0, hex("cc dd"))).value().has_value());
    CHECK(reassembler.add(chunk(4, svc::flag::last, {})).value() == hex("aa bb cc dd"));
}
