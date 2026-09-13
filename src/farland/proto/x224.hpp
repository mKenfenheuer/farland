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
#include <string>
#include <variant>
#include <vector>

/// TPKT (RFC 1006, T.123) and X.224 class 0 (ITU-T X.224) framing, and the
/// RDP negotiation structures carried in the X.224 Connection Request and
/// Confirm ([MS-RDPBCGR] 2.2.1.1 and 2.2.1.2).
namespace farland::proto {

inline constexpr std::size_t tpkt_header_size = 4;
inline constexpr std::size_t max_tpkt_size = 0xFFFF;

/// Security protocols, [MS-RDPBCGR] 2.2.1.1.1 (requestedProtocols) and 2.2.1.2.1 (selectedProtocol).
namespace protocol {
inline constexpr std::uint32_t rdp = 0x00000000;  ///< Standard RDP Security (RC4); never selected by farland.
inline constexpr std::uint32_t ssl = 0x00000001;
inline constexpr std::uint32_t hybrid = 0x00000002;  ///< CredSSP (NLA)
inline constexpr std::uint32_t rdstls = 0x00000004;
inline constexpr std::uint32_t hybrid_ex = 0x00000008;  ///< CredSSP with Early User Authorization Result
inline constexpr std::uint32_t rdsaad = 0x00000010;
}  // namespace protocol

/// RDP_NEG_REQ flags, [MS-RDPBCGR] 2.2.1.1.1.
namespace neg_req_flags {
inline constexpr std::uint8_t restricted_admin_mode_required = 0x01;
inline constexpr std::uint8_t redirected_authentication_mode_required = 0x02;
inline constexpr std::uint8_t correlation_info_present = 0x08;
}  // namespace neg_req_flags

/// RDP_NEG_RSP flags, [MS-RDPBCGR] 2.2.1.2.1.
namespace neg_rsp_flags {
inline constexpr std::uint8_t extended_client_data_supported = 0x01;
inline constexpr std::uint8_t dynvc_gfx_protocol_supported = 0x02;
inline constexpr std::uint8_t restricted_admin_mode_supported = 0x08;
inline constexpr std::uint8_t redirected_authentication_mode_supported = 0x10;
}  // namespace neg_rsp_flags

/// RDP_NEG_FAILURE failureCode, [MS-RDPBCGR] 2.2.1.2.2.
enum class NegotiationFailureCode : std::uint32_t {
    ssl_required_by_server = 1,
    ssl_not_allowed_by_server = 2,
    ssl_cert_not_on_server = 3,
    inconsistent_flags = 4,
    hybrid_required_by_server = 5,
    ssl_with_user_auth_required_by_server = 6,
};

/// X.224 TPDU codes (high nibble of the second header octet).
enum class TpduCode : std::uint8_t {
    connection_request = 0xE0,
    connection_confirm = 0xD0,
    disconnect_request = 0x80,
    data = 0xF0,
};

/// Client X.224 Connection Request PDU, [MS-RDPBCGR] 2.2.1.1.
struct ConnectionRequest {
    struct Negotiation {
        std::uint8_t flags = 0;
        std::uint32_t requested_protocols = 0;
    };

    /// User name hint from "Cookie: mstshash=<value>\r\n", without prefix and CRLF.
    std::string cookie;
    /// Any other CRLF-terminated token, typically a load-balancing "Cookie: msts=..."
    /// routing token, without the CRLF.
    std::vector<std::byte> routing_token;
    std::optional<Negotiation> negotiation;
    std::optional<std::array<std::byte, 16>> correlation_id;
};

/// Server X.224 Connection Confirm PDU, [MS-RDPBCGR] 2.2.1.2.
struct NegotiationResponse {
    std::uint8_t flags = 0;
    std::uint32_t selected_protocol = protocol::ssl;
};
struct ConnectionConfirm {
    /// monostate: no negotiation data (a legacy exchange farland never uses).
    std::variant<std::monostate, NegotiationResponse, NegotiationFailureCode> result;
};

// TPKT --------------------------------------------------------------------

/// Starts a TPKT with a length placeholder; returns its offset for `end_tpkt`.
[[nodiscard]] std::size_t begin_tpkt(Writer& w);
/// Patches the TPKT length. Asserts the packet fits in 65535 bytes.
void end_tpkt(Writer& w, std::size_t start);
/// Validates a complete TPKT and returns a reader over the TPDU it carries.
[[nodiscard]] Result<Reader> read_tpkt(Reader& r);

// X.224 -------------------------------------------------------------------

/// The code of the TPDU at the reader position, without consuming it.
[[nodiscard]] Result<TpduCode> peek_tpdu_code(const Reader& tpdu);

[[nodiscard]] Result<ConnectionRequest> decode_connection_request(Reader& tpdu);
/// Complete TPKT with the Connection Request.
void encode_connection_request(Writer& w, const ConnectionRequest& request);

[[nodiscard]] Result<ConnectionConfirm> decode_connection_confirm(Reader& tpdu);
/// Complete TPKT with the Connection Confirm.
void encode_connection_confirm(Writer& w, const ConnectionConfirm& confirm);

/// Validates an X.224 Data TPDU header and returns a reader over its user data.
[[nodiscard]] Result<Reader> decode_data_tpdu(Reader& tpdu);
/// Starts TPKT + X.224 Data TPDU. Finish with `end_tpkt(w, start)`.
[[nodiscard]] std::size_t begin_data_tpdu(Writer& w);

}  // namespace farland::proto
