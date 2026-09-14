// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/channels/rdpsnd.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

/// The server side of [MS-RDPEA] over a virtual channel, sans-IO: the
/// initialization sequence (formats, quality mode, training) and the data
/// transfer sequence (Wave2, or WaveInfo and Wave, and Wave Confirm).
namespace farland::channels {

namespace rdpsnd_event {
/// The initialization sequence is done; audio may be sent.
struct Ready {
    std::uint16_t client_version = 0;
    std::uint32_t client_flags = 0;  ///< rdpsnd::caps::*
    /// The formats both sides support; wave format numbers index this list.
    std::vector<rdpsnd::AudioFormat> formats;
    /// From the Quality Mode PDU, when the client sent one before the Training Confirm.
    std::optional<std::uint16_t> quality_mode;
    /// Milliseconds from the Training PDU to its confirmation, by the caller's clock.
    std::uint32_t training_round_trip_ms = 0;
};
/// The client cannot play audio: it did not set TSSNDCAPS_ALIVE or has no
/// format in common with the server.
struct Unavailable {
    std::string_view reason;
};
/// A Wave Confirm PDU. `timestamp` is the client's wTimeStamp (the sample's
/// wTimeStamp plus the time the client took, [MS-RDPEA] 3.2.5.2.1.6).
struct WaveConfirmed {
    std::uint8_t block_no = 0;
    std::uint16_t timestamp = 0;
    /// wTimeStamp of the sample with this block number as sent, if known.
    std::optional<std::uint16_t> sent_timestamp;
};
}  // namespace rdpsnd_event

using RdpsndEvent = std::variant<rdpsnd_event::Ready, rdpsnd_event::Unavailable, rdpsnd_event::WaveConfirmed>;

struct RdpsndServerConfig {
    /// Offered in this order; at least one, all distinct.
    std::vector<rdpsnd::AudioFormat> formats;
    std::uint16_t version = rdpsnd::version::windows_8;
    /// Zero bytes in the Training PDU (0: none, wPackSize 0).
    std::uint16_t training_data_size = 0;
};

/// Input is `receive()` with one reassembled client message; output is
/// `take_output()`, the messages to send in order (for the static channel each
/// goes through svc::encode_chunks, for AUDIO_PLAYBACK_DVC each is one DVC
/// message). Time comes from the caller as a millisecond clock, which also
/// provides the 16-bit wTimeStamp values.
///
/// Sequencing ([MS-RDPEA] 3.3.5): the server speaks first. The Client Audio
/// Formats PDU is expected before anything else, and its formats must come
/// from the server's list. The Training PDU goes out right after it; the
/// Quality Mode PDU that version 6 clients send may come before or after the
/// Training Confirm. A second Client Formats PDU, a Training Confirm without
/// a Training, or a Wave Confirm before Ready are errors. Errors are final:
/// receive() returns them now and later, and the caller should stop audio.
class RdpsndServer {
public:
    explicit RdpsndServer(RdpsndServerConfig config);

    /// Queues the Server Audio Formats and Version PDU.
    void start();
    /// Processes one client message received at `now_ms`.
    [[nodiscard]] Result<void> receive(std::span<const std::byte> message, std::uint32_t now_ms);

    /// Queues one audio sample in the client format `format_no` (an index into
    /// Ready::formats) and returns its block number. Wave2 when both sides
    /// have version 8, otherwise WaveInfo and Wave (then the sample must be
    /// longer than 4 bytes). At most 65527 bytes. Only when ready().
    std::uint8_t send_wave(std::uint16_t format_no, std::span<const std::byte> data, std::uint32_t now_ms,
                           std::uint32_t audio_timestamp_ms);
    /// Queues a Close PDU (the audio stopped; a later sample restarts it).
    void send_close();
    /// Queues a Volume PDU if the client set TSSNDCAPS_VOLUME. Returns whether it did.
    bool send_volume(std::uint32_t volume);

    [[nodiscard]] std::vector<std::vector<std::byte>> take_output();
    [[nodiscard]] std::optional<RdpsndEvent> poll_event();

    [[nodiscard]] bool ready() const noexcept { return state_ == State::ready; }
    [[nodiscard]] bool failed() const noexcept { return error_.has_value(); }
    [[nodiscard]] bool uses_wave2() const noexcept;
    /// The negotiated client formats (empty before the Client Formats PDU).
    [[nodiscard]] const std::vector<rdpsnd::AudioFormat>& client_formats() const noexcept { return client_.formats; }
    [[nodiscard]] std::uint16_t client_version() const noexcept { return client_.version; }
    [[nodiscard]] std::uint32_t client_flags() const noexcept { return client_.flags; }

private:
    enum class State : std::uint8_t { idle, waiting_formats, training, ready, unavailable };

    [[nodiscard]] Result<void> handle(const rdpsnd::ClientFormats& pdu, std::uint32_t now_ms);
    [[nodiscard]] Result<void> handle(const rdpsnd::QualityMode& pdu, std::uint32_t now_ms);
    [[nodiscard]] Result<void> handle(const rdpsnd::TrainingConfirm& pdu, std::uint32_t now_ms);
    [[nodiscard]] Result<void> handle(const rdpsnd::WaveConfirm& pdu, std::uint32_t now_ms);

    void queue(const rdpsnd::ServerPdu& pdu);

    RdpsndServerConfig config_;
    State state_ = State::idle;
    rdpsnd::ClientFormats client_;
    std::optional<std::uint16_t> quality_mode_;
    std::uint16_t training_timestamp_ = 0;
    std::uint32_t training_sent_ms_ = 0;
    std::uint8_t last_block_ = 0;  ///< cLastBlockConfirmed, then the block number of the last sample
    /// wTimeStamp of each block number as last sent.
    std::array<std::optional<std::uint16_t>, 256> sent_timestamps_{};
    std::vector<std::vector<std::byte>> output_;
    std::deque<RdpsndEvent> events_;
    std::optional<Error> error_;
};

}  // namespace farland::channels
