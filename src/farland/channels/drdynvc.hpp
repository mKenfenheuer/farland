// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

/// PDU codecs of the Dynamic Virtual Channel Extension ([MS-RDPEDYC] 2.2),
/// the messages carried in the "drdynvc" static virtual channel. Every
/// message here is one reassembled static channel message; the static
/// channel chunking lives in svc.hpp. The server multiplexer is in
/// dvc_server.hpp.
namespace farland::channels::drdynvc {

/// Maximum DVC PDU size ([MS-RDPEDYC] 2.2.3).
inline constexpr std::size_t max_pdu_size = 1600;
/// Largest message sent as one DYNVC_DATA PDU; longer ones start with
/// DYNVC_DATA_FIRST ([MS-RDPEDYC] 2.2.3, 2.2.3.2).
inline constexpr std::size_t max_single_data_size = 1590;
/// farland limit on a listener name in DYNVC_CREATE_REQ. Real names are
/// below 64 characters ("Microsoft::Windows::RDS::Graphics").
inline constexpr std::size_t max_channel_name_size = 256;

/// Cmd values of the DVC header, [MS-RDPEDYC] 2.2.
namespace cmd {
inline constexpr std::uint8_t create = 0x01;
inline constexpr std::uint8_t data_first = 0x02;
inline constexpr std::uint8_t data = 0x03;
inline constexpr std::uint8_t close = 0x04;
inline constexpr std::uint8_t capabilities = 0x05;
inline constexpr std::uint8_t data_first_compressed = 0x06;
inline constexpr std::uint8_t data_compressed = 0x07;
inline constexpr std::uint8_t soft_sync_request = 0x08;
inline constexpr std::uint8_t soft_sync_response = 0x09;
}  // namespace cmd

/// Protocol version levels, [MS-RDPEDYC] 2.2.1.1.
inline constexpr std::uint16_t version1 = 1;
inline constexpr std::uint16_t version2 = 2;
inline constexpr std::uint16_t version3 = 3;

/// PriorityCharge0..3 ([MS-RDPEDYC] 2.2.1.1.2).
using PriorityCharges = std::array<std::uint16_t, 4>;
/// 70%, 20%, 7% and 3% of the bandwidth for priority classes 0-3, the worked
/// example of [MS-RDPEDYC] 2.2.1.1.2. farland opens channels in class 0.
inline constexpr PriorityCharges default_priority_charges{936, 3276, 9362, 21845};

/// Soft-Sync Request flags, [MS-RDPEDYC] 2.2.5.1.
namespace soft_sync_flag {
inline constexpr std::uint16_t tcp_flushed = 0x01;
inline constexpr std::uint16_t channel_list_present = 0x02;
}  // namespace soft_sync_flag

/// TunnelType values, [MS-RDPEDYC] 2.2.5.1.1.
namespace tunnel_type {
inline constexpr std::uint32_t udp_fec_reliable = 0x00000001;
inline constexpr std::uint32_t udp_fec_lossy = 0x00000003;
}  // namespace tunnel_type

// Variable-length fields -------------------------------------------------

/// The cbId/Len code (0, 1, 2) of the smallest field that holds `value`.
[[nodiscard]] std::uint8_t size_code(std::uint32_t value) noexcept;
/// Bytes of a field with the given cbId/Len code (1, 2 or 4).
[[nodiscard]] std::size_t field_size(std::uint8_t code) noexcept;
/// Reads a ChannelId or Length field of the given code. Code 3 is invalid.
[[nodiscard]] Result<std::uint32_t> read_var(Reader& r, std::uint8_t code);
/// Writes `value` in `size_code(value)` bytes.
void write_var(Writer& w, std::uint32_t value);

// PDUs -------------------------------------------------------------------
// Data fields are views: after decoding they refer into the input.

/// DYNVC_CAPS_VERSION1/2/3, [MS-RDPEDYC] 2.2.1.1. Version 1 has no priority
/// charges on the wire (they decode as zero).
struct CapsRequest {
    std::uint16_t version = version3;
    PriorityCharges priority_charges = default_priority_charges;

    friend bool operator==(const CapsRequest&, const CapsRequest&) = default;
};

/// DYNVC_CAPS_RSP, [MS-RDPEDYC] 2.2.1.2.
struct CapsResponse {
    std::uint16_t version = version3;

    friend bool operator==(const CapsResponse&, const CapsResponse&) = default;
};

/// DYNVC_CREATE_REQ, [MS-RDPEDYC] 2.2.2.1. `priority` is the Pri field (0-3).
struct CreateRequest {
    std::uint32_t channel_id = 0;
    std::uint8_t priority = 0;
    std::string name;  ///< ANSI listener name, without the terminator.

    friend bool operator==(const CreateRequest&, const CreateRequest&) = default;
};

/// DYNVC_CREATE_RSP, [MS-RDPEDYC] 2.2.2.2. CreationStatus is an HRESULT:
/// negative means failure.
struct CreateResponse {
    std::uint32_t channel_id = 0;
    std::int32_t creation_status = 0;

    friend bool operator==(const CreateResponse&, const CreateResponse&) = default;
};

/// DYNVC_DATA_FIRST, [MS-RDPEDYC] 2.2.3.1. `length` is the message total.
struct DataFirst {
    std::uint32_t channel_id = 0;
    std::uint32_t length = 0;
    std::span<const std::byte> data;

    friend bool operator==(const DataFirst& a, const DataFirst& b)
    {
        return a.channel_id == b.channel_id && a.length == b.length && std::ranges::equal(a.data, b.data);
    }
};

/// DYNVC_DATA, [MS-RDPEDYC] 2.2.3.2.
struct Data {
    std::uint32_t channel_id = 0;
    std::span<const std::byte> data;

    friend bool operator==(const Data& a, const Data& b)
    {
        return a.channel_id == b.channel_id && std::ranges::equal(a.data, b.data);
    }
};

/// DYNVC_DATA_FIRST_COMPRESSED, [MS-RDPEDYC] 2.2.3.3. `length` is the
/// uncompressed message total; `data` is one RDP_SEGMENTED_DATA (RDP 8.0 Lite).
struct DataFirstCompressed {
    std::uint32_t channel_id = 0;
    std::uint32_t length = 0;
    std::span<const std::byte> data;

    friend bool operator==(const DataFirstCompressed& a, const DataFirstCompressed& b)
    {
        return a.channel_id == b.channel_id && a.length == b.length && std::ranges::equal(a.data, b.data);
    }
};

/// DYNVC_DATA_COMPRESSED, [MS-RDPEDYC] 2.2.3.4.
struct DataCompressed {
    std::uint32_t channel_id = 0;
    std::span<const std::byte> data;

    friend bool operator==(const DataCompressed& a, const DataCompressed& b)
    {
        return a.channel_id == b.channel_id && std::ranges::equal(a.data, b.data);
    }
};

/// DYNVC_CLOSE, [MS-RDPEDYC] 2.2.4 (request and response alike).
struct Close {
    std::uint32_t channel_id = 0;

    friend bool operator==(const Close&, const Close&) = default;
};

/// DYNVC_SOFT_SYNC_CHANNEL_LIST, [MS-RDPEDYC] 2.2.5.1.1.
struct SoftSyncChannelList {
    std::uint32_t tunnel_type = tunnel_type::udp_fec_reliable;
    std::vector<std::uint32_t> channel_ids;

    friend bool operator==(const SoftSyncChannelList&, const SoftSyncChannelList&) = default;
};

/// DYNVC_SOFT_SYNC_REQUEST, [MS-RDPEDYC] 2.2.5.1. `channel_lists` is present
/// on the wire only with SOFT_SYNC_CHANNEL_LIST_PRESENT, and then has
/// `number_of_tunnels` entries.
struct SoftSyncRequest {
    std::uint16_t flags = soft_sync_flag::tcp_flushed;
    std::uint16_t number_of_tunnels = 0;
    std::vector<SoftSyncChannelList> channel_lists;

    friend bool operator==(const SoftSyncRequest&, const SoftSyncRequest&) = default;
};

/// DYNVC_SOFT_SYNC_RESPONSE, [MS-RDPEDYC] 2.2.5.2: TunnelsToSwitch.
struct SoftSyncResponse {
    std::vector<std::uint32_t> tunnels;

    friend bool operator==(const SoftSyncResponse&, const SoftSyncResponse&) = default;
};

/// What a client sends (Cmd 0x01 and 0x05 are responses in this direction).
using ClientPdu = std::variant<CapsResponse, CreateResponse, DataFirst, Data, DataFirstCompressed, DataCompressed,
                               Close, SoftSyncResponse>;
/// What a server sends (Cmd 0x01 and 0x05 are requests in this direction).
using ServerPdu = std::variant<CapsRequest, CreateRequest, DataFirst, Data, DataFirstCompressed, DataCompressed, Close,
                               SoftSyncRequest>;

/// Decodes one client-to-server drdynvc message. The unused cbId, Sp and Pad
/// fields of the capabilities PDU are not checked, because Windows leaves Sp
/// uninitialized ([MS-RDPEDYC] 6, <2>-<9>); Sp of DYNVC_DATA(_COMPRESSED) is
/// ignored for the same reason. Fixed-size PDUs must fill the message.
[[nodiscard]] Result<ClientPdu> decode_client_pdu(std::span<const std::byte> message);
/// Decodes one server-to-client drdynvc message (client side and tests).
[[nodiscard]] Result<ServerPdu> decode_server_pdu(std::span<const std::byte> message);

/// Encoders. ChannelId and Length fields use the smallest size that fits;
/// unused fields are zero. Invalid values (a version outside 1-3, priority
/// above 3, a bad channel name, inconsistent soft-sync counts) assert.
void encode(Writer& w, const CapsRequest& pdu);
void encode(Writer& w, const CapsResponse& pdu);
void encode(Writer& w, const CreateRequest& pdu);
void encode(Writer& w, const CreateResponse& pdu);
void encode(Writer& w, const DataFirst& pdu);
void encode(Writer& w, const Data& pdu);
void encode(Writer& w, const DataFirstCompressed& pdu);
void encode(Writer& w, const DataCompressed& pdu);
void encode(Writer& w, const Close& pdu);
void encode(Writer& w, const SoftSyncRequest& pdu);
void encode(Writer& w, const SoftSyncResponse& pdu);

[[nodiscard]] std::vector<std::byte> encode_client_pdu(const ClientPdu& pdu);
[[nodiscard]] std::vector<std::byte> encode_server_pdu(const ServerPdu& pdu);

/// Splits one message for a channel into uncompressed data PDUs of at most
/// `max_pdu_size` bytes ([MS-RDPEDYC] 2.2.3, 3.1.5.1): one DYNVC_DATA for up
/// to 1590 bytes, otherwise DYNVC_DATA_FIRST (with the total length) and as
/// many DYNVC_DATA as needed.
[[nodiscard]] std::vector<std::vector<std::byte>> encode_data(std::uint32_t channel_id,
                                                              std::span<const std::byte> message);

/// Reassembly of one channel's fragmented messages, [MS-RDPEDYC] 3.1.5.2.3.
/// Takes the (already decompressed) block of each data PDU. A DATA_FIRST
/// inside a message, blocks that overrun its length and lengths above
/// `max_message_size` are errors, and drop the partial message.
class MessageReassembler {
public:
    explicit MessageReassembler(std::size_t max_message_size) : max_size_(max_message_size) {}

    /// DYNVC_DATA_FIRST(_COMPRESSED): total length and the first block.
    [[nodiscard]] Result<std::optional<std::vector<std::byte>>> first(std::uint32_t length,
                                                                      std::span<const std::byte> block);
    /// DYNVC_DATA(_COMPRESSED): the next block, or a whole message.
    [[nodiscard]] Result<std::optional<std::vector<std::byte>>> next(std::span<const std::byte> block);

    [[nodiscard]] bool in_progress() const noexcept { return total_.has_value(); }

private:
    void reset() noexcept;

    std::size_t max_size_;
    std::optional<std::uint32_t> total_;
    std::vector<std::byte> buffer_;
};

}  // namespace farland::channels::drdynvc
