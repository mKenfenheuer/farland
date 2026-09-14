// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Auto-detect and heartbeat messages ([MS-RDPBCGR] 2.2.14, 2.2.16). The first
// input byte picks the target: the request, response and heartbeat decoders
// (whose output must re-encode to the bytes they consumed), or a Connection
// in its connect-time detection that gets the rest as message channel PDUs,
// each prefixed with a length byte.

#include <farland/base/assert.hpp>
#include <farland/proto/autodetect.hpp>
#include <farland/proto/client_info.hpp>
#include <farland/proto/framing.hpp>
#include <farland/proto/gcc.hpp>
#include <farland/proto/mcs.hpp>
#include <farland/proto/security.hpp>
#include <farland/proto/x224.hpp>
#include <farland/server/connection.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace {

using namespace farland;
namespace ad = proto::autodetect;
namespace mcs = proto::mcs;
namespace gcc = proto::gcc;
using Bytes = std::vector<std::byte>;
// One static channel (1004), the message channel (1005), then the user (1006).
constexpr std::uint16_t message_channel = 1005;
constexpr std::uint16_t user_id = 1006;

template <class Decode, class Encode>
void round_trip(std::span<const std::byte> input, Decode decode, Encode encode)
{
    Reader r(input);
    const auto decoded = decode(r);
    if (!decoded) {
        return;
    }
    Writer w;
    encode(w, *decoded);
    FARLAND_ASSERT(std::ranges::equal(w.view(), input.first(r.position())));
}

template <class Pdu>
Bytes domain(const Pdu& pdu)
{
    Writer w;
    const auto start = proto::begin_data_tpdu(w);
    mcs::encode(w, pdu);
    proto::end_tpkt(w, start);
    return std::move(w).take();
}

void check_framing(std::span<const std::byte> out)
{
    while (!out.empty()) {
        const auto frame = proto::peek_frame(out);
        FARLAND_ASSERT(frame.has_value() && frame->has_value() && (*frame)->length <= out.size());
        out = out.subspan((*frame)->length);
    }
}

void fuzz_connection(std::span<const std::byte> fuzz)
{
    server::Connection c(server::ServerConfig{},
                         server::Negotiation{"", proto::protocol::ssl, proto::protocol::ssl, std::nullopt});
    auto now = server::Connection::Clock::time_point{} + std::chrono::hours(1);
    c.tick(now);

    gcc::ClientData data;
    data.core.desktop_width = 640;
    data.core.desktop_height = 480;
    data.core.post_beta2_color_depth = 0xCA01;
    data.core.client_product_id = 1;
    data.core.serial_number = 0;
    data.core.high_color_depth = 24;
    data.core.supported_color_depths = 0x0f;
    data.core.early_capability_flags = 0x0001 | gcc::cs_early_flags::support_netchar_autodetect;
    data.network = gcc::ClientNetworkData{{{"rdpdr", 0}}};
    data.message_channel = gcc::ClientMessageChannelData{0};
    Writer blocks;
    gcc::encode_client_data(blocks, data);
    Writer conference;
    gcc::encode_conference_create_request(conference, blocks.view());
    const std::array selector{std::byte{0x01}};
    mcs::ConnectInitial initial;
    initial.calling_domain_selector = selector;
    initial.called_domain_selector = selector;
    initial.user_data = conference.view();
    Writer connect;
    const auto start = proto::begin_data_tpdu(connect);
    mcs::encode_connect_initial(connect, initial);
    proto::end_tpkt(connect, start);
    c.receive(connect.view());
    c.receive(domain(mcs::ErectDomainRequest{}));
    c.receive(domain(mcs::AttachUserRequest{}));
    for (const std::uint16_t id : {user_id, mcs::io_channel_id, message_channel}) {
        c.receive(domain(mcs::ChannelJoinRequest{user_id, id}));
    }
    Writer info;
    proto::write_basic_security_header(info, proto::sec_flags::info_pkt);
    proto::encode_client_info(info, proto::ClientInfo{});
    c.receive(domain(mcs::SendDataRequest{user_id, mcs::io_channel_id, info.view()}));
    check_framing(c.take_output());
    FARLAND_ASSERT(c.state() == server::State::connect_time_autodetect);

    while (!fuzz.empty() && c.state() != server::State::closed) {
        const std::size_t length = std::min<std::size_t>(std::to_integer<std::size_t>(fuzz[0]), fuzz.size() - 1);
        const auto chunk = fuzz.subspan(1, length);
        fuzz = fuzz.subspan(1 + length);
        now += std::chrono::milliseconds(7);
        c.tick(now);
        c.receive(domain(mcs::SendDataRequest{user_id, message_channel, chunk}));
        check_framing(c.take_output());
        while (c.poll_event()) {
        }
    }
    static_cast<void>(c.network());
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0) {
        return 0;
    }
    const auto input = std::as_bytes(std::span(data, size));
    const auto fuzz = input.subspan(1);
    switch (std::to_integer<unsigned>(input[0]) % 4U) {
    case 0:
        round_trip(fuzz, ad::decode_request, [](Writer& w, const ad::Request& m) { ad::encode(w, m); });
        break;
    case 1:
        round_trip(fuzz, ad::decode_response, [](Writer& w, const ad::Response& m) { ad::encode(w, m); });
        break;
    case 2: {
        // The reserved byte is written as zero whatever came in.
        Reader r(fuzz);
        if (const auto heartbeat = ad::decode_heartbeat(r)) {
            Writer w;
            ad::encode(w, *heartbeat);
            FARLAND_ASSERT(std::ranges::equal(w.view().subspan(1), fuzz.subspan(1, 3)));
        }
        break;
    }
    default:
        fuzz_connection(fuzz);
        break;
    }
    return 0;
}
