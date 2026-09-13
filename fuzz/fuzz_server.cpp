// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The server state machines. The first input byte picks the stage the fuzz
// data meets: pre-authentication from the very first byte, the Connection
// after the MCS connect, or an active Connection. The server must never
// crash, whatever arrives, and its own output must stay well formed.

#include <farland/base/assert.hpp>
#include <farland/proto/client_info.hpp>
#include <farland/proto/framing.hpp>
#include <farland/proto/gcc.hpp>
#include <farland/proto/mcs.hpp>
#include <farland/proto/security.hpp>
#include <farland/proto/share.hpp>
#include <farland/proto/x224.hpp>
#include <farland/server/connection.hpp>
#include <farland/server/preauth.hpp>

#include "fuzz.hpp"

#include <array>
#include <span>

namespace {

using namespace farland;
namespace mcs = proto::mcs;
namespace gcc = proto::gcc;
namespace caps = proto::caps;
using Bytes = std::vector<std::byte>;
constexpr std::uint16_t user_id = 1005;  // one static channel: 1004, then the user channel

template <class Encode>
Bytes x224(Encode&& encode)
{
    Writer w;
    const auto start = proto::begin_data_tpdu(w);
    encode(w);
    proto::end_tpkt(w, start);
    return std::move(w).take();
}

template <class Pdu>
Bytes domain(const Pdu& pdu)
{
    return x224([&pdu](Writer& w) { mcs::encode(w, pdu); });
}

Bytes io(std::span<const std::byte> payload)
{
    return domain(mcs::SendDataRequest{user_id, mcs::io_channel_id, payload});
}

/// Everything the server sends must frame cleanly.
void check_framing(std::span<const std::byte> out)
{
    while (!out.empty()) {
        const auto frame = proto::peek_frame(out);
        FARLAND_ASSERT(frame.has_value() && frame->has_value() && (*frame)->length <= out.size());
        out = out.subspan((*frame)->length);
    }
}

void feed(server::Connection& c, std::span<const std::byte> bytes)
{
    c.receive(bytes);
    check_framing(c.take_output());
    while (c.poll_event()) {
    }
}

void connect_mcs(server::Connection& c)
{
    gcc::ClientData data;
    data.core.desktop_width = 640;
    data.core.desktop_height = 480;
    data.core.post_beta2_color_depth = 0xCA01;
    data.core.client_product_id = 1;
    data.core.serial_number = 0;
    data.core.high_color_depth = 24;
    data.core.supported_color_depths = 0x0f;
    data.core.early_capability_flags = 0x0001;
    data.network = gcc::ClientNetworkData{{{"rdpdr", 0}}};
    Writer blocks;
    gcc::encode_client_data(blocks, data);
    Writer conference;
    gcc::encode_conference_create_request(conference, blocks.view());
    const std::array selector{std::byte{0x01}};
    mcs::ConnectInitial initial;
    initial.calling_domain_selector = selector;
    initial.called_domain_selector = selector;
    initial.user_data = conference.view();
    feed(c, x224([&initial](Writer& w) { mcs::encode_connect_initial(w, initial); }));
    feed(c, domain(mcs::ErectDomainRequest{}));
    feed(c, domain(mcs::AttachUserRequest{}));
}

void activate(server::Connection& c)
{
    proto::ClientInfo info;
    Writer info_pdu;
    proto::write_basic_security_header(info_pdu, proto::sec_flags::info_pkt);
    proto::encode_client_info(info_pdu, info);
    feed(c, io(info_pdu.view()));

    const std::uint32_t share = c.session().share_id;
    proto::ConfirmActive confirm;
    confirm.share_id = share;
    confirm.capabilities.general = caps::General{};
    confirm.capabilities.general->extra_flags = caps::general_extra_flags::fastpath_output_supported;
    confirm.capabilities.bitmap = caps::Bitmap{};
    Writer confirm_pdu;
    proto::encode_confirm_active(confirm_pdu, user_id, confirm);
    feed(c, io(confirm_pdu.view()));

    const auto data_pdu = [&](const auto& pdu) {
        Writer payload;
        proto::encode(payload, pdu);
        Writer w;
        proto::write_data_pdu(w, share, user_id, proto::type2_of(proto::DataPdu{pdu}), payload.view());
        feed(c, io(w.view()));
    };
    data_pdu(proto::Synchronize{1, mcs::server_channel_id});
    data_pdu(proto::Control{proto::control_action::cooperate, 0, 0});
    data_pdu(proto::Control{proto::control_action::request_control, 0, 0});
    data_pdu(proto::FontList{});
    FARLAND_ASSERT(c.active());
}

/// Pre-authentication from the first byte, under a TLS-only policy (the NLA
/// acceptor has its own fuzz target).
void fuzz_preauth(std::span<const std::byte> fuzz)
{
    server::PreAuth p(server::PreAuthConfig{proto::protocol::ssl, false, false});
    const std::size_t half = fuzz.size() / 2;
    p.receive(fuzz.first(half));
    check_framing(p.take_output());
    if (p.state() == server::PreAuth::State::wait_tls) {
        p.tls_established();
    }
    p.receive(fuzz.subspan(half));
    static_cast<void>(p.take_output());
    while (p.poll_event()) {
    }
    static_cast<void>(p.take_remaining_input());
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0) {
        return 0;
    }
    const auto input = std::as_bytes(std::span(data, size));
    const auto stage = std::to_integer<unsigned>(input[0]) % 3U;
    const auto fuzz = input.subspan(1);
    if (stage == 0) {
        fuzz_preauth(fuzz);
        return 0;
    }

    server::Connection c(server::ServerConfig{},
                         server::Negotiation{"", proto::protocol::ssl, proto::protocol::ssl, std::nullopt});
    connect_mcs(c);
    if (stage >= 2) {
        activate(c);
    }
    // Feed the rest in two slices so partial-PDU buffering is exercised too.
    const std::size_t half = fuzz.size() / 2;
    feed(c, fuzz.first(half));
    feed(c, fuzz.subspan(half));
    if (c.active()) {
        const std::array update{std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
        c.send_bitmap_update(update);
        c.reactivate(800, 600);
        c.disconnect();
        static_cast<void>(c.take_output());
    }
    return 0;
}
