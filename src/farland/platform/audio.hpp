// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

/// The audio half of a platform backend (docs/PLAN.md §3.3): what the local
/// desktop plays, for the client's speakers, and a local microphone fed by
/// the client's. Samples are interleaved signed 16-bit PCM in the format the
/// object was created with. Both run a thread of their own; the session
/// thread uses them as below.
namespace farland::platform {

/// Audio captured from the desktop.
class AudioSource {
public:
    AudioSource() = default;
    AudioSource(const AudioSource&) = delete;
    AudioSource& operator=(const AudioSource&) = delete;
    AudioSource(AudioSource&&) = delete;
    AudioSource& operator=(AudioSource&&) = delete;
    virtual ~AudioSource() = default;

    /// Readable while samples wait; read() clears it. Stays readable once closed.
    [[nodiscard]] virtual int wake_fd() const = 0;
    /// Appends every waiting sample to `out`.
    virtual void read(std::vector<std::int16_t>& out) = 0;
    [[nodiscard]] virtual bool closed() const = 0;
    /// Why it closed; empty while open.
    [[nodiscard]] virtual std::string error() const = 0;
};

/// A microphone that desktop applications record from.
class AudioSink {
public:
    AudioSink() = default;
    AudioSink(const AudioSink&) = delete;
    AudioSink& operator=(const AudioSink&) = delete;
    AudioSink(AudioSink&&) = delete;
    AudioSink& operator=(AudioSink&&) = delete;
    virtual ~AudioSink() = default;

    /// Queues samples for the applications recording. What they do not take
    /// in time is dropped, oldest first, so that the delay stays short.
    virtual void write(std::span<const std::int16_t> samples) = 0;
    [[nodiscard]] virtual bool closed() const = 0;
    [[nodiscard]] virtual std::string error() const = 0;
};

}  // namespace farland::platform
