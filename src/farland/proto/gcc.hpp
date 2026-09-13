// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

/// T.124 Generic Conference Control as used by RDP: the Conference Create
/// Request/Response wrappers and the client/server data blocks they carry,
/// [MS-RDPBCGR] 2.2.1.3 and 2.2.1.4.
namespace farland::proto::gcc {

/// Data block types, [MS-RDPBCGR] 2.2.1.3.1 (TS_UD_HEADER).
namespace block_type {
inline constexpr std::uint16_t cs_core = 0xC001;
inline constexpr std::uint16_t cs_security = 0xC002;
inline constexpr std::uint16_t cs_net = 0xC003;
inline constexpr std::uint16_t cs_cluster = 0xC004;
inline constexpr std::uint16_t cs_monitor = 0xC005;
inline constexpr std::uint16_t cs_mcs_msgchannel = 0xC006;
inline constexpr std::uint16_t cs_monitor_ex = 0xC008;
inline constexpr std::uint16_t cs_multitransport = 0xC00A;
inline constexpr std::uint16_t sc_core = 0x0C01;
inline constexpr std::uint16_t sc_security = 0x0C02;
inline constexpr std::uint16_t sc_net = 0x0C03;
inline constexpr std::uint16_t sc_mcs_msgchannel = 0x0C04;
inline constexpr std::uint16_t sc_multitransport = 0x0C08;
}  // namespace block_type

/// TS_UD_CS_CORE earlyCapabilityFlags, [MS-RDPBCGR] 2.2.1.3.2.
namespace cs_early_flags {
inline constexpr std::uint16_t support_errinfo_pdu = 0x0001;
inline constexpr std::uint16_t want_32bpp_session = 0x0002;
inline constexpr std::uint16_t support_statusinfo_pdu = 0x0004;
inline constexpr std::uint16_t strong_asymmetric_keys = 0x0008;
inline constexpr std::uint16_t relative_mouse_input = 0x0010;
inline constexpr std::uint16_t valid_connection_type = 0x0020;
inline constexpr std::uint16_t support_monitor_layout_pdu = 0x0040;
inline constexpr std::uint16_t support_netchar_autodetect = 0x0080;
inline constexpr std::uint16_t support_dynvc_gfx_protocol = 0x0100;
inline constexpr std::uint16_t support_dynamic_time_zone = 0x0200;
inline constexpr std::uint16_t support_heartbeat_pdu = 0x0400;
inline constexpr std::uint16_t support_skip_channeljoin = 0x0800;
}  // namespace cs_early_flags

/// TS_UD_CS_CORE supportedColorDepths, [MS-RDPBCGR] 2.2.1.3.2.
namespace color_depth_support {
inline constexpr std::uint16_t bpp24 = 0x0001;
inline constexpr std::uint16_t bpp16 = 0x0002;
inline constexpr std::uint16_t bpp15 = 0x0004;
inline constexpr std::uint16_t bpp32 = 0x0008;
}  // namespace color_depth_support

/// TS_UD_SC_CORE earlyCapabilityFlags, [MS-RDPBCGR] 2.2.1.4.2.
namespace sc_early_flags {
inline constexpr std::uint32_t edge_actions_supported_v1 = 0x00000001;
inline constexpr std::uint32_t dynamic_dst_supported = 0x00000002;
inline constexpr std::uint32_t edge_actions_supported_v2 = 0x00000004;
inline constexpr std::uint32_t skip_channeljoin_supported = 0x00000008;
}  // namespace sc_early_flags

/// RDP version numbers for TS_UD_CS_CORE / TS_UD_SC_CORE.
inline constexpr std::uint32_t rdp_version_5_plus = 0x00080004;

/// Client Core Data (TS_UD_CS_CORE), [MS-RDPBCGR] 2.2.1.3.2. Fields after
/// imeFileName are optional; each may only be present if all before it are.
struct ClientCoreData {
    std::uint32_t version = rdp_version_5_plus;
    std::uint16_t desktop_width = 0;
    std::uint16_t desktop_height = 0;
    std::uint16_t color_depth = 0xCA01;
    std::uint16_t sas_sequence = 0xAA03;
    std::uint32_t keyboard_layout = 0;
    std::uint32_t client_build = 0;
    std::string client_name;
    std::uint32_t keyboard_type = 4;
    std::uint32_t keyboard_subtype = 0;
    std::uint32_t keyboard_function_keys = 12;
    std::string ime_file_name;

    std::optional<std::uint16_t> post_beta2_color_depth;
    std::optional<std::uint16_t> client_product_id;
    std::optional<std::uint32_t> serial_number;
    std::optional<std::uint16_t> high_color_depth;
    std::optional<std::uint16_t> supported_color_depths;
    std::optional<std::uint16_t> early_capability_flags;
    std::optional<std::string> client_dig_product_id;
    std::optional<std::uint8_t> connection_type;
    std::optional<std::uint32_t> server_selected_protocol;
    std::optional<std::uint32_t> desktop_physical_width;
    std::optional<std::uint32_t> desktop_physical_height;
    std::optional<std::uint16_t> desktop_orientation;
    std::optional<std::uint32_t> desktop_scale_factor;
    std::optional<std::uint32_t> device_scale_factor;

    [[nodiscard]] bool has_early_flag(std::uint16_t flag) const noexcept
    {
        return early_capability_flags.has_value() && (*early_capability_flags & flag) != 0;
    }
};

/// TS_UD_CS_SEC, [MS-RDPBCGR] 2.2.1.3.3.
struct ClientSecurityData {
    std::uint32_t encryption_methods = 0;
    std::uint32_t ext_encryption_methods = 0;
};

/// One static virtual channel (CHANNEL_DEF), [MS-RDPBCGR] 2.2.1.3.4.1.
struct ChannelDef {
    std::string name;  ///< At most 7 ASCII characters.
    std::uint32_t options = 0;
};
inline constexpr std::size_t max_static_channels = 31;

/// TS_UD_CS_NET, [MS-RDPBCGR] 2.2.1.3.4.
struct ClientNetworkData {
    std::vector<ChannelDef> channels;
};

/// TS_UD_CS_CLUSTER, [MS-RDPBCGR] 2.2.1.3.5.
struct ClientClusterData {
    std::uint32_t flags = 0;
    std::uint32_t redirected_session_id = 0;
};

/// TS_MONITOR_DEF, [MS-RDPBCGR] 2.2.1.3.6.1. Coordinates are inclusive.
struct MonitorDef {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    std::uint32_t flags = 0;  ///< TS_MONITOR_PRIMARY = 0x1
};
inline constexpr std::size_t max_monitors = 16;

/// TS_UD_CS_MONITOR, [MS-RDPBCGR] 2.2.1.3.6.
struct ClientMonitorData {
    std::uint32_t flags = 0;
    std::vector<MonitorDef> monitors;
};

/// TS_MONITOR_ATTRIBUTES, [MS-RDPBCGR] 2.2.1.3.9.1.
struct MonitorAttributes {
    std::uint32_t physical_width = 0;
    std::uint32_t physical_height = 0;
    std::uint32_t orientation = 0;
    std::uint32_t desktop_scale_factor = 0;
    std::uint32_t device_scale_factor = 0;
};

/// TS_UD_CS_MONITOR_EX, [MS-RDPBCGR] 2.2.1.3.9.
struct ClientMonitorExtendedData {
    std::uint32_t flags = 0;
    std::vector<MonitorAttributes> monitors;
};

/// TS_UD_CS_MCS_MSGCHANNEL (2.2.1.3.7) and TS_UD_CS_MULTITRANSPORT (2.2.1.3.8).
struct ClientMessageChannelData {
    std::uint32_t flags = 0;
};
struct ClientMultitransportData {
    std::uint32_t flags = 0;
};

/// All client data blocks of a Conference Create Request.
struct ClientData {
    ClientCoreData core;
    std::optional<ClientSecurityData> security;
    std::optional<ClientNetworkData> network;
    std::optional<ClientClusterData> cluster;
    std::optional<ClientMonitorData> monitor;
    std::optional<ClientMessageChannelData> message_channel;
    std::optional<ClientMonitorExtendedData> monitor_ex;
    std::optional<ClientMultitransportData> multitransport;
};

/// TS_UD_SC_CORE, [MS-RDPBCGR] 2.2.1.4.2.
struct ServerCoreData {
    std::uint32_t version = rdp_version_5_plus;
    std::optional<std::uint32_t> client_requested_protocols;
    std::optional<std::uint32_t> early_capability_flags;
};

/// TS_UD_SC_SEC1, [MS-RDPBCGR] 2.2.1.4.3. Under TLS both values are zero and
/// there is no server random or certificate.
struct ServerSecurityData {
    std::uint32_t encryption_method = 0;
    std::uint32_t encryption_level = 0;
};

/// TS_UD_SC_NET, [MS-RDPBCGR] 2.2.1.4.4.
struct ServerNetworkData {
    std::uint16_t io_channel_id = 0;
    std::vector<std::uint16_t> channel_ids;  ///< One per CS_NET channel, in the same order.
};

struct ServerData {
    ServerCoreData core;
    ServerSecurityData security;
    ServerNetworkData network;
    std::optional<std::uint16_t> message_channel_id;    ///< TS_UD_SC_MCS_MSGCHANNEL
    std::optional<std::uint32_t> multitransport_flags;  ///< TS_UD_SC_MULTITRANSPORT
};

// Conference Create Request / Response -------------------------------------

/// Unwraps a Conference Create Request (the MCS Connect-Initial user data) and
/// returns the client data blocks it carries.
[[nodiscard]] Result<std::span<const std::byte>> decode_conference_create_request(Reader& r);
void encode_conference_create_request(Writer& w, std::span<const std::byte> client_data);

/// Unwraps a Conference Create Response and returns the server data blocks.
[[nodiscard]] Result<std::span<const std::byte>> decode_conference_create_response(Reader& r);
void encode_conference_create_response(Writer& w, std::span<const std::byte> server_data);

// Data blocks ---------------------------------------------------------------

[[nodiscard]] Result<ClientData> decode_client_data(Reader& r);
void encode_client_data(Writer& w, const ClientData& data);

[[nodiscard]] Result<ServerData> decode_server_data(Reader& r);
void encode_server_data(Writer& w, const ServerData& data);

}  // namespace farland::proto::gcc
