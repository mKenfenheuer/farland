// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/audio/pcm.hpp>
#include <farland/base/error.hpp>
#include <farland/channels/rdpsnd_server.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

/// Audio output of one session ([MS-RDPEA], docs/PLAN.md §3.1): negotiates a
/// format, cuts the captured PCM into packets, encodes them and paces them by
/// the client's Wave Confirm PDUs. Sans-IO: the caller moves the messages
/// over the "rdpsnd" static channel or the AUDIO_PLAYBACK_DVC dynamic one,
/// and supplies the PCM (in capture_format(), once ready).
namespace farland::server {

namespace playback_event {
/// Audio can flow: capture PCM in `capture` and push() it.
struct Ready {
    audio::PcmFormat capture;
    std::string codec;  ///< for logs, e.g. "Opus 48000 Hz stereo at 96 kbit/s"
};
/// The client cannot play audio (the capture need not start).
struct Unavailable {
    std::string reason;
};
}  // namespace playback_event

using PlaybackEvent = std::variant<playback_event::Ready, playback_event::Unavailable>;

/// Creates an Opus encoder for `format` with packets of `duration` at `bitrate`.
using OpusFactory = std::function<Result<std::unique_ptr<audio::Encoder>>(
    audio::PcmFormat format, std::chrono::milliseconds duration, std::uint32_t bitrate)>;

struct AudioPlaybackOptions {
    /// Offers Opus 48 kHz stereo when set and it creates an encoder; PCM only otherwise.
    OpusFactory make_opus;
    /// Audio per packet (one Wave2 PDU, or one WaveInfo and Wave pair).
    std::chrono::milliseconds packet_duration{20};
    /// Flow control: audio sent but not confirmed may exceed the lowest amount
    /// seen over the last seconds by this much before packets are dropped. The
    /// lowest amount is the client's own buffering (clients that confirm after
    /// playing) plus the network round trip.
    std::chrono::milliseconds max_backlog{100};
    /// Unconfirmed audio allowed before the first confirmation, and ever.
    std::chrono::milliseconds startup_limit{400};
    std::chrono::milliseconds hard_limit{1000};
    /// Samples unconfirmed for this long count as lost (the client never
    /// confirms them), so they stop holding back the stream.
    std::chrono::milliseconds confirm_timeout{2000};
    /// Digital silence for this long stops the stream (with a Close PDU)
    /// until sound comes back; nothing is sent in between.
    std::chrono::milliseconds silence_timeout{1000};
};

/// What happened since the last take_stats().
struct PlaybackStats {
    std::uint64_t packets_sent = 0;
    std::uint64_t packets_dropped = 0;  ///< flow control
    std::uint64_t packets_silent = 0;   ///< suppressed silence
    std::uint64_t bytes_sent = 0;
    /// Longest time from sending a sample to its first confirmation.
    std::optional<std::chrono::milliseconds> max_round_trip;
    /// Largest delay the client reported in a Wave Confirm's wTimeStamp
    /// (FreeRDP adds its render latency in a second confirmation).
    std::optional<std::chrono::milliseconds> max_client_delay;
    std::chrono::milliseconds unconfirmed{0};  ///< now
};

class AudioPlayback {
public:
    using Clock = std::chrono::steady_clock;
    /// Sends one rdpsnd message on the channel in use.
    using SendMessage = std::function<void(std::span<const std::byte> message)>;

    AudioPlayback(SendMessage send, AudioPlaybackOptions options = {});

    /// Sends the Server Audio Formats PDU; `now` starts the wTimeStamp clock.
    void start(Clock::time_point now);
    /// One reassembled rdpsnd message from the client. An error ends audio
    /// output; the caller should close the channel.
    [[nodiscard]] Result<void> receive(std::span<const std::byte> message, Clock::time_point now);
    [[nodiscard]] std::optional<PlaybackEvent> poll_event();

    /// Interleaved samples in capture_format(); ignored before Ready.
    void push(std::span<const std::int16_t> samples, Clock::time_point now);
    /// The bandwidth network auto-detect measured, for the Opus bitrate in
    /// dynamic quality mode.
    void set_bandwidth(std::optional<std::uint32_t> kbps);

    [[nodiscard]] bool ready() const noexcept { return format_no_.has_value(); }
    [[nodiscard]] std::optional<audio::PcmFormat> capture_format() const;
    [[nodiscard]] PlaybackStats take_stats(Clock::time_point now);
    /// The formats offered, in order (Opus first when available).
    [[nodiscard]] const std::vector<channels::rdpsnd::AudioFormat>& offered_formats() const noexcept
    {
        return offered_;
    }

    /// The Opus format as gnome-remote-desktop offers it and FreeRDP accepts it.
    [[nodiscard]] static channels::rdpsnd::AudioFormat opus_format();

private:
    struct InFlight {
        std::uint8_t block = 0;
        Clock::time_point sent;
    };

    void choose_format(const channels::rdpsnd_event::Ready& ready);
    void on_confirm(const channels::rdpsnd_event::WaveConfirmed& confirm, Clock::time_point now);
    void send_packet(std::span<const std::int16_t> packet, Clock::time_point now);
    [[nodiscard]] bool should_drop(Clock::time_point now);
    [[nodiscard]] std::chrono::milliseconds unconfirmed() const;
    [[nodiscard]] std::uint32_t opus_bitrate() const;
    [[nodiscard]] std::uint32_t millis(Clock::time_point t) const;
    void flush();

    SendMessage send_;
    AudioPlaybackOptions options_;
    std::unique_ptr<audio::Encoder> opus_;  ///< before offered_, which depends on it
    std::vector<channels::rdpsnd::AudioFormat> offered_;
    channels::RdpsndServer rdpsnd_;
    Clock::time_point epoch_;
    std::deque<PlaybackEvent> events_;

    std::optional<std::uint16_t> format_no_;  ///< the client format in use, once ready
    audio::PcmFormat capture_;
    bool use_opus_ = false;
    std::uint16_t quality_mode_ = channels::rdpsnd::quality::dynamic;
    std::optional<std::uint32_t> bandwidth_kbps_;
    std::vector<std::int16_t> pending_;

    std::deque<InFlight> in_flight_;
    /// Lowest unconfirmed audio seen at a confirmation, in this and the last window.
    std::optional<std::chrono::milliseconds> floor_current_;
    std::optional<std::chrono::milliseconds> floor_previous_;
    Clock::time_point floor_window_start_;
    std::chrono::milliseconds silent_{0};
    bool idle_ = false;  ///< stopped for silence; the Close PDU went out
    PlaybackStats stats_;
};

}  // namespace farland::server
