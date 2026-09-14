// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/proto/autodetect.hpp>
#include <farland/proto/gcc.hpp>
#include <farland/proto/share.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/// Client-side PDUs for scripting a well-behaved RDP client in tests, built
/// with farland's own encoders.
namespace farland::test::client {

using Bytes = std::vector<std::byte>;

[[nodiscard]] Bytes connection_request(std::uint32_t protocols);
[[nodiscard]] proto::gcc::ClientData client_data(std::uint32_t selected_protocol, std::uint16_t early_flags,
                                                 std::uint16_t width, std::uint16_t height);
[[nodiscard]] Bytes connect_initial(const proto::gcc::ClientData& data);
[[nodiscard]] Bytes erect_domain();
[[nodiscard]] Bytes attach_user();
[[nodiscard]] Bytes channel_join(std::uint16_t user, std::uint16_t channel);
/// Send Data Request on the I/O channel.
[[nodiscard]] Bytes io(std::uint16_t user, std::span<const std::byte> payload);
[[nodiscard]] Bytes client_info(std::uint16_t user, std::string_view user_name);
[[nodiscard]] Bytes confirm_active(std::uint16_t user, std::uint32_t share_id, bool fastpath_output,
                                   std::uint16_t width, std::uint16_t height);
[[nodiscard]] Bytes data_pdu(std::uint16_t user, std::uint32_t share_id, const proto::DataPdu& pdu);
/// Synchronize, Control Cooperate, Control Request and Font List, concatenated.
[[nodiscard]] Bytes finalization(std::uint16_t user, std::uint32_t share_id);
[[nodiscard]] Bytes fastpath_input(std::span<const proto::InputEvent> events);

/// Asks for auto-detect and heartbeats as FreeRDP 3 does by default:
/// RNS_UD_CS_SUPPORT_NETCHAR_AUTODETECT, RNS_UD_CS_SUPPORT_HEARTBEAT_PDU and
/// a CS_MCS_MSGCHANNEL block.
void request_autodetect(proto::gcc::ClientData& data);
/// The message channel from the server's MCS Connect Response, if it offered one.
[[nodiscard]] std::optional<std::uint16_t> message_channel_id(std::span<const std::byte> connect_response);

/// The client half of auto-detect ([MS-RDPBCGR] 3.2.5.14): answers RTT
/// probes, times bandwidth measurements over the PDUs between Start and
/// Stop, and records Network Characteristics Results and heartbeats.
class AutoDetectResponder {
public:
    using Clock = std::chrono::steady_clock;

    AutoDetectResponder(std::uint16_t user, std::uint16_t message_channel)
        : user_(user), message_channel_(message_channel)
    {
    }

    /// Looks at one server PDU (TPKT or fast-path) and returns the answer if
    /// it is an auto-detect request that wants one.
    [[nodiscard]] std::optional<Bytes> answer(std::span<const std::byte> pdu, Clock::time_point now = Clock::now());
    [[nodiscard]] bool on_message_channel(std::span<const std::byte> pdu) const;

    unsigned rtt_answers = 0;
    unsigned connect_time_results = 0;
    unsigned continuous_results = 0;
    unsigned heartbeats = 0;
    std::optional<proto::autodetect::NetworkCharacteristicsResult> result;

private:
    [[nodiscard]] Bytes response(const proto::autodetect::Response& response) const;

    std::uint16_t user_;
    std::uint16_t message_channel_;
    bool measuring_ = false;
    Clock::time_point measure_start_;
    std::uint64_t measured_bytes_ = 0;
};

}  // namespace farland::test::client
