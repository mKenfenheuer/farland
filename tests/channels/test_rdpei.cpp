// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/channels/rdpei.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <functional>
#include <variant>
#include <vector>

using farland::Errc;
using farland::Reader;
using farland::Writer;
using farland::test::hex;
namespace rdpei = farland::channels::rdpei;

namespace {

using Bytes = std::vector<std::byte>;

template <class T>
Bytes encoded(void (*write)(Writer&, T), T value)
{
    Writer w;
    write(w, value);
    return std::move(w).take();
}

template <class T>
T decoded(farland::Result<T> (*read)(Reader&), const Bytes& bytes)
{
    Reader r(bytes);
    const auto value = read(r);
    REQUIRE(value.has_value());
    CHECK(r.empty());
    return *value;
}

template <class T>
Errc decode_error(farland::Result<T> (*read)(Reader&), const Bytes& bytes)
{
    Reader r(bytes);
    const auto value = read(r);
    REQUIRE_FALSE(value.has_value());
    return value.error().code;
}

rdpei::ClientPdu client(const Bytes& bytes)
{
    const auto pdu = rdpei::decode_client_pdu(bytes);
    INFO((pdu.has_value() ? "" : pdu.error().message()));
    REQUIRE(pdu.has_value());
    return *pdu;
}

Errc client_error(const Bytes& bytes)
{
    const auto pdu = rdpei::decode_client_pdu(bytes);
    REQUIRE_FALSE(pdu.has_value());
    return pdu.error().code;
}

}  // namespace

TEST_CASE("RDPEI variable-length integers: the examples of [MS-RDPEI] 2.2.2")
{
    CHECK(encoded(rdpei::write_two_byte_unsigned, std::uint16_t{0x1A1B}) == hex("9A 1B"));
    CHECK(encoded(rdpei::write_two_byte_signed, std::int16_t{-0x1A1B}) == hex("DA 1B"));
    CHECK(encoded(rdpei::write_two_byte_signed, std::int16_t{-2}) == hex("42"));
    CHECK(encoded(rdpei::write_four_byte_unsigned, std::uint32_t{0x001A1B1C}) == hex("9A 1B 1C"));
    CHECK(encoded(rdpei::write_four_byte_signed, std::int32_t{-0x001A1B1C}) == hex("BA 1B 1C"));
    CHECK(encoded(rdpei::write_four_byte_signed, std::int32_t{-2}) == hex("22"));
    CHECK(encoded(rdpei::write_eight_byte_unsigned, std::uint64_t{0x001A1B1C1D1E1F2A}) == hex("DA 1B 1C 1D 1E 1F 2A"));

    CHECK(decoded(rdpei::read_two_byte_unsigned, hex("9A 1B")) == 0x1A1B);
    CHECK(decoded(rdpei::read_two_byte_signed, hex("DA 1B")) == -0x1A1B);
    CHECK(decoded(rdpei::read_two_byte_signed, hex("42")) == -2);
    CHECK(decoded(rdpei::read_four_byte_unsigned, hex("9A 1B 1C")) == 0x001A1B1C);
    CHECK(decoded(rdpei::read_four_byte_signed, hex("BA 1B 1C")) == -0x001A1B1C);
    CHECK(decoded(rdpei::read_four_byte_signed, hex("22")) == -2);
    CHECK(decoded(rdpei::read_eight_byte_unsigned, hex("DA 1B 1C 1D 1E 1F 2A")) == 0x001A1B1C1D1E1F2A);
}

TEST_CASE("RDPEI variable-length integers at the edges of each size")
{
    CHECK(encoded(rdpei::write_two_byte_unsigned, std::uint16_t{0x7F}) == hex("7F"));
    CHECK(encoded(rdpei::write_two_byte_unsigned, std::uint16_t{0x80}) == hex("80 80"));
    CHECK(encoded(rdpei::write_two_byte_unsigned, rdpei::max_two_byte_unsigned) == hex("FF FF"));

    CHECK(encoded(rdpei::write_two_byte_signed, std::int16_t{0x3F}) == hex("3F"));
    CHECK(encoded(rdpei::write_two_byte_signed, std::int16_t{0x40}) == hex("80 40"));
    CHECK(encoded(rdpei::write_two_byte_signed, rdpei::max_two_byte_signed) == hex("BF FF"));
    CHECK(encoded(rdpei::write_two_byte_signed, static_cast<std::int16_t>(-rdpei::max_two_byte_signed)) ==
          hex("FF FF"));

    CHECK(encoded(rdpei::write_four_byte_unsigned, std::uint32_t{0x3F}) == hex("3F"));
    CHECK(encoded(rdpei::write_four_byte_unsigned, std::uint32_t{0x40}) == hex("40 40"));
    CHECK(encoded(rdpei::write_four_byte_unsigned, std::uint32_t{0x4000}) == hex("80 40 00"));
    CHECK(encoded(rdpei::write_four_byte_unsigned, rdpei::max_four_byte_unsigned) == hex("FF FF FF FF"));

    CHECK(encoded(rdpei::write_four_byte_signed, std::int32_t{0x1F}) == hex("1F"));
    CHECK(encoded(rdpei::write_four_byte_signed, std::int32_t{0x20}) == hex("40 20"));
    CHECK(encoded(rdpei::write_four_byte_signed, rdpei::max_four_byte_signed) == hex("DF FF FF FF"));
    CHECK(encoded(rdpei::write_four_byte_signed, -rdpei::max_four_byte_signed) == hex("FF FF FF FF"));

    CHECK(encoded(rdpei::write_eight_byte_unsigned, std::uint64_t{0x1F}) == hex("1F"));
    CHECK(encoded(rdpei::write_eight_byte_unsigned, std::uint64_t{0x20}) == hex("20 20"));
    CHECK(encoded(rdpei::write_eight_byte_unsigned, rdpei::max_eight_byte_unsigned) == hex("FF FF FF FF FF FF FF FF"));
}

TEST_CASE("RDPEI variable-length integers round-trip")
{
    for (std::uint32_t v = 0; v <= rdpei::max_two_byte_unsigned; v += 7) {
        const auto value = static_cast<std::uint16_t>(v);
        CHECK(decoded(rdpei::read_two_byte_unsigned, encoded(rdpei::write_two_byte_unsigned, value)) == value);
    }
    for (std::int32_t v = -rdpei::max_two_byte_signed; v <= rdpei::max_two_byte_signed; v += 5) {
        const auto value = static_cast<std::int16_t>(v);
        CHECK(decoded(rdpei::read_two_byte_signed, encoded(rdpei::write_two_byte_signed, value)) == value);
    }
    for (const std::uint32_t value : {0U, 0x3FU, 0x40U, 0x3FFFU, 0x4000U, 0x3FFFFFU, 0x400000U, 0x3FFFFFFFU}) {
        CHECK(decoded(rdpei::read_four_byte_unsigned, encoded(rdpei::write_four_byte_unsigned, value)) == value);
    }
    for (const std::int32_t value : {0, 1, -1, 0x1F, -0x20, 0x1FFF, -0x2000, 0x1FFFFF, -0x200000, 0x1FFFFFFF}) {
        CHECK(decoded(rdpei::read_four_byte_signed, encoded(rdpei::write_four_byte_signed, value)) == value);
    }
    for (unsigned bits = 0; bits <= 61; ++bits) {
        const std::uint64_t value = (std::uint64_t{1} << bits) - 1;
        CHECK(decoded(rdpei::read_eight_byte_unsigned, encoded(rdpei::write_eight_byte_unsigned, value)) == value);
    }
}

TEST_CASE("RDPEI variable-length integers: longer encodings are accepted, truncated ones are not")
{
    // FreeRDP writes 0x7F in two bytes.
    CHECK(decoded(rdpei::read_two_byte_unsigned, hex("80 7F")) == 0x7F);
    CHECK(decoded(rdpei::read_two_byte_signed, hex("80 3F")) == 0x3F);
    CHECK(decoded(rdpei::read_four_byte_unsigned, hex("C0 00 00 05")) == 5);
    CHECK(decoded(rdpei::read_four_byte_signed, hex("A0 00 05")) == -5);
    CHECK(decoded(rdpei::read_eight_byte_unsigned, hex("E0 00 00 00 00 00 00 05")) == 5);
    CHECK(decoded(rdpei::read_two_byte_signed, hex("40")) == 0);  // negative zero

    CHECK(decode_error(rdpei::read_two_byte_unsigned, hex("")) == Errc::truncated);
    CHECK(decode_error(rdpei::read_two_byte_unsigned, hex("80")) == Errc::truncated);
    CHECK(decode_error(rdpei::read_two_byte_signed, hex("C0")) == Errc::truncated);
    CHECK(decode_error(rdpei::read_four_byte_unsigned, hex("C0 00 00")) == Errc::truncated);
    CHECK(decode_error(rdpei::read_four_byte_signed, hex("80 00")) == Errc::truncated);
    CHECK(decode_error(rdpei::read_eight_byte_unsigned, hex("E0 00 00 00 00 00 00")) == Errc::truncated);
}

TEST_CASE("RDPEI server PDUs")
{
    const Bytes sc_ready_v300 = hex("01 00 0E 00 00 00  00 00 03 00  01 00 00 00");
    const Bytes sc_ready_v200 = hex("01 00 0A 00 00 00  00 00 02 00");
    CHECK(rdpei::encode_server_pdu(
              rdpei::ScReady{rdpei::version::v300, rdpei::sc_features::multipen_injection_supported}) == sc_ready_v300);
    CHECK(rdpei::encode_server_pdu(rdpei::ScReady{rdpei::version::v200, std::nullopt}) == sc_ready_v200);
    CHECK(rdpei::encode_server_pdu(rdpei::SuspendInput{}) == hex("04 00 06 00 00 00"));
    CHECK(rdpei::encode_server_pdu(rdpei::ResumeInput{}) == hex("05 00 06 00 00 00"));

    CHECK(rdpei::decode_server_pdu(sc_ready_v300).value() == rdpei::ServerPdu{rdpei::ScReady{rdpei::version::v300, 1}});
    CHECK(rdpei::decode_server_pdu(sc_ready_v200).value() ==
          rdpei::ServerPdu{rdpei::ScReady{rdpei::version::v200, std::nullopt}});
    CHECK(rdpei::decode_server_pdu(hex("05 00 06 00 00 00")).value() == rdpei::ServerPdu{rdpei::ResumeInput{}});
    CHECK(rdpei::decode_server_pdu(hex("04 00 07 00 00 00 00")).error().code == Errc::trailing_data);
    CHECK(rdpei::decode_server_pdu(hex("02 00 06 00 00 00")).error().code == Errc::unsupported);
}

TEST_CASE("RDPEI CS_READY as FreeRDP 3 sends it")
{
    // SHOW_TOUCH_VISUALS | DISABLE_TIMESTAMP_INJECTION, version 3.0, 10 contacts.
    const Bytes bytes = hex("02 00 10 00 00 00  03 00 00 00  00 00 03 00  0A 00");
    const rdpei::CsReady expected{.flags = rdpei::cs_flags::show_touch_visuals |
                                           rdpei::cs_flags::disable_timestamp_injection,
                                  .protocol_version = rdpei::version::v300,
                                  .max_touch_contacts = 10};
    CHECK(client(bytes) == rdpei::ClientPdu{expected});
    CHECK(rdpei::encode_client_pdu(expected) == bytes);
}

TEST_CASE("RDPEI touch event PDU")
{
    // One frame, contact 0 down at (100, 200) with a 4x4 contact rectangle.
    const Bytes bytes = hex("03 00 15 00 00 00"
                            "00 01"                 // encodeTime 0, frameCount 1
                            "01 00"                 // contactCount 1, frameOffset 0
                            "00 01 40 64 40 C8 19"  // id 0, rect present, x 100, y 200, DOWN|INRANGE|INCONTACT
                            "42 42 02 02");         // rect -2, -2, 2, 2
    rdpei::TouchContact contact{.contact_id = 0,
                                .x = 100,
                                .y = 200,
                                .contact_flags = rdpei::contact_flags::down | rdpei::contact_flags::in_range |
                                                 rdpei::contact_flags::in_contact,
                                .contact_rect = rdpei::ContactRect{-2, -2, 2, 2},
                                .orientation = std::nullopt,
                                .pressure = std::nullopt};
    const rdpei::TouchEvent expected{.encode_time = 0, .frames = {rdpei::TouchFrame{0, {contact}}}};
    CHECK(client(bytes) == rdpei::ClientPdu{expected});
    CHECK(rdpei::encode_client_pdu(expected) == bytes);

    SECTION("all optional fields, several frames and contacts, negative coordinates")
    {
        rdpei::TouchEvent event{.encode_time = 12345, .frames = {}};
        contact.orientation = 359;
        contact.pressure = 1024;
        contact.x = -1920;
        rdpei::TouchContact other{.contact_id = 7,
                                  .x = 0x1FFFFFFF,
                                  .y = -0x1FFFFFFF,
                                  .contact_flags = rdpei::contact_flags::up,
                                  .contact_rect = std::nullopt,
                                  .orientation = std::nullopt,
                                  .pressure = 0};
        event.frames.push_back(rdpei::TouchFrame{0, {contact, other}});
        event.frames.push_back(rdpei::TouchFrame{rdpei::max_eight_byte_unsigned, {other}});
        event.frames.push_back(rdpei::TouchFrame{16000, {}});
        CHECK(client(rdpei::encode_client_pdu(event)) == rdpei::ClientPdu{event});
    }
}

TEST_CASE("RDPEI pen event PDU")
{
    // Pen 0 down at (10, 20) with pressure 512.
    const Bytes bytes = hex("08 00 11 00 00 00  00 01  01 00  00 02 0A 14 19 42 00");
    const rdpei::PenContact contact{.device_id = 0,
                                    .x = 10,
                                    .y = 20,
                                    .contact_flags = 0x19,
                                    .pen_flags = std::nullopt,
                                    .pressure = 512,
                                    .rotation = std::nullopt,
                                    .tilt_x = std::nullopt,
                                    .tilt_y = std::nullopt};
    const rdpei::PenEvent expected{.encode_time = 0, .frames = {rdpei::PenFrame{0, {contact}}}};
    CHECK(client(bytes) == rdpei::ClientPdu{expected});
    CHECK(rdpei::encode_client_pdu(expected) == bytes);

    SECTION("every field")
    {
        rdpei::PenContact full = contact;
        full.device_id = 3;
        full.pen_flags = rdpei::pen_flags::barrel_pressed | rdpei::pen_flags::inverted;
        full.rotation = 359;
        full.tilt_x = -90;
        full.tilt_y = 90;
        const rdpei::PenEvent event{.encode_time = 1, .frames = {rdpei::PenFrame{5, {full, contact}}}};
        CHECK(client(rdpei::encode_client_pdu(event)) == rdpei::ClientPdu{event});
    }
}

TEST_CASE("RDPEI dismiss hovering contact PDU")
{
    CHECK(client(hex("06 00 07 00 00 00 05")) == rdpei::ClientPdu{rdpei::DismissHoveringContact{5}});
    CHECK(rdpei::encode_client_pdu(rdpei::DismissHoveringContact{5}) == hex("06 00 07 00 00 00 05"));
}

TEST_CASE("RDPEI client PDUs are decoded strictly")
{
    SECTION("pduLength must match the PDU")
    {
        CHECK(client_error(hex("02 00 11 00 00 00  03 00 00 00  00 00 03 00  0A 00")) == Errc::invalid_length);
        CHECK(client_error(hex("02 00 10 00")) == Errc::truncated);
    }
    SECTION("nothing may follow the last field")
    {
        CHECK(client_error(hex("02 00 11 00 00 00  03 00 00 00  00 00 03 00  0A 00  00")) == Errc::trailing_data);
        CHECK(client_error(hex("06 00 08 00 00 00 05 00")) == Errc::trailing_data);
    }
    SECTION("fields the specification does not define cannot be skipped")
    {
        CHECK(client_error(hex("03 00 0F 00 00 00  00 01 01 00  00 08 00 00 19")) == Errc::unsupported);
        CHECK(client_error(hex("08 00 0F 00 00 00  00 01 01 00  00 20 00 00 19")) == Errc::unsupported);
    }
    SECTION("counts")
    {
        // 129 frames: over farland's limit, before looking at the bytes.
        CHECK(client_error(hex("03 00 09 00 00 00  00 80 81")) == Errc::limit_exceeded);
        // 5 pens in a frame.
        CHECK(client_error(hex("08 00 0A 00 00 00  00 01 05 00")) == Errc::limit_exceeded);
        // 5 frames announced, none there.
        CHECK(client_error(hex("03 00 08 00 00 00  00 05")) == Errc::truncated);
        // A contact cut short.
        CHECK(client_error(hex("03 00 0C 00 00 00  00 01 01 00  00 00")) == Errc::truncated);
    }
    SECTION("PDUs the client does not send")
    {
        CHECK(client_error(hex("01 00 0A 00 00 00  00 00 02 00")) == Errc::unsupported);
        CHECK(client_error(hex("04 00 06 00 00 00")) == Errc::unsupported);
        CHECK(client_error(hex("07 00 06 00 00 00")) == Errc::unsupported);
    }
}

TEST_CASE("RDPEI frame_pdu finds the first PDU of a message")
{
    const Bytes two = hex("06 00 07 00 00 00 05  06 00 07 00 00 00 06");
    CHECK(rdpei::frame_pdu(two).value() == 7);
    CHECK(rdpei::frame_pdu(hex("06 00 05 00 00 00")).error().code == Errc::invalid_length);
    CHECK(rdpei::frame_pdu(hex("06 00 08 00 00 00 05")).error().code == Errc::truncated);
    CHECK(rdpei::frame_pdu(hex("06 00 01 00 01 00")).error().code == Errc::limit_exceeded);  // 64 KiB + 1
    CHECK(rdpei::frame_pdu(hex("06 00 07")).error().code == Errc::truncated);
}
