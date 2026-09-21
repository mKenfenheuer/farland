// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

/// When to send the next frame (docs/PLAN.md §3.4): only when something
/// changed, no faster than the frame-rate cap, and only while fewer than
/// `max_frames_in_flight` frames await the client's acknowledgement
/// ([MS-RDPEGFX] 3.2.5.13 RDPGFX_FRAME_ACKNOWLEDGE_PDU). Damage that arrives
/// while the window is full is not lost: it stays pending and the next frame
/// covers all of it, so a slow client gets fewer, more complete frames rather
/// than a growing backlog.
///
/// The scheduler keeps no clock of its own; every call takes the time.
namespace farland::server {

class FrameScheduler {
public:
    using Clock = std::chrono::steady_clock;

    struct Config {
        unsigned max_fps = 30;
        /// Frames sent but not acknowledged before the scheduler waits.
        std::size_t max_frames_in_flight = 2;
        /// A frame without an acknowledgement for this long no longer counts
        /// as in flight, so a lost acknowledgement cannot stall the output.
        /// This is a floor: the effective timeout follows the measured round
        /// trip (ack_timeout_rtt_factor), because on a link slower than the
        /// floor every frame would expire before its acknowledgement could
        /// arrive and the window would stop limiting the output at all --
        /// exactly when limiting it matters most.
        Clock::duration ack_timeout = std::chrono::seconds(1);
        /// Multiple of the smoothed round trip a frame may be outstanding for
        /// before it is given up on, once a round trip is known.
        unsigned ack_timeout_rtt_factor = 4;
        /// Upper bound on the effective timeout, so a pathological round trip
        /// cannot hold the output shut indefinitely.
        Clock::duration max_ack_timeout = std::chrono::seconds(10);
        /// Whether the transport acknowledges frames at all (GFX does; legacy
        /// bitmap updates do not).
        bool acknowledgements = true;
    };

    explicit FrameScheduler(Config config);

    /// The screen changed; a frame is needed.
    void damage() noexcept { pending_ = true; }
    /// A frame was sent now; its damage is consumed.
    void frame_sent(std::uint32_t frame_id, Clock::time_point now);
    /// The client acknowledged `frame_id`. Unknown IDs are ignored.
    void frame_acknowledged(std::uint32_t frame_id, Clock::time_point now);
    /// The client asked the server to stop waiting for acknowledgements
    /// (queueDepth SUSPEND_FRAME_ACKNOWLEDGEMENT), or to resume.
    void set_acknowledgements_suspended(bool suspended) noexcept;
    /// Changes the frame-rate cap (a new quality tier).
    void set_max_fps(unsigned fps) noexcept;

    /// A frame should be encoded and sent now.
    [[nodiscard]] bool due(Clock::time_point now);
    /// How long the caller may sleep before due() can change, or nullopt when
    /// only new damage or an acknowledgement can make a frame due.
    [[nodiscard]] std::optional<Clock::duration> wait(Clock::time_point now);

    [[nodiscard]] bool pending() const noexcept { return pending_; }
    [[nodiscard]] std::size_t frames_in_flight() const noexcept { return in_flight_.size(); }
    /// Smoothed time from sending a frame to its acknowledgement, once known.
    [[nodiscard]] std::optional<Clock::duration> round_trip() const noexcept { return round_trip_; }

private:
    struct InFlight {
        std::uint32_t frame_id;
        Clock::time_point sent;
    };

    [[nodiscard]] bool gated_by_acks() const noexcept { return config_.acknowledgements && !suspended_; }
    /// Forgets frames whose acknowledgement is overdue.
    void expire(Clock::time_point now);
    /// How long a frame may wait for its acknowledgement: the configured
    /// floor, or a multiple of the measured round trip where that is slower.
    [[nodiscard]] Clock::duration ack_timeout() const noexcept;

    Config config_;
    Clock::duration interval_;
    bool pending_ = false;
    bool suspended_ = false;
    std::optional<Clock::time_point> last_sent_;
    std::deque<InFlight> in_flight_;
    std::optional<Clock::duration> round_trip_;
};

}  // namespace farland::server
