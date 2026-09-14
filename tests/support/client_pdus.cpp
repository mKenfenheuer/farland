// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "support/client_pdus.hpp"

#include <farland/proto/client_info.hpp>
#include <farland/proto/input.hpp>
#include <farland/proto/mcs.hpp>
#include <farland/proto/security.hpp>
#include <farland/proto/x224.hpp>

#include <array>

namespace farland::test::client {

namespace {

namespace mcs = proto::mcs;
namespace caps = proto::caps;

template <class Encode>
Bytes x224_data(Encode&& encode)
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
    return x224_data([&pdu](Writer& w) { mcs::encode(w, pdu); });
}

void append(Bytes& to, const Bytes& more)
{
    to.insert(to.end(), more.begin(), more.end());
}

}  // namespace

Bytes connection_request(std::uint32_t protocols)
{
    proto::ConnectionRequest request;
    request.cookie = "tester";
    request.negotiation = proto::ConnectionRequest::Negotiation{0, protocols};
    Writer w;
    proto::encode_connection_request(w, request);
    return std::move(w).take();
}

proto::gcc::ClientData client_data(std::uint32_t selected_protocol, std::uint16_t early_flags, std::uint16_t width,
                                   std::uint16_t height)
{
    proto::gcc::ClientData data;
    data.core.desktop_width = width;
    data.core.desktop_height = height;
    data.core.client_name = "test-client";
    data.core.post_beta2_color_depth = 0xCA01;
    data.core.client_product_id = 1;
    data.core.serial_number = 0;
    data.core.high_color_depth = 24;
    data.core.supported_color_depths = 0x0f;
    data.core.early_capability_flags = early_flags;
    data.core.client_dig_product_id = "";
    data.core.connection_type = 6;
    data.core.server_selected_protocol = selected_protocol;
    data.network = proto::gcc::ClientNetworkData{{{"rdpdr", 0x80800000}, {"cliprdr", 0xc0a00000}}};
    return data;
}

Bytes connect_initial(const proto::gcc::ClientData& data)
{
    Writer blocks;
    proto::gcc::encode_client_data(blocks, data);
    Writer conference;
    proto::gcc::encode_conference_create_request(conference, blocks.view());
    const std::array selector{std::byte{0x01}};
    mcs::ConnectInitial initial;
    initial.calling_domain_selector = selector;
    initial.called_domain_selector = selector;
    initial.target = {34, 2, 0, 1, 0, 1, 65535, 2};
    initial.minimum = {1, 1, 1, 1, 0, 1, 1056, 2};
    initial.maximum = {65535, 64535, 65535, 1, 0, 1, 65535, 2};
    initial.user_data = conference.view();
    return x224_data([&initial](Writer& w) { mcs::encode_connect_initial(w, initial); });
}

Bytes erect_domain()
{
    return domain(mcs::ErectDomainRequest{});
}

Bytes attach_user()
{
    return domain(mcs::AttachUserRequest{});
}

Bytes channel_join(std::uint16_t user, std::uint16_t channel)
{
    return domain(mcs::ChannelJoinRequest{user, channel});
}

Bytes io(std::uint16_t user, std::span<const std::byte> payload)
{
    return domain(mcs::SendDataRequest{user, mcs::io_channel_id, payload});
}

Bytes client_info(std::uint16_t user, std::string_view user_name)
{
    proto::ClientInfo info;
    info.flags = proto::info_flags::unicode | proto::info_flags::mouse;
    info.user_name = user_name;
    Writer w;
    proto::write_basic_security_header(w, proto::sec_flags::info_pkt);
    proto::encode_client_info(w, info);
    return io(user, w.view());
}

Bytes confirm_active(std::uint16_t user, std::uint32_t share_id, bool fastpath_output, std::uint16_t width,
                     std::uint16_t height)
{
    proto::ConfirmActive confirm;
    confirm.share_id = share_id;
    auto& sets = confirm.capabilities;
    sets.general = caps::General{};
    sets.general->extra_flags = caps::general_extra_flags::no_bitmap_compression_hdr |
                                (fastpath_output ? caps::general_extra_flags::fastpath_output_supported : 0U);
    sets.bitmap = caps::Bitmap{};
    sets.bitmap->desktop_width = width;
    sets.bitmap->desktop_height = height;
    sets.input = caps::Input{};
    sets.input->input_flags = caps::input_flags::scancodes | caps::input_flags::fastpath_input2;
    sets.multifragment_update = caps::MultifragmentUpdate{0x100000};
    sets.pointer = caps::Pointer{};  // 32 bpp pointers and a 25-entry cache, as mstsc and FreeRDP send
    Writer w;
    proto::encode_confirm_active(w, user, confirm);
    return io(user, w.view());
}

Bytes data_pdu(std::uint16_t user, std::uint32_t share_id, const proto::DataPdu& pdu)
{
    Writer payload;
    std::visit(
        [&payload](const auto& p) {
            if constexpr (requires { proto::encode(payload, p); }) {
                proto::encode(payload, p);
            }
        },
        pdu);
    Writer w;
    proto::write_data_pdu(w, share_id, user, proto::type2_of(pdu), payload.view());
    return io(user, w.view());
}

Bytes finalization(std::uint16_t user, std::uint32_t share_id)
{
    Bytes out;
    append(out, data_pdu(user, share_id, proto::Synchronize{1, mcs::server_channel_id}));
    append(out, data_pdu(user, share_id, proto::Control{proto::control_action::cooperate, 0, 0}));
    append(out, data_pdu(user, share_id, proto::Control{proto::control_action::request_control, 0, 0}));
    append(out, data_pdu(user, share_id, proto::FontList{}));
    return out;
}

Bytes fastpath_input(std::span<const proto::InputEvent> events)
{
    Writer w;
    proto::encode_fastpath_input(w, events);
    return std::move(w).take();
}

void request_autodetect(proto::gcc::ClientData& data)
{
    data.core.early_capability_flags = static_cast<std::uint16_t>(
        data.core.early_capability_flags.value_or(0) | proto::gcc::cs_early_flags::support_netchar_autodetect |
        proto::gcc::cs_early_flags::support_heartbeat_pdu);
    data.message_channel = proto::gcc::ClientMessageChannelData{0};
}

std::optional<std::uint16_t> message_channel_id(std::span<const std::byte> connect_response)
{
    Reader r(connect_response);
    Reader tpdu = proto::read_tpkt(r).value();
    Reader data = proto::decode_data_tpdu(tpdu).value();
    const auto response = mcs::decode_connect_response(data).value();
    Reader user_data(response.user_data);
    Reader blocks(proto::gcc::decode_conference_create_response(user_data).value());
    return proto::gcc::decode_server_data(blocks).value().message_channel_id;
}

bool AutoDetectResponder::on_message_channel(std::span<const std::byte> pdu) const
{
    if (pdu.empty() || std::to_integer<unsigned>(pdu[0]) != 0x03) {
        return false;
    }
    Reader r(pdu);
    Reader tpdu = proto::read_tpkt(r).value();
    auto data = proto::decode_data_tpdu(tpdu);
    if (!data) {
        return false;
    }
    const auto domain_pdu = mcs::decode_domain_pdu(*data);
    const auto* send = domain_pdu ? std::get_if<mcs::SendDataIndication>(&*domain_pdu) : nullptr;
    return send != nullptr && send->channel_id == message_channel_;
}

std::optional<Bytes> AutoDetectResponder::answer(std::span<const std::byte> pdu, Clock::time_point now)
{
    namespace ad = proto::autodetect;
    if (!on_message_channel(pdu)) {
        if (measuring_) {
            measured_bytes_ += pdu.size();  // as FreeRDP counts: every PDU in between
        }
        return std::nullopt;
    }
    Reader r(pdu);
    Reader tpdu = proto::read_tpkt(r).value();
    Reader data = proto::decode_data_tpdu(tpdu).value();
    const auto domain_pdu = mcs::decode_domain_pdu(data).value();
    Reader payload(std::get<mcs::SendDataIndication>(domain_pdu).data);
    const std::uint16_t flags = proto::read_basic_security_header(payload).value();
    if ((flags & proto::sec_flags::heartbeat) != 0) {
        ++heartbeats;
        return std::nullopt;
    }
    if ((flags & proto::sec_flags::autodetect_req) == 0) {
        return std::nullopt;
    }
    const auto request = ad::decode_request(payload).value();
    if (const auto* rtt = std::get_if<ad::RttRequest>(&request)) {
        ++rtt_answers;
        return response(ad::RttResponse{rtt->sequence});
    }
    if (std::holds_alternative<ad::BandwidthStart>(request)) {
        measuring_ = true;
        measure_start_ = now;
        measured_bytes_ = 0;
        return std::nullopt;
    }
    if (const auto* p = std::get_if<ad::BandwidthPayload>(&request)) {
        measured_bytes_ += p->payload.size();
        return std::nullopt;
    }
    if (const auto* stop = std::get_if<ad::BandwidthStop>(&request)) {
        measured_bytes_ += stop->payload.size();
        measuring_ = false;
        const bool connect_time = stop->request_type == ad::request_type::bw_stop_connect_time;
        ++(connect_time ? connect_time_results : continuous_results);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - measure_start_).count();
        return response(ad::BandwidthResults{
            stop->sequence,
            connect_time ? ad::response_type::bw_results_connect_time : ad::response_type::bw_results_continuous,
            static_cast<std::uint32_t>(elapsed), static_cast<std::uint32_t>(measured_bytes_)});
    }
    result = std::get<ad::NetworkCharacteristicsResult>(request);
    return std::nullopt;
}

Bytes AutoDetectResponder::response(const proto::autodetect::Response& response) const
{
    Writer payload;
    proto::write_basic_security_header(payload, proto::sec_flags::autodetect_rsp);
    proto::autodetect::encode(payload, response);
    return domain(mcs::SendDataRequest{user_, message_channel_, payload.view()});
}

}  // namespace farland::test::client
