// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// drdynvc. The first byte picks the mode:
// - bit 0 clear: the rest is one message, decoded as client and as server
//   PDU; whatever decodes must re-encode to an equal PDU.
// - bit 0 set: client-to-server messages for a DvcServer that finished the
//   capabilities exchange (client version from bits 1-2) with three
//   channels (channel 2 refused when bit 3 is set). The rest is records of
//   [u16le length][message]. Complete messages are echoed back with send(),
//   and every output PDU must decode and stay within 1600 bytes.

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/channels/drdynvc.hpp>
#include <farland/channels/dvc_server.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <span>
#include <variant>
#include <vector>

using namespace farland;
using namespace farland::channels;
namespace dyn = farland::channels::drdynvc;

namespace {

template <class Pdu, class Encode, class Decode>
void check_codec(const Result<Pdu>& decoded, Encode encode, Decode decode)
{
    if (!decoded.has_value()) {
        return;
    }
    const auto bytes = encode(*decoded);
    const auto again = decode(bytes);
    FARLAND_ASSERT(again.has_value() && *again == *decoded);
}

void codec_mode(std::span<const std::byte> input)
{
    check_codec(dyn::decode_client_pdu(input), dyn::encode_client_pdu, dyn::decode_client_pdu);
    check_codec(dyn::decode_server_pdu(input), dyn::encode_server_pdu, dyn::decode_server_pdu);
}

void check_output(DvcServer& server)
{
    for (const auto& message : server.take_output()) {
        FARLAND_ASSERT(message.size() <= dyn::max_pdu_size);
        FARLAND_ASSERT(dyn::decode_server_pdu(message).has_value());
    }
}

void server_mode(std::uint8_t mode, Reader& r)
{
    DvcServerConfig config;
    config.max_message_size = std::size_t{256} * 1024;
    // A stand-in for RDP 8.0 Lite: the block minus its first byte, and an
    // error for blocks that start with 0xFF.
    config.make_decompressor = []() -> DvcDecompressor {
        return [](std::span<const std::byte> block) -> Result<std::vector<std::byte>> {
            if (block.empty() || block.front() == std::byte{0xFF}) {
                return fail(Errc::invalid_value, "bad block");
            }
            return std::vector<std::byte>(block.begin() + 1, block.end());
        };
    };
    DvcServer server(config);
    server.start();
    const auto version = static_cast<std::uint16_t>(((mode >> 1U) & 0x03U) + 1U);
    Writer caps;
    dyn::encode(caps, dyn::CapsResponse{version});
    FARLAND_ASSERT(server.receive(caps.view()).has_value());
    for (std::uint32_t i = 0; i < 3; ++i) {
        const auto id = server.open("channel");
        const std::int32_t status = (i == 1 && (mode & 0x08U) != 0) ? -1 : 0;
        Writer rsp;
        dyn::encode(rsp, dyn::CreateResponse{id, status});
        FARLAND_ASSERT(server.receive(rsp.view()).has_value());
    }
    // A fourth channel stays in "opening".
    static_cast<void>(server.open("opening"));
    check_output(server);

    while (!r.empty()) {
        const auto length = r.u16le();
        if (!length) {
            break;
        }
        const auto message = r.bytes(std::min<std::size_t>(*length, r.remaining())).value();
        if (!server.receive(message).has_value()) {
            FARLAND_ASSERT(server.failed());
            break;
        }
        while (auto event = server.poll_event()) {
            if (const auto* data = std::get_if<dvc_event::ChannelData>(&*event)) {
                FARLAND_ASSERT(server.send(data->id, data->data));
            } else if (const auto* closed = std::get_if<dvc_event::ChannelClosed>(&*event)) {
                FARLAND_ASSERT(!server.is_open(closed->id));
            }
        }
        check_output(server);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    Reader r(std::as_bytes(std::span(data, size)));
    const auto mode = r.u8();
    if (!mode) {
        return 0;
    }
    if ((*mode & 0x01U) == 0) {
        codec_mode(r.rest());
    } else {
        server_mode(*mode, r);
    }
    return 0;
}
