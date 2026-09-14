// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/audio/pcm.hpp>
#include <farland/base/error.hpp>
#include <farland/platform/audio.hpp>

#include <chrono>
#include <memory>
#include <string>

/// Session audio through the user's own PipeWire daemon (not the portal's
/// remote, which carries only the screen cast): the default sink's monitor
/// for the client's speakers, and a virtual source for its microphone.
/// PipeWire converts between these streams' format and the graph's.
namespace farland::platform::portal {

struct AudioCaptureOptions {
    /// node.name and node.description of the capture stream.
    std::string node_name = "farland-audio-capture";
    std::string description = "farland audio output capture";
    /// PipeWire quantum asked for (node.latency), which bounds the delay the
    /// capture adds.
    std::chrono::milliseconds quantum{10};
    /// Samples the session has not read are dropped beyond this, oldest first.
    std::chrono::milliseconds max_buffered{200};
};

/// Records what the default audio output plays (stream.capture.sink), and
/// follows the default output when the user switches it.
[[nodiscard]] Result<std::unique_ptr<AudioSource>> capture_default_sink(audio::PcmFormat format,
                                                                        const AudioCaptureOptions& options = {});

struct VirtualSourceOptions {
    /// node.name and node.description, as applications list the microphone.
    std::string node_name = "farland-microphone";
    std::string description = "farland microphone";
    std::chrono::milliseconds quantum{10};
    /// Queued samples beyond this are dropped, oldest first.
    std::chrono::milliseconds max_buffered{100};
    /// Samples collected before playout starts, against network jitter.
    std::chrono::milliseconds prebuffer{40};
};

/// A virtual audio source (a stream node with media.class Audio/Source) that
/// local applications can record from; it goes away with the object.
[[nodiscard]] Result<std::unique_ptr<AudioSink>> create_virtual_source(audio::PcmFormat format,
                                                                       const VirtualSourceOptions& options = {});

}  // namespace farland::platform::portal
