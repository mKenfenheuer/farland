// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

/// Where the time goes between the compositor drawing a frame and the client
/// acknowledging it (docs/ROADMAP.md M4: glass-to-glass at most 50 ms on a
/// LAN). The exit criterion is about two screens, and a server cannot see
/// either of them, so this measures the part farland is answerable for and
/// says plainly what is left over:
///
///   captured ──► taken ──► encoded ──► sent ──────────► acknowledged
///            (1)       (2)         (3)              (4)
///
/// 1. **waiting** -- the frame sat in PipeWire until the scheduler wanted it.
///    At a frame-rate cap this is mostly the cap, not a fault.
/// 2. **reading** -- copying or mapping the pixels.
/// 3. **encoding** -- the codec, and building the PDUs.
/// 4. **client** -- the wire, the client's decode, and its acknowledgement.
///    One network round trip is inside this; `round_trip` is measured
///    separately (Network Characteristics Detection) so the two can be told
///    apart.
///
/// What no server can measure: the client's own compositor putting the
/// decoded frame on the screen, and the screen's own delay. A number from
/// here is glass-to-glass minus those, and is reported as such.
///
/// It keeps every sample of a run rather than a smoothed value, because the
/// question is what the worst frames do, not the average.
namespace farland::server {

class LatencyTracker {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::nanoseconds;

    /// One frame's journey. Durations are between the stages above.
    struct Sample {
        Duration waiting{};
        Duration reading{};
        Duration encoding{};
        Duration client{};
        /// waiting + reading + encoding: what the server added before the
        /// first byte went out.
        [[nodiscard]] Duration server() const noexcept { return waiting + reading + encoding; }
        /// Everything measured: the server's part plus the client's answer.
        [[nodiscard]] Duration total() const noexcept { return server() + client; }
    };

    struct Summary {
        std::size_t frames = 0;
        /// Frames whose source gave no capture time, so `waiting` is unknown
        /// and the measurement starts where the frame was taken.
        std::size_t without_capture_time = 0;
        Duration waiting_p50{}, waiting_p95{};
        Duration reading_p50{}, reading_p95{};
        Duration encoding_p50{}, encoding_p95{};
        Duration client_p50{}, client_p95{};
        Duration server_p50{}, server_p95{}, server_max{};
        Duration total_p50{}, total_p95{}, total_max{};
    };

    /// `capacity` samples are kept; the oldest go first.
    explicit LatencyTracker(std::size_t capacity = 4096) : capacity_(capacity) {}

    /// A frame was taken from the source. `captured` is when the compositor
    /// made it, where the source knows.
    void frame_taken(Clock::time_point now, std::optional<Clock::time_point> captured);
    /// Its pixels are readable (mapped or copied).
    void frame_read(Clock::time_point now);
    /// It is encoded and its PDUs are built.
    void frame_encoded(Clock::time_point now);
    /// It went to the transport, as `frame_id` in the GFX stream.
    void frame_sent(std::uint32_t frame_id, Clock::time_point now);
    /// The client acknowledged it. An id that was never sent is ignored, and
    /// so is one whose frame has already been forgotten.
    void frame_acknowledged(std::uint32_t frame_id, Clock::time_point now);

    [[nodiscard]] std::size_t frames() const noexcept { return samples_.size(); }
    /// Percentiles over what has been collected. Empty runs give zeroes.
    [[nodiscard]] Summary summary() const;

    /// The summary as one line per stage, for a log or a report.
    [[nodiscard]] static std::string describe(const Summary& summary);

private:
    /// A frame being timed, from taken until acknowledged.
    struct Pending {
        std::uint32_t frame_id = 0;
        Clock::time_point taken;
        std::optional<Clock::time_point> captured;
        Clock::time_point read;
        Clock::time_point encoded;
        Clock::time_point sent;
        bool has_read = false;
        bool has_encoded = false;
    };

    /// The frame being timed now, before it is sent.
    std::optional<Pending> current_;
    /// Sent, waiting for an acknowledgement. Few at a time
    /// (max_frames_in_flight), so a deque is enough.
    std::deque<Pending> in_flight_;
    std::vector<Sample> samples_;
    std::size_t capacity_;
    std::size_t without_capture_time_ = 0;
};

}  // namespace farland::server
