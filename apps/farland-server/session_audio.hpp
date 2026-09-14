// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/channels/dvc_server.hpp>
#include <farland/channels/svc.hpp>
#include <farland/platform/audio.hpp>
#include <farland/server/audio_input.hpp>
#include <farland/server/audio_playback.hpp>
#include <farland/server/connection.hpp>
#include <farland/server/dynamic_channels.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <vector>

namespace farland::app {

struct AudioOptions {
    /// The desktop's audio output plays on the client ([MS-RDPEA]).
    bool playback = true;
    /// The client's microphone becomes a local source ([MS-RDPEAI]).
    bool microphone = true;
};

/// The audio of one session, on the session thread.
///
/// Playback goes over the AUDIO_PLAYBACK_DVC dynamic channel, as Windows
/// servers and gnome-remote-desktop do, or over the "rdpsnd" static channel
/// when the client refuses that (or has no drdynvc). The desktop's output is
/// captured only while a client plays it. The microphone runs over
/// AUDIO_INPUT, and the local source exists only while the client records.
class SessionAudio {
public:
    /// Sends one Virtual Channel PDU on a static channel.
    using SendStatic = std::function<void(std::uint16_t channel_id, std::span<const std::byte> chunk)>;

    SessionAudio(std::string peer, AudioOptions options);

    /// On activation, with what the client asked for (Client Info flags and
    /// static channels). Without drdynvc, playback starts on "rdpsnd" now.
    void start(const server::Session& session, SendStatic send_static);
    /// The dynamic channels are ready: opens the audio channels on them.
    /// `channels` must outlive this object.
    void dynamic_channels_ready(server::DynamicChannels& channels);
    /// Handles a dynamic channel event for an audio channel. False otherwise.
    bool handle(const channels::DvcEvent& event);
    /// Handles data on the "rdpsnd" static channel. False for other channels.
    bool receive_static(std::uint16_t channel_id, std::span<const std::byte> pdu);

    /// Descriptors whose readiness calls for service().
    void add_fds(std::vector<pollfd>& fds) const;
    /// Moves captured audio to the client. `bandwidth_kbps` is auto-detect's measurement.
    void service(std::optional<std::uint32_t> bandwidth_kbps);

private:
    enum class Transport : std::uint8_t { none, dynamic, static_channel };

    void start_playback(Transport transport);
    void stop_playback(const std::string& why);
    void send_playback(std::span<const std::byte> message);
    void on_playback_message(std::span<const std::byte> message);
    void poll_playback();
    void poll_input();
    void log_statistics(std::chrono::steady_clock::time_point now);

    std::string peer_;
    AudioOptions options_;
    SendStatic send_static_;
    bool want_playback_ = false;
    bool want_microphone_ = false;
    std::optional<std::uint16_t> rdpsnd_channel_;
    channels::svc::Reassembler rdpsnd_reassembler_;
    server::DynamicChannels* dvc_ = nullptr;
    std::optional<std::uint32_t> playback_dvc_id_;
    Transport transport_ = Transport::none;
    std::optional<server::AudioPlayback> playback_;
    std::unique_ptr<platform::AudioSource> capture_;
    std::vector<std::int16_t> samples_;
    std::optional<server::AudioInput> input_;
    std::unique_ptr<platform::AudioSink> microphone_;
    std::uint64_t microphone_samples_ = 0;
    std::chrono::steady_clock::time_point statistics_since_;
};

}  // namespace farland::app
