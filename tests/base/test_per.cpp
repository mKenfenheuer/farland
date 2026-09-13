// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/hexdump.hpp>
#include <farland/base/per.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

namespace per = farland::per;
using farland::Errc;
using farland::Reader;
using farland::to_hex;
using farland::Writer;
using farland::test::hex;

TEST_CASE("PER length determinants (X.691 10.9)")
{
    const auto bytes = hex("05 81 2a bf ff c0 00");
    Reader r(bytes);
    CHECK(per::read_length(r).value() == 5);
    CHECK(per::read_length(r).value() == 0x12a);
    CHECK(per::read_length(r).value() == 0x3fff);
    CHECK(per::read_length(r).error().code == Errc::unsupported);

    Writer w;
    per::write_length(w, 0x7f);
    per::write_length(w, 0x80);
    per::write_length(w, per::max_length);
    CHECK(to_hex(w.view()) == "7f 80 80 bf ff");
}

TEST_CASE("PER integers")
{
    const auto bytes = hex("01 05 02 01 00 04 00 01 00 00 03 00 00 00");
    Reader r(bytes);
    CHECK(per::read_integer(r).value() == 5);
    CHECK(per::read_integer(r).value() == 256);
    CHECK(per::read_integer(r).value() == 65536);
    CHECK(per::read_integer(r).error().code == Errc::invalid_length);

    Writer w;
    per::write_integer(w, 5);
    per::write_integer(w, 256);
    per::write_integer(w, 65536);
    CHECK(to_hex(w.view()) == "01 05 02 01 00 04 00 01 00 00");
}

TEST_CASE("PER integer16 with a lower bound: MCS channel IDs start at 1001")
{
    const auto bytes = hex("00 02 ff ff");
    Reader r(bytes);
    CHECK(per::read_integer16(r, 1001).value() == 1003);
    CHECK(per::read_integer16(r, 1001).error().code == Errc::invalid_value);

    Writer w;
    per::write_integer16(w, 1003, 1001);
    CHECK(to_hex(w.view()) == "00 02");
}

TEST_CASE("PER enumerated values are range-checked")
{
    const auto bytes = hex("0f 10");
    Reader r(bytes);
    CHECK(per::read_enumerated(r, 16).value() == 15);
    CHECK(per::read_enumerated(r, 16).error().code == Errc::invalid_value);
}

TEST_CASE("PER object identifiers")
{
    const auto bytes = hex("05 00 14 7c 00 01 06 00 14 7c 00 01 00 05 00 14 7c 80 01");
    Reader r(bytes);
    CHECK(per::read_object_identifier(r).value() == per::t124_02_98_oid);
    CHECK(per::read_object_identifier(r).error().code == Errc::unsupported);  // six octets

    Reader multi_octet_arc{std::span(bytes).subspan(13)};
    CHECK(per::read_object_identifier(multi_octet_arc).error().code == Errc::unsupported);

    Writer w;
    per::write_object_identifier(w, per::t124_02_98_oid);
    CHECK(to_hex(w.view()) == "05 00 14 7c 00 01");
}

TEST_CASE("GCC Conference Create Request header ([MS-RDPBCGR] 4.1.3)")
{
    // From the annotated Client MCS Connect Initial PDU: the start of the
    // T.124 ConnectData inside the MCS userData, up to the client data length.
    constexpr std::string_view wire = "00 05 00 14 7c 00 01 81 2a 00 08 00 10 00 01 c0 00 44 75 63 61 81 1c";
    const auto bytes = hex(wire);
    Reader r(bytes);
    CHECK(per::read_choice(r).value() == 0);  // ConnectData::Key: object
    CHECK(per::read_object_identifier(r).value() == per::t124_02_98_oid);
    CHECK(per::read_length(r).value() == 0x12a);                           // ConnectData::connectPDU length
    CHECK(per::read_choice(r).value() == 0);                               // ConnectGCCPDU: conferenceCreateRequest
    CHECK(per::read_selection(r).value() == 0x08);                         // optional userData present
    CHECK(to_hex(per::read_numeric_string(r, 1).value()) == "10");         // conferenceName "1"
    CHECK(per::read_padding(r, 1).has_value());                            // termination method
    CHECK(per::read_number_of_sets(r).value() == 1);                       // one UserData set
    CHECK(per::read_choice(r).value() == 0xc0);                            // value present, h221NonStandard key
    CHECK(to_hex(per::read_octet_string(r, 4).value()) == "44 75 63 61");  // "Duca"
    CHECK(per::read_length(r).value() == 0x11c);                           // client data blocks length
    CHECK(r.empty());

    Writer w;
    per::write_choice(w, 0);
    per::write_object_identifier(w, per::t124_02_98_oid);
    per::write_length(w, 0x12a);
    per::write_choice(w, 0);
    per::write_selection(w, 0x08);
    per::write_numeric_string(w, "1", 1);
    per::write_padding(w, 1);
    per::write_number_of_sets(w, 1);
    per::write_choice(w, 0xc0);
    per::write_octet_string(w, farland::test::ascii("Duca"), 4);
    per::write_length(w, 0x11c);
    CHECK(to_hex(w.view()) == wire);
}

TEST_CASE("PER octet and numeric strings are bounded by the input")
{
    const auto bytes = hex("05 01 02");
    Reader r(bytes);
    CHECK(per::read_octet_string(r, 4).error().code == Errc::truncated);

    Writer w;
    per::write_numeric_string(w, "12345", 1);
    CHECK(to_hex(w.view()) == "04 12 34 50");
}
