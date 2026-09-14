// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// [MS-RDPBCGR] 4.1.3 - 4.2.2 and 4.7: connection data, Client Info,
// licensing, capability exchange, finalization and input. The encrypted
// originals were rebuilt as TLS-mode PDUs (tests/data/spec/*-decrypted.txt).

#include <farland/base/hexdump.hpp>
#include <farland/proto/client_info.hpp>
#include <farland/proto/gcc.hpp>
#include <farland/proto/input.hpp>
#include <farland/proto/license.hpp>
#include <farland/proto/mcs.hpp>
#include <farland/proto/security.hpp>
#include <farland/proto/share.hpp>
#include <farland/proto/x224.hpp>

#include "support/transcript.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace proto = farland::proto;
namespace mcs = farland::proto::mcs;
namespace gcc = farland::proto::gcc;
using farland::Reader;
using farland::to_hex;
using farland::Writer;

namespace {

std::vector<std::byte> spec(const std::string& name)
{
    const auto records = farland::test::load_transcript(std::string(FARLAND_TEST_DATA_DIR) + "/spec/" + name);
    REQUIRE(records.size() == 1);
    return records[0].bytes;
}

/// The user data of the X.224 Data TPDU in a TPKT.
std::vector<std::byte> x224_payload(std::span<const std::byte> packet)
{
    Reader r(packet);
    Reader tpdu = proto::read_tpkt(r).value();
    Reader data = proto::decode_data_tpdu(tpdu).value();
    return {data.rest().begin(), data.rest().end()};
}

struct McsData {
    std::uint16_t initiator = 0;
    std::uint16_t channel = 0;
    std::vector<std::byte> bytes;
};

McsData mcs_payload(std::span<const std::byte> packet)
{
    const auto x224 = x224_payload(packet);
    Reader r(x224);
    const auto pdu = mcs::decode_domain_pdu(r).value();
    McsData out;
    if (const auto* request = std::get_if<mcs::SendDataRequest>(&pdu)) {
        out = {request->initiator, request->channel_id, {request->data.begin(), request->data.end()}};
    } else {
        const auto& indication = std::get<mcs::SendDataIndication>(pdu);
        out = {indication.initiator, indication.channel_id, {indication.data.begin(), indication.data.end()}};
    }
    return out;
}

/// Decodes a Data PDU carried in a spec file.
proto::DataPdu data_pdu(const std::string& name, std::uint16_t expected_source,
                        std::uint32_t expected_share_id = 0x000103ea)
{
    const auto payload = mcs_payload(spec(name));
    CHECK(payload.channel == mcs::io_channel_id);
    Reader r(payload.bytes);
    auto control = proto::read_share_control(r).value();
    CHECK(control.type == proto::pdu_type::data);
    CHECK(control.source == expected_source);
    auto data = proto::read_share_data(control.body).value();
    CHECK(data.share_id == expected_share_id);
    return proto::decode_data_pdu(data).value();
}

}  // namespace

TEST_CASE("4.1.3 Client MCS Connect Initial with GCC Conference Create Request")
{
    const auto payload = x224_payload(spec("bcgr-4.1.3-mcs-connect-initial.txt"));
    Reader r(payload);
    const auto initial = mcs::decode_connect_initial(r).value();
    CHECK(initial.upward_flag);
    CHECK(initial.target == mcs::DomainParameters{34, 2, 0, 1, 0, 1, 65535, 2});
    CHECK(initial.minimum == mcs::DomainParameters{1, 1, 1, 1, 0, 1, 1056, 2});
    CHECK(initial.maximum == mcs::DomainParameters{65535, 64535, 65535, 1, 0, 1, 65535, 2});
    CHECK(mcs::negotiate_domain_parameters(initial) == mcs::DomainParameters{34, 3, 0, 1, 0, 1, 65528, 2});

    Reader user_data(initial.user_data);
    const auto blocks = gcc::decode_conference_create_request(user_data).value();
    Reader block_reader(blocks);
    const auto client = gcc::decode_client_data(block_reader).value();
    const auto& core = client.core;
    CHECK(core.version == gcc::rdp_version_5_plus);
    CHECK(core.desktop_width == 1280);
    CHECK(core.desktop_height == 1024);
    CHECK(core.keyboard_layout == 0x409);
    CHECK(core.client_build == 3790);
    CHECK(core.client_name == "ELTONS-DEV2");
    CHECK(core.keyboard_type == 4);
    CHECK(core.keyboard_function_keys == 12);
    CHECK(core.high_color_depth == 24);
    CHECK(core.supported_color_depths == 0x07);
    CHECK(core.early_capability_flags == 0x0001);
    CHECK(core.connection_type == 0);
    CHECK(core.server_selected_protocol == proto::protocol::rdp);
    CHECK_FALSE(core.desktop_physical_width.has_value());

    REQUIRE(client.cluster.has_value());
    CHECK(client.cluster->flags == 0x0d);
    REQUIRE(client.security.has_value());
    CHECK(client.security->encryption_methods == 0x1b);
    REQUIRE(client.network.has_value());
    REQUIRE(client.network->channels.size() == 3);
    CHECK(client.network->channels[0].name == "rdpdr");
    CHECK(client.network->channels[0].options == 0x80800000);
    CHECK(client.network->channels[1].name == "cliprdr");
    CHECK(client.network->channels[1].options == 0xc0a00000);
    CHECK(client.network->channels[2].name == "rdpsnd");
    CHECK(client.network->channels[2].options == 0xc0000000);
}

TEST_CASE("4.1.4 Server MCS Connect Response with GCC Conference Create Response")
{
    const auto payload = x224_payload(spec("bcgr-4.1.4-mcs-connect-response.txt"));
    Reader r(payload);
    const auto response = mcs::decode_connect_response(r).value();
    CHECK(response.result == mcs::ResultCode::successful);
    CHECK(response.parameters == mcs::DomainParameters{34, 3, 0, 1, 0, 1, 65528, 2});

    // The connectPDU length says 0x2a although 279 bytes follow.
    Reader user_data(response.user_data);
    const auto blocks = gcc::decode_conference_create_response(user_data).value();
    Reader block_reader(blocks);
    const auto server = gcc::decode_server_data(block_reader).value();
    CHECK(server.core.version == gcc::rdp_version_5_plus);
    CHECK(server.core.client_requested_protocols == proto::protocol::rdp);
    CHECK_FALSE(server.core.early_capability_flags.has_value());
    CHECK(server.network.io_channel_id == mcs::io_channel_id);
    CHECK(server.network.channel_ids == std::vector<std::uint16_t>{1004, 1005, 1006});
    CHECK(server.security.encryption_method == 2);
    CHECK(server.security.encryption_level == 2);
}

TEST_CASE("4.1.10 Client Info PDU")
{
    const auto payload = mcs_payload(spec("bcgr-4.1.10-client-info-decrypted.txt"));
    CHECK(payload.initiator == 1007);
    CHECK(payload.channel == mcs::io_channel_id);
    Reader r(payload.bytes);
    const auto flags = proto::read_basic_security_header(r).value();
    CHECK(flags == proto::sec_flags::info_pkt);
    const auto info = proto::decode_client_info(r).value();
    CHECK(info.code_page == 0x04090409);
    CHECK(info.flags == 0x000043b3);
    CHECK(info.domain == "NTDEV");
    CHECK(info.user_name == "eltons");
    CHECK(info.password.empty());
    CHECK(info.alternate_shell.empty());
    REQUIRE(info.extended.has_value());
    CHECK(info.extended->client_address_family == 2);
    CHECK(info.extended->client_address == "157.59.242.156");
    CHECK(info.extended->client_dir.find("mstscax.dll") != std::string::npos);
    REQUIRE(info.extended->time_zone.has_value());
    CHECK(info.extended->time_zone->bias == 480);
    CHECK(info.extended->time_zone->standard_name == "Pacific Standard Time");
    CHECK(info.extended->time_zone->daylight_name == "Pacific Daylight Time");
    CHECK(info.extended->session_id == 0);
    CHECK(info.extended->performance_flags == 1);
    CHECK_FALSE(info.extended->auto_reconnect_cookie.has_value());

    // Our encoder reproduces the TS_INFO_PACKET exactly.
    Writer w;
    proto::write_basic_security_header(w, proto::sec_flags::info_pkt);
    proto::encode_client_info(w, info);
    CHECK(to_hex(w.view()) == to_hex(payload.bytes));
}

TEST_CASE("4.1.11 Server License Error PDU - Valid Client")
{
    const auto payload = mcs_payload(spec("bcgr-4.1.11-license-error-valid-client-decrypted.txt"));
    CHECK(payload.initiator == mcs::server_channel_id);
    Reader r(payload.bytes);
    CHECK(proto::decode_license_valid_client(r).has_value());

    Writer w;
    proto::encode_license_valid_client(w);
    CHECK(to_hex(w.view()) == to_hex(payload.bytes));
}

TEST_CASE("4.1.12 Server Demand Active PDU")
{
    const auto payload = mcs_payload(spec("bcgr-4.1.12-demand-active-decrypted.txt"));
    Reader r(payload.bytes);
    auto control = proto::read_share_control(r).value();
    CHECK(control.type == proto::pdu_type::demand_active);
    CHECK(control.source == mcs::server_channel_id);
    const auto pdu = proto::decode_demand_active(control.body).value();
    CHECK(pdu.share_id == 0x000103ea);
    CHECK(pdu.source_descriptor == "RDP");
    CHECK(pdu.session_id == 0);
    const auto& caps = pdu.capabilities;
    CHECK(caps.general.has_value());
    CHECK(caps.bitmap.has_value());
    CHECK(caps.order.has_value());
    REQUIRE(caps.pointer.has_value());
    CHECK(caps.pointer->color_pointer_cache_size == 25);  // farland's server offers the same
    CHECK(caps.pointer->pointer_cache_size == 25);
    CHECK(caps.input.has_value());
    CHECK(caps.virtual_channel.has_value());
    CHECK(caps.font.has_value());  // length 4, empty body
    CHECK(caps.color_cache.has_value());
    REQUIRE(caps.share.has_value());
    CHECK(caps.share->node_id == 0x03ea);
    CHECK(caps.other.size() == 4);  // DrawGdiPlus, BitmapCacheHostSupport, Rail, Window
}

TEST_CASE("4.1.13 Client Confirm Active PDU")
{
    const auto payload = mcs_payload(spec("bcgr-4.1.13-confirm-active-decrypted.txt"));
    Reader r(payload.bytes);
    auto control = proto::read_share_control(r).value();
    CHECK(control.type == proto::pdu_type::confirm_active);
    CHECK(control.source == 1007);
    const auto pdu = proto::decode_confirm_active(control.body).value();
    CHECK(pdu.share_id == 0x000103ea);
    CHECK(pdu.originator_id == mcs::server_channel_id);
    CHECK(pdu.source_descriptor == "MSTSC");
    const auto& caps = pdu.capabilities;
    REQUIRE(caps.general.has_value());
    REQUIRE(caps.bitmap.has_value());
    CHECK(caps.order.has_value());
    REQUIRE(caps.pointer.has_value());
    CHECK(caps.pointer->color_pointer_cache_size == 20);
    CHECK(caps.pointer->pointer_cache_size == 21);
    CHECK(caps.share.has_value());
    CHECK(caps.input.has_value());
    CHECK(caps.font.has_value());
    CHECK(caps.color_cache.has_value());
    CHECK(caps.virtual_channel.has_value());
    CHECK(caps.other.size() == 9);

    // Re-encoding and decoding again keeps every decoded value.
    Writer w;
    proto::encode_confirm_active(w, 1007, pdu);
    Reader again(w.view());
    auto control2 = proto::read_share_control(again).value();
    const auto pdu2 = proto::decode_confirm_active(control2.body).value();
    CHECK(pdu2.capabilities.general->extra_flags == caps.general->extra_flags);
    CHECK(pdu2.capabilities.bitmap->desktop_width == caps.bitmap->desktop_width);
    CHECK(pdu2.capabilities.input->input_flags == caps.input->input_flags);
    CHECK(pdu2.capabilities.other.size() == 9);
}

TEST_CASE("4.1.14 - 4.1.22 Finalization PDUs")
{
    const auto sync = std::get<proto::Synchronize>(data_pdu("bcgr-4.1.14-client-synchronize-decrypted.txt", 1007));
    CHECK(sync.target_user == mcs::server_channel_id);
    const auto cooperate =
        std::get<proto::Control>(data_pdu("bcgr-4.1.15-client-control-cooperate-decrypted.txt", 1007));
    CHECK(cooperate.action == proto::control_action::cooperate);
    const auto request = std::get<proto::Control>(data_pdu("bcgr-4.1.16-client-control-request-decrypted.txt", 1007));
    CHECK(request.action == proto::control_action::request_control);
    const auto fonts = std::get<proto::FontList>(data_pdu("bcgr-4.1.18-font-list-decrypted.txt", 1007));
    CHECK(fonts.list_flags == 0x0003);
    CHECK(fonts.entry_size == 0x0032);
    CHECK(std::holds_alternative<proto::OtherDataPdu>(data_pdu("bcgr-4.1.17-persistent-key-list-decrypted.txt", 1007)));

    CHECK(std::holds_alternative<proto::Synchronize>(
        data_pdu("bcgr-4.1.19-server-synchronize-decrypted.txt", mcs::server_channel_id)));
    const auto server_cooperate = std::get<proto::Control>(
        data_pdu("bcgr-4.1.20-server-control-cooperate-decrypted.txt", mcs::server_channel_id));
    CHECK(server_cooperate.action == proto::control_action::cooperate);
    const auto granted =
        std::get<proto::Control>(data_pdu("bcgr-4.1.21-server-control-granted-decrypted.txt", mcs::server_channel_id));
    CHECK(granted.action == proto::control_action::granted_control);
    CHECK(granted.grant_id == 1007);
    CHECK(granted.control_id == mcs::server_channel_id);
    const auto map = std::get<proto::FontMap>(data_pdu("bcgr-4.1.22-font-map-decrypted.txt", mcs::server_channel_id));
    CHECK(map.map_flags == 0x0003);
    CHECK(map.entry_size == 0x0004);
}

TEST_CASE("Client finalization PDUs encode exactly as in the specification")
{
    const auto encode = [](const auto& pdu) {
        Writer payload;
        proto::encode(payload, pdu);
        Writer w;
        proto::write_data_pdu(w, 0x000103ea, 1007, proto::type2_of(proto::DataPdu{pdu}), payload.view());
        return to_hex(w.view());
    };
    CHECK(encode(proto::Synchronize{1, mcs::server_channel_id}) ==
          to_hex(mcs_payload(spec("bcgr-4.1.14-client-synchronize-decrypted.txt")).bytes));
    CHECK(encode(proto::Control{proto::control_action::cooperate, 0, 0}) ==
          to_hex(mcs_payload(spec("bcgr-4.1.15-client-control-cooperate-decrypted.txt")).bytes));
    CHECK(encode(proto::Control{proto::control_action::request_control, 0, 0}) ==
          to_hex(mcs_payload(spec("bcgr-4.1.16-client-control-request-decrypted.txt")).bytes));
}

TEST_CASE("4.2.1 and 4.2.2 Shutdown Request and Denied")
{
    CHECK(std::holds_alternative<proto::ShutdownRequest>(
        data_pdu("bcgr-4.2.1-shutdown-request-decrypted.txt", 1007, 0x000203ea)));
}

TEST_CASE("4.7 Fast-path input")
{
    const auto bytes = spec("bcgr-4.7-fast-path-input-decrypted.txt");
    Reader r(bytes);
    const auto events = proto::decode_fastpath_input(r).value();
    REQUIRE(events.size() == 1);
    const auto mouse = std::get<proto::MouseEvent>(events[0]);
    CHECK(mouse.flags == proto::ptr_flags::move);
    CHECK(mouse.x == 683);
    CHECK(mouse.y == 367);
}
