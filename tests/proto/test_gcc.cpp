// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/hexdump.hpp>
#include <farland/proto/gcc.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

namespace gcc = farland::proto::gcc;
using farland::Errc;
using farland::Reader;
using farland::to_hex;
using farland::Writer;
using farland::test::hex;

namespace {

gcc::ClientData full_client_data()
{
    gcc::ClientData data;
    auto& core = data.core;
    core.desktop_width = 1920;
    core.desktop_height = 1080;
    core.keyboard_layout = 0x00000407;
    core.client_build = 26100;
    core.client_name = "WORKSTATION-7";
    core.post_beta2_color_depth = 0xCA01;
    core.client_product_id = 1;
    core.serial_number = 0;
    core.high_color_depth = 24;
    core.supported_color_depths = gcc::color_depth_support::bpp32 | gcc::color_depth_support::bpp24;
    core.early_capability_flags = gcc::cs_early_flags::support_errinfo_pdu | gcc::cs_early_flags::want_32bpp_session;
    core.client_dig_product_id = "0123456789";
    core.connection_type = 6;
    core.server_selected_protocol = 1;
    core.desktop_physical_width = 600;
    core.desktop_physical_height = 340;
    core.desktop_orientation = 0;
    core.desktop_scale_factor = 150;
    core.device_scale_factor = 140;

    data.security = gcc::ClientSecurityData{0x1b, 0};
    data.network = gcc::ClientNetworkData{{{"rdpdr", 0x80800000}, {"rdpsnd", 0xc0000000}, {"drdynvc", 0xc0800000}}};
    data.cluster = gcc::ClientClusterData{0x0d, 0};
    data.monitor = gcc::ClientMonitorData{0, {{0, 0, 1919, 1079, 1}, {1920, 0, 3839, 1079, 0}}};
    data.message_channel = gcc::ClientMessageChannelData{0};
    data.monitor_ex = gcc::ClientMonitorExtendedData{0, {{600, 340, 0, 150, 140}, {600, 340, 0, 100, 100}}};
    data.multitransport = gcc::ClientMultitransportData{0x0301};
    return data;
}

gcc::ClientData round_trip(const gcc::ClientData& data)
{
    Writer w;
    gcc::encode_client_data(w, data);
    Reader r(w.view());
    return gcc::decode_client_data(r).value();
}

}  // namespace

TEST_CASE("Client data blocks round-trip ([MS-RDPBCGR] 2.2.1.3)")
{
    const auto data = full_client_data();
    const auto decoded = round_trip(data);

    const auto& core = decoded.core;
    CHECK(core.desktop_width == 1920);
    CHECK(core.desktop_height == 1080);
    CHECK(core.keyboard_layout == 0x407);
    CHECK(core.client_name == "WORKSTATION-7");
    CHECK(core.high_color_depth == 24);
    CHECK(core.has_early_flag(gcc::cs_early_flags::want_32bpp_session));
    CHECK_FALSE(core.has_early_flag(gcc::cs_early_flags::support_heartbeat_pdu));
    CHECK(core.client_dig_product_id == "0123456789");
    CHECK(core.connection_type == 6);
    CHECK(core.server_selected_protocol == 1);
    CHECK(core.desktop_scale_factor == 150);
    CHECK(core.device_scale_factor == 140);

    REQUIRE(decoded.network.has_value());
    REQUIRE(decoded.network->channels.size() == 3);
    CHECK(decoded.network->channels[2].name == "drdynvc");
    CHECK(decoded.network->channels[2].options == 0xc0800000);
    REQUIRE(decoded.monitor.has_value());
    CHECK(decoded.monitor->monitors[1].left == 1920);
    CHECK(decoded.monitor->monitors[1].right == 3839);
    REQUIRE(decoded.monitor_ex.has_value());
    CHECK(decoded.monitor_ex->monitors[0].desktop_scale_factor == 150);
    CHECK(decoded.cluster->flags == 0x0d);
    CHECK(decoded.security->encryption_methods == 0x1b);
    CHECK(decoded.message_channel.has_value());
    CHECK(decoded.multitransport->flags == 0x0301);
}

TEST_CASE("Client core data stops at the first absent optional field")
{
    gcc::ClientData data;
    data.core.desktop_width = 1024;
    data.core.desktop_height = 768;
    data.core.client_name = "x";

    Writer minimal;
    gcc::encode_client_data(minimal, data);
    CHECK(minimal.size() == 4 + 128);  // TS_UD_CS_CORE with only the mandatory fields
    const auto decoded = round_trip(data);
    CHECK_FALSE(decoded.core.post_beta2_color_depth.has_value());
    CHECK_FALSE(decoded.core.early_capability_flags.has_value());

    data.core.post_beta2_color_depth = 0xCA01;
    data.core.client_product_id = 1;
    data.core.serial_number = 0;
    data.core.high_color_depth = 16;
    data.core.supported_color_depths = 0x0f;
    data.core.early_capability_flags = 0x0001;
    const auto partial = round_trip(data);
    CHECK(partial.core.early_capability_flags == 0x0001);
    CHECK_FALSE(partial.core.client_dig_product_id.has_value());
    CHECK_FALSE(partial.core.server_selected_protocol.has_value());
}

TEST_CASE("Malformed client data is rejected")
{
    const auto check = [](std::string_view text, Errc expected) {
        const auto bytes = hex(text);
        Reader r(bytes);
        const auto data = gcc::decode_client_data(r);
        REQUIRE_FALSE(data.has_value());
        CHECK(data.error().code == expected);
    };
    check("01 c0 02 00", Errc::invalid_length);                          // block shorter than its header
    check("01 c0 10 00 00 00", Errc::truncated);                         // block longer than the data
    check("02 c0 0c 00 00 00 00 00 00 00 00 00", Errc::invalid_value);   // no core block
    check("03 c0 0c 00 20 00 00 00 00 00 00 00", Errc::limit_exceeded);  // 32 channels

    // A core block cut in the middle of an optional field.
    gcc::ClientData data;
    data.core.post_beta2_color_depth = 0xCA01;
    Writer w;
    gcc::encode_client_data(w, data);
    auto bytes = std::move(w).take();
    bytes.push_back(std::byte{0x01});  // one byte of clientProductId
    bytes.at(2) = static_cast<std::byte>(bytes.size());
    Reader r(bytes);
    CHECK(gcc::decode_client_data(r).error().code == Errc::truncated);
}

TEST_CASE("Server data blocks round-trip ([MS-RDPBCGR] 2.2.1.4)")
{
    gcc::ServerData data;
    data.core.client_requested_protocols = 0x0b;
    data.core.early_capability_flags = gcc::sc_early_flags::skip_channeljoin_supported;
    data.network.io_channel_id = 1003;
    data.network.channel_ids = {1004, 1005, 1006};
    data.message_channel_id = 1007;

    Writer w;
    gcc::encode_server_data(w, data);
    CHECK(to_hex(w.view()) == "01 0c 10 00 04 00 08 00 0b 00 00 00 08 00 00 00 "  // SC_CORE
                              "02 0c 0c 00 00 00 00 00 00 00 00 00 "              // SC_SECURITY
                              "03 0c 10 00 eb 03 03 00 ec 03 ed 03 ee 03 00 00 "  // SC_NET, padded
                              "04 0c 06 00 ef 03");                               // SC_MCS_MSGCHANNEL

    Reader r(w.view());
    const auto decoded = gcc::decode_server_data(r).value();
    CHECK(decoded.core.client_requested_protocols == 0x0b);
    CHECK(decoded.network.io_channel_id == 1003);
    CHECK(decoded.network.channel_ids == std::vector<std::uint16_t>{1004, 1005, 1006});
    CHECK(decoded.message_channel_id == 1007);
}

TEST_CASE("Conference Create Request and Response wrappers round-trip")
{
    const auto blocks = hex("01 c0 04 00");
    Writer request;
    gcc::encode_conference_create_request(request, blocks);
    // connectPDU length 17: choice, selection, conference name (2), padding, number of sets,
    // choice, H.221 key (5), data length, 4 data bytes. Same layout as [MS-RDPBCGR] 4.1.3.
    CHECK(to_hex(request.view()) == "00 05 00 14 7c 00 01 11 00 08 00 10 00 01 c0 00 44 75 63 61 04 01 c0 04 00");
    Reader r(request.view());
    CHECK(to_hex(gcc::decode_conference_create_request(r).value()) == "01 c0 04 00");

    Writer response;
    gcc::encode_conference_create_response(response, blocks);
    // As FreeRDP and Windows servers send it, except that the connectPDU length
    // is exact (FreeRDP writes a fixed 0x2a, which clients ignore).
    CHECK(to_hex(response.view()) == "00 05 00 14 7c 00 01 12 14 76 0a 01 01 00 01 c0 00 4d 63 44 6e 04 01 c0 04 00");
    Reader rr(response.view());
    CHECK(to_hex(gcc::decode_conference_create_response(rr).value()) == "01 c0 04 00");
}
