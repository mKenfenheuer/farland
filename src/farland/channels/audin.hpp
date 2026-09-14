// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/channels/rdpsnd.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

/// Audio input redirection, [MS-RDPEAI]: the client's microphone over the
/// "AUDIO_INPUT" dynamic virtual channel (2.1). Each PDU is one DVC message.
namespace farland::channels::audin {

inline constexpr std::string_view channel_name = "AUDIO_INPUT";

/// MessageId of the SNDIN_PDU header, [MS-RDPEAI] 2.2.1.
namespace msg {
inline constexpr std::uint8_t version = 0x01;
inline constexpr std::uint8_t formats = 0x02;
inline constexpr std::uint8_t open = 0x03;
inline constexpr std::uint8_t open_reply = 0x04;
inline constexpr std::uint8_t data_incoming = 0x05;
inline constexpr std::uint8_t data = 0x06;
inline constexpr std::uint8_t format_change = 0x07;
}  // namespace msg

/// Version PDU values, [MS-RDPEAI] 2.2.2.1.
inline constexpr std::uint32_t version1 = 1;
inline constexpr std::uint32_t version2 = 2;

/// Version PDU, [MS-RDPEAI] 2.2.2.1 (both directions).
struct Version {
    std::uint32_t version = version2;
    friend bool operator==(const Version&, const Version&) = default;
};

/// Sound Formats PDU, [MS-RDPEAI] 2.2.2.2 (both directions). ExtraData is
/// ignored on receipt and never sent.
struct Formats {
    std::vector<rdpsnd::AudioFormat> formats;
    friend bool operator==(const Formats&, const Formats&) = default;
};

/// Open PDU, [MS-RDPEAI] 2.2.2.3: record `initial_format` (an index into the
/// client's format list), capturing from the device in `capture_format`.
struct Open {
    std::uint32_t frames_per_packet = 0;
    std::uint32_t initial_format = 0;
    rdpsnd::AudioFormat capture_format;
    friend bool operator==(const Open&, const Open&) = default;
};

/// Open Reply PDU, [MS-RDPEAI] 2.2.2.4: an HRESULT.
struct OpenReply {
    std::uint32_t result = 0;
    friend bool operator==(const OpenReply&, const OpenReply&) = default;
};

/// Incoming Data PDU, [MS-RDPEAI] 2.2.3.1.
struct IncomingData {
    friend bool operator==(const IncomingData&, const IncomingData&) = default;
};

/// Data PDU, [MS-RDPEAI] 2.2.3.2. Points into the decoded message.
struct Data {
    std::span<const std::byte> data;
};

/// Format Change PDU, [MS-RDPEAI] 2.2.4.1 (both directions).
struct FormatChange {
    std::uint32_t new_format = 0;
    friend bool operator==(const FormatChange&, const FormatChange&) = default;
};

using ServerPdu = std::variant<Version, Formats, Open, FormatChange>;
using ClientPdu = std::variant<Version, Formats, OpenReply, IncomingData, Data, FormatChange>;

/// WAVEFORMAT_EXTENSIBLE for 16-bit PCM with `channels` (1 or 2) at `rate`,
/// the capture format of an Open PDU ([MS-RDPEAI] 2.2.2.3.1).
[[nodiscard]] rdpsnd::AudioFormat extensible_pcm_format(std::uint32_t rate, std::uint16_t channels);

[[nodiscard]] std::vector<std::byte> encode_server_pdu(const ServerPdu& pdu);
[[nodiscard]] std::vector<std::byte> encode_client_pdu(const ClientPdu& pdu);
/// Decodes a client PDU. Strict: fields must fill the message exactly
/// (except the ExtraData of a Sound Formats PDU, whose start the client's
/// cbSizeFormatsPacket must point to), and PDUs only the server sends are errors.
[[nodiscard]] Result<ClientPdu> decode_client_pdu(std::span<const std::byte> message);
[[nodiscard]] Result<ServerPdu> decode_server_pdu(std::span<const std::byte> message);

}  // namespace farland::channels::audin

namespace farland::channels {

namespace audin_event {
/// The client is recording: Data events in `format` follow.
struct Opened {
    rdpsnd::AudioFormat format;
    std::uint32_t frames_per_packet = 0;
};
/// Audio in the current format, points into the message given to receive().
struct Data {
    std::span<const std::byte> data;
};
/// The client could not open its microphone, or has no format in common with the server.
struct Failed {
    std::string_view reason;
    std::uint32_t result = 0;  ///< the HRESULT from the Open Reply PDU, if any
};
}  // namespace audin_event

using AudinEvent = std::variant<audin_event::Opened, audin_event::Data, audin_event::Failed>;

struct AudinServerConfig {
    /// Offered in this order, which is also the order of preference; linear
    /// PCM only, at least one, all distinct.
    std::vector<rdpsnd::AudioFormat> formats;
    /// Milliseconds of audio in each Data PDU (FramesPerPacket).
    std::uint32_t packet_ms = 20;
};

/// The server side of [MS-RDPEAI], sans-IO. start() sends the Version PDU;
/// the client's Version brings the Sound Formats PDU, the client's formats
/// the Open PDU for the most preferred format they share, and a successful
/// Open Reply the Opened event; Data PDUs then become Data events.
///
/// [MS-RDPEAI] 3.1.5 wants out-of-sequence PDUs ignored, and they are
/// (Incoming Data PDUs, which are diagnostic only, too). Malformed PDUs, a
/// client format the server did not offer, or Data of the wrong size are
/// errors, final like those of the other channel servers.
class AudinServer {
public:
    explicit AudinServer(AudinServerConfig config);

    void start();
    /// Processes one client message. Data events point into `message`, so
    /// poll them before it goes away.
    [[nodiscard]] Result<void> receive(std::span<const std::byte> message);

    [[nodiscard]] std::vector<std::vector<std::byte>> take_output() { return std::exchange(output_, {}); }
    [[nodiscard]] std::optional<AudinEvent> poll_event();

    [[nodiscard]] bool opened() const noexcept { return state_ == State::opened; }
    [[nodiscard]] std::uint32_t client_version() const noexcept { return client_version_; }

private:
    enum class State : std::uint8_t { idle, waiting_version, waiting_formats, waiting_reply, opened, failed };

    [[nodiscard]] Result<void> handle(const audin::Version& pdu);
    [[nodiscard]] Result<void> handle(const audin::Formats& pdu);
    [[nodiscard]] Result<void> handle(const audin::OpenReply& pdu);
    [[nodiscard]] Result<void> handle(const audin::IncomingData& pdu);
    [[nodiscard]] Result<void> handle(const audin::Data& pdu);
    [[nodiscard]] Result<void> handle(const audin::FormatChange& pdu);

    AudinServerConfig config_;
    State state_ = State::idle;
    std::uint32_t client_version_ = 0;
    std::vector<rdpsnd::AudioFormat> client_formats_;
    std::uint32_t current_format_ = 0;
    std::uint32_t frames_per_packet_ = 0;
    std::vector<std::vector<std::byte>> output_;
    std::deque<AudinEvent> events_;
    std::optional<Error> error_;
};

}  // namespace farland::channels
