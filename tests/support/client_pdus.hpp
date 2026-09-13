// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/proto/gcc.hpp>
#include <farland/proto/share.hpp>

#include <cstdint>
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

}  // namespace farland::test::client
