// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Audio output PDUs ([MS-RDPEA]). Input: records of [u16le length][message].
// Each message is decoded as a client PDU and as a server PDU; whatever
// decodes must survive encoding and decoding again. The client messages
// also drive an RdpsndServer, which sends a sample whenever it is ready.

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/channels/rdpsnd.hpp>
#include <farland/channels/rdpsnd_server.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <vector>

using namespace farland;
namespace rdpsnd = farland::channels::rdpsnd;

namespace {

void check_client_round_trip(std::span<const std::byte> message)
{
    const auto pdu = rdpsnd::decode_client_pdu(message);
    if (!pdu) {
        return;
    }
    const auto again = rdpsnd::decode_client_pdu(rdpsnd::encode_client_pdu(*pdu));
    FARLAND_ASSERT(again.has_value() && *again == *pdu);
}

void check_server_round_trip(std::span<const std::byte> message)
{
    const auto pdu = rdpsnd::decode_server_pdu(message);
    if (!pdu) {
        return;
    }
    if (const auto* info = std::get_if<rdpsnd::WaveInfo>(&*pdu)) {
        // A sample of the announced size must come back through encode_wave.
        std::vector<std::byte> sample(info->sample_size, std::byte{0x5A});
        std::ranges::copy(info->first_bytes, sample.begin());
        const auto [first, rest] = rdpsnd::encode_wave(rdpsnd::Wave{
            .timestamp = info->timestamp, .format_no = info->format_no, .block_no = info->block_no, .data = sample});
        const auto decoded = rdpsnd::decode_server_pdu(first);
        FARLAND_ASSERT(decoded.has_value() && std::get<rdpsnd::WaveInfo>(*decoded) == *info);
        const auto body = rdpsnd::decode_wave_body(rest, *info);
        FARLAND_ASSERT(body.has_value() && *body == sample);
        return;
    }
    const auto again = rdpsnd::decode_server_pdu(rdpsnd::encode_server_pdu(*pdu));
    FARLAND_ASSERT(again.has_value() && *again == *pdu);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    Reader r(std::as_bytes(std::span(data, size)));
    channels::RdpsndServer server(channels::RdpsndServerConfig{
        .formats = {rdpsnd::pcm_format(48000, 2), rdpsnd::pcm_format(44100, 2), rdpsnd::pcm_format(22050, 1)}});
    server.start();
    static_cast<void>(server.take_output());
    std::uint32_t now = 0;
    const std::array<std::byte, 8> sample{};
    while (!r.empty()) {
        const auto length = r.u16le();
        if (!length) {
            break;
        }
        const auto message = r.bytes(std::min<std::size_t>(*length, r.remaining())).value();
        check_client_round_trip(message);
        check_server_round_trip(message);
        now += 7;
        if (server.receive(message, now).has_value() && server.ready()) {
            server.send_wave(0, sample, now, now);
        }
        while (server.poll_event()) {
        }
        for (const auto& out : server.take_output()) {
            FARLAND_ASSERT(rdpsnd::decode_server_pdu(out).has_value() || out.size() >= 4);
        }
    }
    return 0;
}
