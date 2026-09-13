// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

/// T.125 Multipoint Communication Service as used by RDP: the BER-encoded
/// Connect-Initial/Connect-Response ([MS-RDPBCGR] 2.2.1.3, 2.2.1.4) and the
/// PER-encoded domain PDUs ([MS-RDPBCGR] 2.2.1.5 - 2.2.1.8, 2.2.8.1.1.1).
///
/// Decoded structures hold spans into the input; they are valid only as long
/// as the input buffer is.
namespace farland::proto::mcs {

/// Lower bound of MCS user IDs (T.125 UserId ::= DynamicChannelId (1001..65535)).
inline constexpr std::uint16_t user_id_base = 1001;
/// The channel carrying RDP's own PDUs, [MS-RDPBCGR] 2.2.1.4.4.
inline constexpr std::uint16_t io_channel_id = 1003;
/// The channel ID servers use as their share control pduSource.
inline constexpr std::uint16_t server_channel_id = 1002;

/// T.125 DomainParameters.
struct DomainParameters {
    std::uint32_t max_channel_ids = 34;
    std::uint32_t max_user_ids = 2;
    std::uint32_t max_token_ids = 0;
    std::uint32_t num_priorities = 1;
    std::uint32_t min_throughput = 0;
    std::uint32_t max_height = 1;
    std::uint32_t max_mcs_pdu_size = 65535;
    std::uint32_t protocol_version = 2;

    friend bool operator==(const DomainParameters&, const DomainParameters&) = default;
};

struct ConnectInitial {
    std::span<const std::byte> calling_domain_selector;
    std::span<const std::byte> called_domain_selector;
    bool upward_flag = true;
    DomainParameters target;
    DomainParameters minimum;
    DomainParameters maximum;
    std::span<const std::byte> user_data;  ///< GCC Conference Create Request
};

/// T.125 Result values used in Connect-Response and the confirm PDUs.
enum class ResultCode : std::uint8_t {
    successful = 0,
    no_such_channel = 3,
    no_such_user = 5,
    parameters_unacceptable = 8,
    too_many_channels = 11,
    unspecified_failure = 14,
};

struct ConnectResponse {
    ResultCode result = ResultCode::successful;
    std::uint32_t called_connect_id = 0;
    DomainParameters parameters;
    std::span<const std::byte> user_data;  ///< GCC Conference Create Response
};

[[nodiscard]] Result<ConnectInitial> decode_connect_initial(Reader& r);
void encode_connect_initial(Writer& w, const ConnectInitial& pdu);
[[nodiscard]] Result<ConnectResponse> decode_connect_response(Reader& r);
void encode_connect_response(Writer& w, const ConnectResponse& pdu);

/// The parameters a server answers with: the client's targets, clamped to the
/// client's minimum and maximum.
[[nodiscard]] DomainParameters negotiate_domain_parameters(const ConnectInitial& pdu);

// Domain PDUs ---------------------------------------------------------------

/// Disconnect Provider Ultimatum reasons (T.125 Reason).
enum class DisconnectReason : std::uint8_t {
    domain_disconnected = 0,
    provider_initiated = 1,
    token_purged = 2,
    user_requested = 3,
    channel_purged = 4,
};

struct ErectDomainRequest {
    std::uint32_t sub_height = 0;
    std::uint32_t sub_interval = 0;
};
struct DisconnectProviderUltimatum {
    DisconnectReason reason = DisconnectReason::user_requested;
};
struct AttachUserRequest {};
struct AttachUserConfirm {
    ResultCode result = ResultCode::successful;
    std::optional<std::uint16_t> initiator;  ///< The user channel ID assigned to the client.
};
struct ChannelJoinRequest {
    std::uint16_t initiator = 0;
    std::uint16_t channel_id = 0;
};
struct ChannelJoinConfirm {
    ResultCode result = ResultCode::successful;
    std::uint16_t initiator = 0;
    std::uint16_t requested = 0;
    std::optional<std::uint16_t> channel_id;
};
/// Send Data Request (client to server).
struct SendDataRequest {
    std::uint16_t initiator = 0;
    std::uint16_t channel_id = 0;
    std::span<const std::byte> data;
};
/// Send Data Indication (server to client).
struct SendDataIndication {
    std::uint16_t initiator = 0;
    std::uint16_t channel_id = 0;
    std::span<const std::byte> data;
};

using DomainPdu = std::variant<ErectDomainRequest, DisconnectProviderUltimatum, AttachUserRequest, AttachUserConfirm,
                               ChannelJoinRequest, ChannelJoinConfirm, SendDataRequest, SendDataIndication>;

/// Decodes the single domain PDU filling `r` (the X.224 Data TPDU user data).
[[nodiscard]] Result<DomainPdu> decode_domain_pdu(Reader& r);

void encode(Writer& w, const ErectDomainRequest& pdu);
void encode(Writer& w, const DisconnectProviderUltimatum& pdu);
void encode(Writer& w, const AttachUserRequest& pdu);
void encode(Writer& w, const AttachUserConfirm& pdu);
void encode(Writer& w, const ChannelJoinRequest& pdu);
void encode(Writer& w, const ChannelJoinConfirm& pdu);
void encode(Writer& w, const SendDataRequest& pdu);
void encode(Writer& w, const SendDataIndication& pdu);

/// Size of the Send Data header in front of the payload, for callers that
/// budget PDU sizes.
inline constexpr std::size_t send_data_header_max = 8;

}  // namespace farland::proto::mcs
