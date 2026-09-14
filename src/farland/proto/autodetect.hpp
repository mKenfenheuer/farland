// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <variant>

/// Network characteristics detection ("auto-detect", [MS-RDPBCGR] 1.3.9 and
/// 2.2.14) and the Heartbeat PDU (2.2.16). Both travel on the MCS message
/// channel after a basic security header with SEC_AUTODETECT_REQ,
/// SEC_AUTODETECT_RSP or SEC_HEARTBEAT; these codecs cover what follows
/// that header (autoDetectReqPduData, autoDetectRspPduData, the heartbeat
/// fields).
///
/// Decoded payloads hold spans into the input.
namespace farland::proto::autodetect {

/// headerTypeId, [MS-RDPBCGR] 2.2.14.1 and 2.2.14.2.
inline constexpr std::uint8_t type_id_request = 0x00;
inline constexpr std::uint8_t type_id_response = 0x01;

/// requestType values of the server-to-client messages, [MS-RDPBCGR] 2.2.14.1.
namespace request_type {
inline constexpr std::uint16_t rtt_continuous = 0x0001;
inline constexpr std::uint16_t rtt_connect_time = 0x1001;
inline constexpr std::uint16_t bw_start_continuous = 0x0014;  ///< also over reliable UDP
inline constexpr std::uint16_t bw_start_lossy_udp = 0x0114;
inline constexpr std::uint16_t bw_start_connect_time = 0x1014;
inline constexpr std::uint16_t bw_payload = 0x0002;
inline constexpr std::uint16_t bw_stop_connect_time = 0x002B;
inline constexpr std::uint16_t bw_stop_continuous = 0x0429;  ///< also over reliable UDP
inline constexpr std::uint16_t bw_stop_lossy_udp = 0x0629;
inline constexpr std::uint16_t netchar_base_rtt_average_rtt = 0x0840;
inline constexpr std::uint16_t netchar_bandwidth_average_rtt = 0x0880;
inline constexpr std::uint16_t netchar_all = 0x08C0;
}  // namespace request_type

/// responseType values of the client-to-server messages, [MS-RDPBCGR] 2.2.14.2.
namespace response_type {
inline constexpr std::uint16_t rtt = 0x0000;
inline constexpr std::uint16_t bw_results_connect_time = 0x0003;
inline constexpr std::uint16_t bw_results_continuous = 0x000B;
inline constexpr std::uint16_t netchar_sync = 0x0018;
}  // namespace response_type

/// Largest payload farland puts in one Bandwidth Measure Payload or Stop, so
/// that the MCS Send Data PDU stays below 16383 bytes ([MS-RDPBCGR] 2.2).
inline constexpr std::size_t max_payload_size = 15000;

// Server to client -----------------------------------------------------------

/// RTT Measure Request (RDP_RTT_REQUEST), [MS-RDPBCGR] 2.2.14.1.1.
struct RttRequest {
    std::uint16_t sequence = 0;
    std::uint16_t request_type = request_type::rtt_continuous;
    friend bool operator==(const RttRequest&, const RttRequest&) = default;
};

/// Bandwidth Measure Start (RDP_BW_START), [MS-RDPBCGR] 2.2.14.1.2.
struct BandwidthStart {
    std::uint16_t sequence = 0;
    std::uint16_t request_type = request_type::bw_start_continuous;
    friend bool operator==(const BandwidthStart&, const BandwidthStart&) = default;
};

/// Bandwidth Measure Payload (RDP_BW_PAYLOAD), [MS-RDPBCGR] 2.2.14.1.3; only
/// during the connect-time detection.
struct BandwidthPayload {
    std::uint16_t sequence = 0;
    std::span<const std::byte> payload;
};

/// Bandwidth Measure Stop (RDP_BW_STOP), [MS-RDPBCGR] 2.2.14.1.4. Only the
/// connect-time stop (0x002B) carries a payload, and it must not be empty.
struct BandwidthStop {
    std::uint16_t sequence = 0;
    std::uint16_t request_type = request_type::bw_stop_continuous;
    std::span<const std::byte> payload;
};

/// Network Characteristics Result (RDP_NETCHAR_RESULTS), [MS-RDPBCGR]
/// 2.2.14.1.5. averageRTT is always present, with baseRTT, bandwidth or both;
/// the requestType follows from which.
struct NetworkCharacteristicsResult {
    std::uint16_t sequence = 0;
    std::optional<std::uint32_t> base_rtt_ms;
    std::optional<std::uint32_t> bandwidth_kbps;
    std::uint32_t average_rtt_ms = 0;
    friend bool operator==(const NetworkCharacteristicsResult&, const NetworkCharacteristicsResult&) = default;
};

using Request = std::variant<RttRequest, BandwidthStart, BandwidthPayload, BandwidthStop, NetworkCharacteristicsResult>;

// Client to server -----------------------------------------------------------

/// RTT Measure Response (RDP_RTT_RESPONSE), [MS-RDPBCGR] 2.2.14.2.1.
struct RttResponse {
    std::uint16_t sequence = 0;
    friend bool operator==(const RttResponse&, const RttResponse&) = default;
};

/// Bandwidth Measure Results (RDP_BW_RESULTS), [MS-RDPBCGR] 2.2.14.2.2.
struct BandwidthResults {
    std::uint16_t sequence = 0;
    std::uint16_t response_type = response_type::bw_results_continuous;
    std::uint32_t time_delta_ms = 0;
    std::uint32_t byte_count = 0;
    friend bool operator==(const BandwidthResults&, const BandwidthResults&) = default;
};

/// Network Characteristics Sync (RDP_NETCHAR_SYNC), [MS-RDPBCGR] 2.2.14.2.3:
/// an auto-reconnecting client reports what it measured before.
struct NetworkCharacteristicsSync {
    std::uint16_t sequence = 0;
    std::uint32_t bandwidth_kbps = 0;
    std::uint32_t rtt_ms = 0;
    friend bool operator==(const NetworkCharacteristicsSync&, const NetworkCharacteristicsSync&) = default;
};

using Response = std::variant<RttResponse, BandwidthResults, NetworkCharacteristicsSync>;

/// Decodes autoDetectReqPduData (the rest of `r` after the security header).
/// Unknown request types are Errc::unsupported.
[[nodiscard]] Result<Request> decode_request(Reader& r);
void encode(Writer& w, const Request& request);

/// Decodes autoDetectRspPduData. Unknown response types are Errc::unsupported.
[[nodiscard]] Result<Response> decode_response(Reader& r);
void encode(Writer& w, const Response& response);

/// The requestType a Network Characteristics Result goes out with.
[[nodiscard]] std::uint16_t request_type_of(const NetworkCharacteristicsResult& result) noexcept;

// Heartbeat --------------------------------------------------------------------

/// Server Heartbeat PDU fields after the security header, [MS-RDPBCGR] 2.2.16.1.
struct Heartbeat {
    std::uint8_t period_seconds = 0;
    /// Missed heartbeats before the client warns, then before it reconnects.
    std::uint8_t warning_count = 0;
    std::uint8_t reconnect_count = 0;
    friend bool operator==(const Heartbeat&, const Heartbeat&) = default;
};

[[nodiscard]] Result<Heartbeat> decode_heartbeat(Reader& r);
void encode(Writer& w, const Heartbeat& heartbeat);

}  // namespace farland::proto::autodetect
