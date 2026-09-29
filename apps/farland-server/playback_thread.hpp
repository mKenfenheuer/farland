// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "wake_pipe.hpp"

#include <farland/base/error.hpp>
#include <farland/platform/audio.hpp>
#include <farland/server/audio_playback.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace farland::app {

/// The session's audio output on a thread that does nothing else: it
/// negotiates the format with the client, captures the desktop's sound, cuts,
/// encodes and paces it (server::AudioPlayback) and counts what went out. It
/// never waits for the rest of the session -- a frame encoding, a slow
/// clipboard -- so the client's buffer never runs dry for want of the next
/// packet.
///
/// The connection thread carries the rdpsnd messages both ways: it hands the
/// client's to receive(), and when wake_fd() is readable it sends what
/// take_output() returns, on whichever channel audio runs over.
class PlaybackThread {
public:
    /// Opens the capture of the desktop's audio output in `format`.
    using OpenCapture = std::function<Result<std::unique_ptr<platform::AudioSource>>(audio::PcmFormat format)>;

    /// What the audio thread produced since the last take_output().
    struct Output {
        /// rdpsnd messages, in the order they go out.
        std::vector<std::vector<std::byte>> messages;
        /// Set once, when audio output ended for good; the reason.
        std::optional<std::string> stopped;
    };

    /// Starts the thread, which sends the Server Audio Formats PDU at once.
    /// `transport` names the channel for the log.
    PlaybackThread(std::string peer, std::string transport, server::AudioPlaybackOptions options,
                   OpenCapture open_capture);
    /// Stops the capture and the thread.
    ~PlaybackThread();
    PlaybackThread(const PlaybackThread&) = delete;
    PlaybackThread& operator=(const PlaybackThread&) = delete;
    PlaybackThread(PlaybackThread&&) = delete;
    PlaybackThread& operator=(PlaybackThread&&) = delete;

    /// One reassembled rdpsnd message from the client.
    void receive(std::vector<std::byte> message);
    /// What network auto-detect measured, for the Opus bitrate.
    void set_bandwidth(std::optional<std::uint32_t> kbps);
    /// Readable while take_output() has something.
    [[nodiscard]] int wake_fd() const noexcept { return outbox_wake_.fd(); }
    [[nodiscard]] Output take_output();

private:
    using Clock = std::chrono::steady_clock;

    void run();
    /// On the audio thread: what AudioPlayback reported.
    void poll_playback();
    void stop(std::string why);
    void log_statistics(Clock::time_point now);

    std::string peer_;
    std::string transport_;
    OpenCapture open_capture_;

    // The audio thread's alone.
    std::optional<server::AudioPlayback> playback_;
    std::unique_ptr<platform::AudioSource> capture_;
    std::vector<std::int16_t> samples_;
    Clock::time_point statistics_since_;
    bool stopped_ = false;

    // Shared with the connection thread.
    std::mutex mutex_;
    std::deque<std::vector<std::byte>> inbox_;
    std::optional<std::uint32_t> bandwidth_kbps_;
    bool quit_ = false;
    Output outbox_;
    WakePipe inbox_wake_;
    WakePipe outbox_wake_;
    std::thread thread_;  ///< last: starts once the rest is there
};

}  // namespace farland::app
