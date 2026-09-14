// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/channels/audin.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using farland::Errc;
using farland::test::hex;
namespace audin = farland::channels::audin;
namespace rdpsnd = farland::channels::rdpsnd;
namespace ev = farland::channels::audin_event;
using farland::channels::AudinServer;
using farland::channels::AudinServerConfig;
using Bytes = std::vector<std::byte>;

namespace {

AudinServerConfig config()
{
    return AudinServerConfig{.formats = {rdpsnd::pcm_format(48000, 1), rdpsnd::pcm_format(44100, 2)}, .packet_ms = 20};
}

template <class T>
T server_pdu(const Bytes& message)
{
    return std::get<T>(audin::decode_server_pdu(message).value());
}

/// A server that sent its Open PDU for 48 kHz mono, index 1 of the client's list.
AudinServer opening_server()
{
    AudinServer server(config());
    server.start();
    REQUIRE(server.receive(audin::encode_client_pdu(audin::Version{2})));
    REQUIRE(server.receive(audin::encode_client_pdu(audin::IncomingData{})));
    REQUIRE(server.receive(
        audin::encode_client_pdu(audin::Formats{{rdpsnd::pcm_format(44100, 2), rdpsnd::pcm_format(48000, 1)}})));
    static_cast<void>(server.take_output());
    return server;
}

}  // namespace

TEST_CASE("Version PDU layout ([MS-RDPEAI] 2.2.2.1)")
{
    CHECK(audin::encode_server_pdu(audin::Version{2}) == hex("01 02 00 00 00"));
    CHECK(std::get<audin::Version>(audin::decode_client_pdu(hex("01 01 00 00 00")).value()).version == 1);
    CHECK(audin::decode_client_pdu(hex("01 01 00 00 00 00")).error().code == Errc::trailing_data);
}

TEST_CASE("Sound Formats PDU: cbSizeFormatsPacket points at ExtraData ([MS-RDPEAI] 2.2.2.2)")
{
    const auto format = "01 00 01 00 80 bb 00 00 00 77 01 00 02 00 10 00 00 00";  // PCM 48 kHz mono
    CHECK(audin::encode_server_pdu(audin::Formats{{rdpsnd::pcm_format(48000, 1)}}) ==
          hex(std::string("02 01 00 00 00 1b 00 00 00 ") + format));
    // From the client, with two bytes of ExtraData after the formats.
    const auto with_extra = hex(std::string("02 01 00 00 00 1b 00 00 00 ") + format + " ee ee");
    CHECK(std::get<audin::Formats>(audin::decode_client_pdu(with_extra).value()).formats ==
          std::vector{rdpsnd::pcm_format(48000, 1)});
    const auto wrong_size = hex(std::string("02 01 00 00 00 1d 00 00 00 ") + format + " ee ee");
    CHECK(audin::decode_client_pdu(wrong_size).error().code == Errc::invalid_length);
    CHECK(audin::decode_client_pdu(hex("02 ff ff ff ff 09 00 00 00")).error().code == Errc::limit_exceeded);
}

TEST_CASE("Open PDU with a WAVEFORMAT_EXTENSIBLE capture format ([MS-RDPEAI] 2.2.2.3)")
{
    const audin::Open open{
        .frames_per_packet = 960, .initial_format = 1, .capture_format = audin::extensible_pcm_format(48000, 2)};
    const auto bytes = audin::encode_server_pdu(open);
    CHECK(bytes == hex("03 c0 03 00 00 01 00 00 00"
                       " fe ff 02 00 80 bb 00 00 00 ee 02 00 04 00 10 00 16 00"
                       " 10 00 03 00 00 00 01 00 00 00 00 00 10 00 80 00 00 aa 00 38 9b 71"));
    CHECK(server_pdu<audin::Open>(bytes) == open);
}

TEST_CASE("Open Reply, Incoming Data, Data and Format Change PDUs ([MS-RDPEAI] 2.2.2.4, 2.2.3, 2.2.4.1)")
{
    CHECK(std::get<audin::OpenReply>(audin::decode_client_pdu(hex("04 05 40 00 80")).value()).result == 0x80004005);
    CHECK(std::holds_alternative<audin::IncomingData>(audin::decode_client_pdu(hex("05")).value()));
    CHECK(audin::decode_client_pdu(hex("05 00")).error().code == Errc::trailing_data);
    const auto data = hex("06 01 02 03 04");
    CHECK(std::get<audin::Data>(audin::decode_client_pdu(data).value()).data.size() == 4);
    CHECK(std::get<audin::FormatChange>(audin::decode_client_pdu(hex("07 01 00 00 00")).value()).new_format == 1);
    // The Open PDU comes only from the server.
    CHECK(audin::decode_client_pdu(hex("03 00 00 00 00")).error().code == Errc::unsupported);
}

TEST_CASE("AudinServer negotiates and opens the preferred format ([MS-RDPEAI] 3.1.3)")
{
    AudinServer server(config());
    server.start();
    auto out = server.take_output();
    REQUIRE(out.size() == 1);
    CHECK(server_pdu<audin::Version>(out[0]).version == audin::version2);

    REQUIRE(server.receive(audin::encode_client_pdu(audin::Version{1})));
    out = server.take_output();
    REQUIRE(out.size() == 1);
    CHECK(server_pdu<audin::Formats>(out[0]).formats == config().formats);

    REQUIRE(server.receive(audin::encode_client_pdu(audin::IncomingData{})));
    REQUIRE(server.receive(
        audin::encode_client_pdu(audin::Formats{{rdpsnd::pcm_format(44100, 2), rdpsnd::pcm_format(48000, 1)}})));
    out = server.take_output();
    REQUIRE(out.size() == 1);
    const auto open = server_pdu<audin::Open>(out[0]);
    CHECK(open.initial_format == 1);  // 48 kHz mono comes first in the server's preference
    CHECK(open.frames_per_packet == 960);
    CHECK(open.capture_format == audin::extensible_pcm_format(48000, 1));

    // 3.2.5.1.7: the client confirms the format before the Open Reply.
    REQUIRE(server.receive(audin::encode_client_pdu(audin::FormatChange{1})));
    REQUIRE(server.receive(audin::encode_client_pdu(audin::OpenReply{0})));
    const auto opened = std::get<ev::Opened>(server.poll_event().value());
    CHECK(opened.format == rdpsnd::pcm_format(48000, 1));
    CHECK(opened.frames_per_packet == 960);

    Bytes samples(1920, std::byte{0x11});
    REQUIRE(server.receive(audin::encode_client_pdu(audin::IncomingData{})));
    const auto message = audin::encode_client_pdu(audin::Data{samples});
    REQUIRE(server.receive(message));
    CHECK(std::get<ev::Data>(server.poll_event().value()).data.size() == 1920);
}

TEST_CASE("AudinServer ignores out-of-sequence PDUs and refuses malformed ones ([MS-RDPEAI] 3.1.5)")
{
    SECTION("data before the Open Reply is ignored")
    {
        auto server = opening_server();
        REQUIRE(server.receive(audin::encode_client_pdu(audin::Data{hex("00 00")})));
        CHECK_FALSE(server.poll_event());
    }
    SECTION("a failed Open Reply ends the microphone")
    {
        auto server = opening_server();
        REQUIRE(server.receive(audin::encode_client_pdu(audin::OpenReply{0x80004005})));
        CHECK(std::get<ev::Failed>(server.poll_event().value()).result == 0x80004005);
        CHECK_FALSE(server.opened());
    }
    SECTION("a Format Change to another format is an error")
    {
        auto server = opening_server();
        CHECK(server.receive(audin::encode_client_pdu(audin::FormatChange{0})).error().code == Errc::invalid_value);
    }
    SECTION("data that is not a whole number of frames is an error")
    {
        auto server = opening_server();
        REQUIRE(server.receive(audin::encode_client_pdu(audin::OpenReply{0})));
        static_cast<void>(server.poll_event());
        CHECK(server.receive(audin::encode_client_pdu(audin::Data{hex("00 00 00")})).error().code ==
              Errc::invalid_length);
    }
    SECTION("client formats the server did not offer are an error")
    {
        AudinServer server(config());
        server.start();
        REQUIRE(server.receive(audin::encode_client_pdu(audin::Version{2})));
        CHECK_FALSE(server.receive(audin::encode_client_pdu(audin::Formats{{rdpsnd::pcm_format(8000, 1)}})));
    }
    SECTION("no common format")
    {
        AudinServer server(config());
        server.start();
        REQUIRE(server.receive(audin::encode_client_pdu(audin::Version{2})));
        REQUIRE(server.receive(audin::encode_client_pdu(audin::Formats{{}})));
        CHECK(std::holds_alternative<ev::Failed>(server.poll_event().value()));
    }
}
