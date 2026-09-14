// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/audio/pcm.hpp>
#include <farland/channels/audin.hpp>
#include <farland/server/dynamic_channels.hpp>

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <variant>
#include <vector>

/// The client's microphone ([MS-RDPEAI]) over the AUDIO_INPUT dynamic
/// channel: opens the channel, negotiates 16-bit PCM and turns the client's
/// Data PDUs into samples for a local audio source.
namespace farland::server {

namespace input_event {
/// The client records; Samples in `format` follow.
struct Opened {
    audio::PcmFormat format;
};
/// Interleaved samples in the Opened format.
struct Samples {
    std::vector<std::int16_t> samples;
};
/// The microphone is gone: refused, failed or closed by the client.
struct Closed {
    std::string reason;
};
}  // namespace input_event

using InputEvent = std::variant<input_event::Opened, input_event::Samples, input_event::Closed>;

class AudioInput {
public:
    /// Offered in this order of preference: mono first (microphones are),
    /// 48 and 44.1 kHz, then stereo, then lower rates.
    [[nodiscard]] static std::vector<audio::PcmFormat> default_formats();

    /// Opens AUDIO_INPUT on `channels` (whose capabilities must be ready),
    /// which must outlive this object.
    explicit AudioInput(DynamicChannels& channels, const std::vector<audio::PcmFormat>& formats = default_formats());

    /// Handles `event` if it concerns the AUDIO_INPUT channel. False otherwise.
    bool handle(const channels::DvcEvent& event);
    [[nodiscard]] std::optional<InputEvent> poll_event();

    [[nodiscard]] bool closed() const noexcept { return closed_; }

private:
    void flush();
    void close(std::string reason);

    DynamicChannels* channels_;
    channels::AudinServer audin_;
    std::uint32_t channel_id_ = 0;
    bool closed_ = false;
    std::deque<InputEvent> events_;
};

}  // namespace farland::server
