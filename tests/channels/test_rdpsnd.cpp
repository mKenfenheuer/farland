// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/channels/rdpsnd.hpp>
#include <farland/channels/rdpsnd_server.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using farland::Errc;
using farland::test::hex;
namespace rdpsnd = farland::channels::rdpsnd;
namespace ev = farland::channels::rdpsnd_event;
using farland::channels::RdpsndServer;
using farland::channels::RdpsndServerConfig;
using Bytes = std::vector<std::byte>;

namespace {

/// PCM 44.1 kHz stereo 16-bit as an AUDIO_FORMAT.
constexpr const char* pcm44_hex = "01 00 02 00 44 ac 00 00 10 b1 02 00 04 00 10 00 00 00";

Bytes client_formats(std::uint16_t version, std::uint32_t flags, const std::vector<rdpsnd::AudioFormat>& formats)
{
    return rdpsnd::encode_client_pdu(rdpsnd::ClientFormats{.flags = flags,
                                                           .volume = 0xFFFFFFFF,
                                                           .pitch = 0x00010000,
                                                           .dgram_port = 0,
                                                           .version = version,
                                                           .formats = formats});
}

RdpsndServerConfig config()
{
    return RdpsndServerConfig{.formats = {rdpsnd::pcm_format(48000, 2), rdpsnd::pcm_format(44100, 2)}};
}

/// A server past the initialization sequence with a client of `version`.
RdpsndServer ready_server(std::uint16_t version)
{
    RdpsndServer server(config());
    server.start();
    static_cast<void>(server.take_output());
    REQUIRE(server.receive(client_formats(version, rdpsnd::caps::alive, {rdpsnd::pcm_format(44100, 2)}), 100));
    const auto training = rdpsnd::decode_server_pdu(server.take_output().at(0)).value();
    const auto ts = std::get<rdpsnd::Training>(training).timestamp;
    REQUIRE(server.receive(rdpsnd::encode_client_pdu(rdpsnd::TrainingConfirm{ts, 0}), 120));
    REQUIRE(server.ready());
    static_cast<void>(server.poll_event());
    return server;
}

}  // namespace

TEST_CASE("Server Audio Formats and Version PDU layout ([MS-RDPEA] 2.2.2.1)")
{
    const rdpsnd::ServerFormats pdu{
        .version = 8, .last_block_confirmed = 0xFF, .formats = {rdpsnd::pcm_format(44100, 2)}};
    const auto bytes = rdpsnd::encode_server_pdu(pdu);
    CHECK(bytes ==
          hex(std::string("07 00 26 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 00 ff 08 00 00 ") + pcm44_hex));
    CHECK(std::get<rdpsnd::ServerFormats>(rdpsnd::decode_server_pdu(bytes).value()) == pdu);
}

TEST_CASE("Client Audio Formats and Version PDU decoding ([MS-RDPEA] 2.2.2.2)")
{
    // TSSNDCAPS_ALIVE | TSSNDCAPS_VOLUME, full volume, pitch 1.0, UDP port
    // 1234 (big-endian), version 8, one format.
    const auto bytes =
        hex(std::string("07 00 26 00 03 00 00 00 ff ff ff ff 00 00 01 00 04 d2 01 00 00 08 00 00 ") + pcm44_hex);
    const auto pdu = std::get<rdpsnd::ClientFormats>(rdpsnd::decode_client_pdu(bytes).value());
    CHECK(pdu.flags == (rdpsnd::caps::alive | rdpsnd::caps::volume));
    CHECK(pdu.volume == 0xFFFFFFFF);
    CHECK(pdu.pitch == 0x00010000);
    CHECK(pdu.dgram_port == 1234);
    CHECK(pdu.version == 8);
    REQUIRE(pdu.formats.size() == 1);
    CHECK(pdu.formats[0] == rdpsnd::pcm_format(44100, 2));
    CHECK(rdpsnd::encode_client_pdu(pdu) == bytes);

    SECTION("BodySize must cover the rest of the message exactly")
    {
        auto longer = bytes;
        longer.push_back(std::byte{0});
        CHECK(rdpsnd::decode_client_pdu(longer).error().code == Errc::invalid_length);
        CHECK(rdpsnd::decode_client_pdu(std::span(bytes).first(bytes.size() - 1)).error().code == Errc::invalid_length);
    }
    SECTION("A format without channels is refused")
    {
        auto broken = bytes;
        broken[24 + 2] = std::byte{0};
        CHECK(rdpsnd::decode_client_pdu(broken).error().code == Errc::invalid_value);
    }
    SECTION("A format count beyond the message is truncated input")
    {
        auto broken = bytes;
        broken[18] = std::byte{2};
        CHECK(rdpsnd::decode_client_pdu(broken).error().code == Errc::truncated);
    }
}

TEST_CASE("Quality Mode, Training Confirm and Wave Confirm PDUs ([MS-RDPEA] 2.2.2.3, 2.2.3.2, 2.2.3.8)")
{
    CHECK(std::get<rdpsnd::QualityMode>(rdpsnd::decode_client_pdu(hex("0c 00 04 00 02 00 00 00")).value()).mode ==
          rdpsnd::quality::high);
    CHECK(std::get<rdpsnd::TrainingConfirm>(rdpsnd::decode_client_pdu(hex("06 00 04 00 34 12 00 04")).value()) ==
          rdpsnd::TrainingConfirm{0x1234, 0x0400});
    CHECK(std::get<rdpsnd::WaveConfirm>(rdpsnd::decode_client_pdu(hex("05 00 04 00 10 00 07 00")).value()) ==
          rdpsnd::WaveConfirm{0x10, 7});
    CHECK(rdpsnd::encode_client_pdu(rdpsnd::WaveConfirm{0x10, 7}) == hex("05 00 04 00 10 00 07 00"));
    // UDP PDUs and unknown types are not client PDUs farland accepts.
    CHECK(rdpsnd::decode_client_pdu(hex("0a 00 00 00")).error().code == Errc::unsupported);
    CHECK(rdpsnd::decode_client_pdu(hex("05 00 04")).error().code == Errc::truncated);
}

TEST_CASE("Training PDU wPackSize is the whole PDU or 0 ([MS-RDPEA] 2.2.3.1)")
{
    CHECK(rdpsnd::encode_server_pdu(rdpsnd::Training{0x0102, 0}) == hex("06 00 04 00 02 01 00 00"));
    const auto with_data = rdpsnd::encode_server_pdu(rdpsnd::Training{0x0102, 4});
    CHECK(with_data == hex("06 00 08 00 02 01 0c 00 00 00 00 00"));
    CHECK(std::get<rdpsnd::Training>(rdpsnd::decode_server_pdu(with_data).value()) == rdpsnd::Training{0x0102, 4});
    CHECK(rdpsnd::decode_server_pdu(hex("06 00 08 00 02 01 00 00 00 00 00 00")).error().code == Errc::invalid_length);
}

TEST_CASE("WaveInfo and Wave PDUs split a sample after four bytes ([MS-RDPEA] 2.2.3.3, 2.2.3.4, 3.3.5.2.1.1)")
{
    const auto sample = hex("00 01 02 03 04 05 06 07 08 09");
    const auto [info, wave] =
        rdpsnd::encode_wave(rdpsnd::Wave{.timestamp = 0x0100, .format_no = 1, .block_no = 5, .data = sample});
    // BodySize: the sample plus 8.
    CHECK(info == hex("02 00 12 00 00 01 01 00 05 00 00 00 00 01 02 03"));
    CHECK(wave == hex("00 00 00 00 04 05 06 07 08 09"));
    const auto decoded = std::get<rdpsnd::WaveInfo>(rdpsnd::decode_server_pdu(info).value());
    CHECK(decoded.sample_size == 10);
    CHECK(decoded.block_no == 5);
    CHECK(rdpsnd::decode_wave_body(wave, decoded).value() == sample);
    CHECK(rdpsnd::decode_wave_body(std::span(wave).first(9), decoded).error().code == Errc::invalid_length);
}

TEST_CASE("Wave2 PDU layout ([MS-RDPEA] 2.2.3.10)")
{
    const rdpsnd::Wave2 pdu{
        .timestamp = 0x0203, .format_no = 2, .block_no = 9, .audio_timestamp = 0x11223344, .data = hex("aa bb")};
    const auto bytes = rdpsnd::encode_server_pdu(pdu);
    CHECK(bytes == hex("0d 00 0e 00 03 02 02 00 09 00 00 00 44 33 22 11 aa bb"));
    CHECK(std::get<rdpsnd::Wave2>(rdpsnd::decode_server_pdu(bytes).value()) == pdu);
}

TEST_CASE("Close, Volume and Pitch PDUs ([MS-RDPEA] 2.2.3.9, 2.2.4)")
{
    CHECK(rdpsnd::encode_server_pdu(rdpsnd::Close{}) == hex("01 00 00 00"));
    CHECK(rdpsnd::encode_server_pdu(rdpsnd::Volume{0xFFFF8000}) == hex("03 00 04 00 00 80 ff ff"));
    CHECK(rdpsnd::encode_server_pdu(rdpsnd::Pitch{0x00010000}) == hex("04 00 04 00 00 00 01 00"));
}

TEST_CASE("RdpsndServer runs the initialization sequence ([MS-RDPEA] 3.3.5.1)")
{
    RdpsndServer server(config());
    server.start();
    auto out = server.take_output();
    REQUIRE(out.size() == 1);
    const auto formats = std::get<rdpsnd::ServerFormats>(rdpsnd::decode_server_pdu(out[0]).value());
    CHECK(formats.version == rdpsnd::version::windows_8);
    CHECK(formats.formats == config().formats);

    REQUIRE(server.receive(client_formats(8, rdpsnd::caps::alive, {rdpsnd::pcm_format(44100, 2)}), 1000));
    out = server.take_output();
    REQUIRE(out.size() == 1);
    const auto training = std::get<rdpsnd::Training>(rdpsnd::decode_server_pdu(out[0]).value());
    CHECK(training.timestamp == 1000);
    CHECK_FALSE(server.poll_event());

    REQUIRE(server.receive(rdpsnd::encode_client_pdu(rdpsnd::QualityMode{rdpsnd::quality::high}), 1010));
    REQUIRE(server.receive(rdpsnd::encode_client_pdu(rdpsnd::TrainingConfirm{1000, 0}), 1030));
    const auto ready = std::get<ev::Ready>(server.poll_event().value());
    CHECK(ready.client_version == 8);
    CHECK(ready.formats == std::vector{rdpsnd::pcm_format(44100, 2)});
    CHECK(ready.quality_mode == rdpsnd::quality::high);
    CHECK(ready.training_round_trip_ms == 30);
    CHECK(server.ready());
}

TEST_CASE("RdpsndServer refuses out-of-sequence and foreign PDUs ([MS-RDPEA] 3.3.5.1.1.2)")
{
    SECTION("a client format the server never offered")
    {
        RdpsndServer server(config());
        server.start();
        const auto result = server.receive(client_formats(8, rdpsnd::caps::alive, {rdpsnd::pcm_format(8000, 1)}), 0);
        CHECK(result.error().code == Errc::invalid_value);
        // Errors are final.
        CHECK_FALSE(server.receive(client_formats(8, rdpsnd::caps::alive, {}), 0));
    }
    SECTION("a Wave Confirm before the data transfer sequence")
    {
        RdpsndServer server(config());
        server.start();
        CHECK_FALSE(server.receive(rdpsnd::encode_client_pdu(rdpsnd::WaveConfirm{0, 0}), 0));
    }
    SECTION("a Training Confirm without a Training")
    {
        RdpsndServer server(config());
        server.start();
        CHECK_FALSE(server.receive(rdpsnd::encode_client_pdu(rdpsnd::TrainingConfirm{0, 0}), 0));
    }
    SECTION("a client without TSSNDCAPS_ALIVE is unavailable, not an error")
    {
        RdpsndServer server(config());
        server.start();
        REQUIRE(server.receive(client_formats(8, rdpsnd::caps::volume, {rdpsnd::pcm_format(44100, 2)}), 0));
        CHECK(std::holds_alternative<ev::Unavailable>(server.poll_event().value()));
        CHECK_FALSE(server.ready());
    }
}

TEST_CASE("Version 8 clients get Wave2, older ones WaveInfo and Wave ([MS-RDPEA] 3.3.5.2.1.8)")
{
    const auto sample = hex("01 00 02 00 03 00 04 00");
    {
        auto server = ready_server(8);
        const auto block = server.send_wave(0, sample, 500, 77);
        CHECK(block == 0);  // one after cLastBlockConfirmed 0xFF
        const auto out = server.take_output();
        REQUIRE(out.size() == 1);
        const auto wave = std::get<rdpsnd::Wave2>(rdpsnd::decode_server_pdu(out[0]).value());
        CHECK(wave.timestamp == 500);
        CHECK(wave.audio_timestamp == 77);
        CHECK(wave.data == sample);
    }
    {
        auto server = ready_server(6);
        server.send_wave(0, sample, 500, 77);
        const auto out = server.take_output();
        REQUIRE(out.size() == 2);
        const auto info = std::get<rdpsnd::WaveInfo>(rdpsnd::decode_server_pdu(out[0]).value());
        CHECK(rdpsnd::decode_wave_body(out[1], info).value() == sample);
    }
}

TEST_CASE("Block numbers wrap and confirmations carry the sent timestamp ([MS-RDPEA] 3.3.5.2.1.1)")
{
    auto server = ready_server(8);
    const auto sample = hex("01 00 02 00 03 00 04 00");
    std::uint8_t last = 0;
    for (int i = 0; i < 257; ++i) {
        last = server.send_wave(0, sample, static_cast<std::uint32_t>(1000 + i), 0);
    }
    CHECK(last == 0);  // 256 blocks later, 0 again
    static_cast<void>(server.take_output());
    REQUIRE(server.receive(rdpsnd::encode_client_pdu(rdpsnd::WaveConfirm{1300, 0}), 1300));
    const auto confirmed = std::get<ev::WaveConfirmed>(server.poll_event().value());
    CHECK(confirmed.block_no == 0);
    CHECK(confirmed.timestamp == 1300);
    CHECK(confirmed.sent_timestamp == 1256);
}

TEST_CASE("Volume PDUs go only to clients with TSSNDCAPS_VOLUME ([MS-RDPEA] 3.3.5.3.1.1)")
{
    auto server = ready_server(8);
    CHECK_FALSE(server.send_volume(0xFFFFFFFF));
    CHECK(server.take_output().empty());
}
