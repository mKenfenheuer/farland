// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

/// Save Session Info PDU ([MS-RDPBCGR] 2.2.10.1) with the extended logon
/// information: how the server hands the client its auto-reconnect cookie
/// (ARC_SC_PRIVATE_PACKET, 2.2.4.2) after the user has logged on ([MS-RDPBCGR]
/// 5.5). The client's answer, ARC_CS_PRIVATE_PACKET, is part of the Client
/// Info PDU (client_info.hpp); the verifier is in auth/auto_reconnect.hpp.
namespace farland::proto {

/// TS_SAVE_SESSION_INFO_PDU_DATA infoType, [MS-RDPBCGR] 2.2.10.1.1.
namespace info_type {
inline constexpr std::uint32_t logon = 0x00000000;
inline constexpr std::uint32_t logon_long = 0x00000001;
inline constexpr std::uint32_t logon_plain_notify = 0x00000002;
inline constexpr std::uint32_t logon_extended_info = 0x00000003;
}  // namespace info_type

/// TS_LOGON_INFO_EXTENDED FieldsPresent, [MS-RDPBCGR] 2.2.10.1.1.4.
namespace logon_ex {
inline constexpr std::uint32_t auto_reconnect_cookie = 0x00000001;
inline constexpr std::uint32_t logon_errors = 0x00000002;
}  // namespace logon_ex

inline constexpr std::uint32_t auto_reconnect_version_1 = 0x00000001;

/// ARC_SC_PRIVATE_PACKET, [MS-RDPBCGR] 2.2.4.2.
struct ServerAutoReconnectCookie {
    std::uint32_t version = auto_reconnect_version_1;
    std::uint32_t logon_id = 0;  ///< the session the client reconnects to
    std::array<std::byte, 16> random_bits{};
};

/// TS_LOGON_ERRORS_INFO, [MS-RDPBCGR] 2.2.10.1.1.4.1.1.
struct LogonErrorsInfo {
    std::uint32_t error_notification_type = 0;
    std::uint32_t error_notification_data = 0;
};

/// TS_LOGON_INFO_EXTENDED, [MS-RDPBCGR] 2.2.10.1.1.4.
struct LogonInfoExtended {
    std::optional<ServerAutoReconnectCookie> auto_reconnect_cookie;
    std::optional<LogonErrorsInfo> logon_errors;
};

/// Encodes TS_SAVE_SESSION_INFO_PDU_DATA after the Share Data Header (the
/// payload for `write_data_pdu` with pdu_type2::save_session_info): infoType
/// INFOTYPE_LOGON_EXTENDED_INFO and the structure.
void encode_save_session_info(Writer& w, const LogonInfoExtended& info);

/// Decodes that payload. Other info types are Errc::unsupported.
[[nodiscard]] Result<LogonInfoExtended> decode_save_session_info(Reader& r);

}  // namespace farland::proto
