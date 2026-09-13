// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/text.hpp>
#include <farland/base/writer.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

/// Client Info PDU (TS_INFO_PACKET), [MS-RDPBCGR] 2.2.1.11.
namespace farland::proto {

/// TS_INFO_PACKET flags, [MS-RDPBCGR] 2.2.1.11.1.1.
namespace info_flags {
inline constexpr std::uint32_t mouse = 0x00000001;
inline constexpr std::uint32_t disable_ctrl_alt_del = 0x00000002;
inline constexpr std::uint32_t autologon = 0x00000008;
inline constexpr std::uint32_t unicode = 0x00000010;
inline constexpr std::uint32_t maximize_shell = 0x00000020;
inline constexpr std::uint32_t logon_notify = 0x00000040;
inline constexpr std::uint32_t compression = 0x00000080;
inline constexpr std::uint32_t enable_windows_key = 0x00000100;
inline constexpr std::uint32_t compression_type_mask = 0x00001E00;
inline constexpr std::uint32_t remote_console_audio = 0x00002000;
inline constexpr std::uint32_t force_encrypted_cs_pdu = 0x00004000;
inline constexpr std::uint32_t rail = 0x00008000;
inline constexpr std::uint32_t logon_errors = 0x00010000;
inline constexpr std::uint32_t mouse_has_wheel = 0x00020000;
inline constexpr std::uint32_t password_is_sc_pin = 0x00040000;
inline constexpr std::uint32_t no_audio_playback = 0x00080000;
inline constexpr std::uint32_t using_saved_creds = 0x00100000;
inline constexpr std::uint32_t audio_capture = 0x00200000;
inline constexpr std::uint32_t video_disable = 0x00400000;
inline constexpr std::uint32_t hidef_rail_supported = 0x02000000;
}  // namespace info_flags

/// TS_TIME_ZONE_INFORMATION, [MS-RDPBCGR] 2.2.1.11.1.1.1.1.
struct TimeZoneInformation {
    std::int32_t bias = 0;
    std::string standard_name;
    std::array<std::byte, 16> standard_date{};  ///< TS_SYSTEMTIME, kept raw
    std::int32_t standard_bias = 0;
    std::string daylight_name;
    std::array<std::byte, 16> daylight_date{};
    std::int32_t daylight_bias = 0;
};

/// ARC_CS_PRIVATE_PACKET, [MS-RDPBCGR] 2.2.4.3.
struct AutoReconnectCookie {
    std::uint32_t version = 1;
    std::uint32_t logon_id = 0;
    std::array<std::byte, 16> security_verifier{};
};

/// TS_EXTENDED_INFO_PACKET, [MS-RDPBCGR] 2.2.1.11.1.1.1. Fields after
/// clientDir are optional; each is present only if all before it are.
struct ExtendedInfo {
    std::uint16_t client_address_family = 0x0002;  ///< AF_INET; AF_INET6 is 0x0017
    std::string client_address;
    std::string client_dir;
    std::optional<TimeZoneInformation> time_zone;
    std::optional<std::uint32_t> session_id;
    std::optional<std::uint32_t> performance_flags;
    std::optional<AutoReconnectCookie> auto_reconnect_cookie;
    std::optional<std::string> dynamic_dst_time_zone_key_name;
    std::optional<bool> dynamic_daylight_time_disabled;
};

struct ClientInfo {
    std::uint32_t code_page = 0;
    std::uint32_t flags = info_flags::unicode;
    std::string domain;
    std::string user_name;
    SecretString password;
    std::string alternate_shell;
    std::string working_dir;
    std::optional<ExtendedInfo> extended;
};

/// Longest string field accepted, in bytes without terminator
/// ([MS-RDPBCGR] 2.2.1.11.1.1 allows 512 for RDP 5.1 and later).
inline constexpr std::uint16_t max_info_string_size = 512;

/// Decodes TS_INFO_PACKET (the bytes after the basic security header).
[[nodiscard]] Result<ClientInfo> decode_client_info(Reader& r);
/// Encodes TS_INFO_PACKET with Unicode strings.
void encode_client_info(Writer& w, const ClientInfo& info);

}  // namespace farland::proto
