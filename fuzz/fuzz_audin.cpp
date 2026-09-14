// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Audio input PDUs ([MS-RDPEAI]). Input: records of [u16le length][message].
// Each message is decoded as a client and as a server PDU; whatever decodes
// must survive encoding and decoding again. The messages also drive an
// AudinServer.

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/channels/audin.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <span>
#include <vector>

using namespace farland;
namespace audin = farland::channels::audin;
namespace rdpsnd = farland::channels::rdpsnd;

namespace {

bool same(const audin::ClientPdu& a, const audin::ClientPdu& b)
{
    if (a.index() != b.index()) {
        return false;
    }
    if (const auto* data = std::get_if<audin::Data>(&a)) {
        return std::ranges::equal(data->data, std::get<audin::Data>(b).data);
    }
    return std::visit(
        [&b](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, audin::Data>) {
                return true;  // handled above
            } else {
                return x == std::get<T>(b);
            }
        },
        a);
}

void check_round_trips(std::span<const std::byte> message)
{
    if (const auto pdu = audin::decode_client_pdu(message)) {
        const auto bytes = audin::encode_client_pdu(*pdu);
        const auto again = audin::decode_client_pdu(bytes);
        FARLAND_ASSERT(again.has_value() && same(*again, *pdu));
    }
    if (const auto pdu = audin::decode_server_pdu(message)) {
        const auto again = audin::decode_server_pdu(audin::encode_server_pdu(*pdu));
        FARLAND_ASSERT(again.has_value() && *again == *pdu);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    Reader r(std::as_bytes(std::span(data, size)));
    channels::AudinServer server(channels::AudinServerConfig{
        .formats = {rdpsnd::pcm_format(48000, 1), rdpsnd::pcm_format(44100, 2), rdpsnd::pcm_format(16000, 1)}});
    server.start();
    static_cast<void>(server.take_output());
    while (!r.empty()) {
        const auto length = r.u16le();
        if (!length) {
            break;
        }
        const auto message = r.bytes(std::min<std::size_t>(*length, r.remaining())).value();
        check_round_trips(message);
        static_cast<void>(server.receive(message));
        while (auto event = server.poll_event()) {
            if (const auto* d = std::get_if<channels::audin_event::Data>(&*event)) {
                FARLAND_ASSERT(d->data.size() <= message.size());
            }
        }
        for (const auto& out : server.take_output()) {
            FARLAND_ASSERT(audin::decode_server_pdu(out).has_value());
        }
    }
    return 0;
}
