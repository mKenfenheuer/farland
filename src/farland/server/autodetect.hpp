// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/proto/autodetect.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

/// The server half of network characteristics detection ([MS-RDPBCGR] 1.3.9,
/// 3.3.5.14): which auto-detect requests to send when, and what the client's
/// answers say about the link. server::Connection wraps the requests in PDUs
/// and feeds the responses back; this class only keeps the schedule and the
/// estimates. It keeps no clock of its own; every call takes the time.
///
/// - Connect-time detection: one RTT probe, then a burst of Bandwidth
///   Measure Payloads between a Start and a Stop, answered with the time the
///   client took to receive it.
/// - Continuous detection: an RTT probe every `rtt_interval`, and from time to
///   time a Bandwidth Measure Start/Stop around a large burst of real output
///   (the spec replaces the payload PDUs with ordinary traffic once the
///   connection is up, [MS-RDPBCGR] 2.2.14.2.2).
namespace farland::server {

/// What the detection measured so far.
struct NetworkEstimate {
    using Clock = std::chrono::steady_clock;

    /// Smoothed round-trip time and its mean deviation, as TCP computes SRTT
    /// and RTTVAR (RFC 6298).
    std::optional<Clock::duration> rtt;
    Clock::duration jitter{};
    /// The newest sample, and the lowest of the recent ones (the link's RTT
    /// without queueing).
    std::optional<Clock::duration> last_rtt;
    std::optional<Clock::duration> base_rtt;
    /// How long the oldest RTT probe has waited for its answer; zero when
    /// none is outstanding. A stalled link shows here before any sample does.
    Clock::duration unanswered{};

    /// Bandwidth in kbit/s, smoothed (falls fast, rises slowly), and the
    /// newest measurement.
    std::optional<std::uint32_t> bandwidth_kbps;
    std::optional<std::uint32_t> last_bandwidth_kbps;
    /// When the newest bandwidth measurement was taken. Continuous
    /// measurements ride on a burst of real output, so a session with little
    /// to send keeps whatever connect time happened to measure -- which is
    /// the least reliable reading there is. A reader can tell how old it is
    /// and stop trusting it.
    std::optional<Clock::time_point> bandwidth_at;

    std::uint64_t rtt_samples = 0;
    std::uint64_t bandwidth_samples = 0;
};

class AutoDetect {
public:
    using Clock = std::chrono::steady_clock;

    struct Config {
        /// Continuous RTT probes this far apart.
        Clock::duration rtt_interval = std::chrono::seconds(1);
        /// A probe without an answer for this long is given up.
        Clock::duration rtt_timeout = std::chrono::seconds(5);
        /// A client that has not answered one of this many probes is taken
        /// not to answer continuous probes at all, and probing stops.
        unsigned max_unanswered_probes = 5;
        /// Samples the base RTT is the minimum of.
        std::size_t base_rtt_window = 120;
        /// Bytes of Bandwidth Measure Payload in the connect-time burst.
        std::size_t connect_time_bytes = std::size_t{64} * 1024;
        /// How long the connect-time detection waits for the answers.
        Clock::duration connect_time_timeout = std::chrono::seconds(2);
        /// Continuous bandwidth measurements at most this often, each around
        /// an output burst of at least `min_burst_bytes`.
        Clock::duration bandwidth_interval = std::chrono::seconds(2);
        std::size_t min_burst_bytes = std::size_t{32} * 1024;
    };

    explicit AutoDetect(Config config);
    AutoDetect() : AutoDetect(Config{}) {}

    // Connect-time detection ([MS-RDPBCGR] 1.3.1.1, phase 6) ---------------

    /// The requests of the connect-time detection, to send at once. Payload
    /// spans point into this object.
    [[nodiscard]] std::vector<proto::autodetect::Request> start_connect_time(Clock::time_point now);
    /// The connect-time detection has its answers, or waited long enough.
    [[nodiscard]] bool connect_time_complete(Clock::time_point now) const noexcept;
    /// The Network Characteristics Result that ends the connect-time
    /// detection, when there is a measurement to report.
    [[nodiscard]] std::optional<proto::autodetect::NetworkCharacteristicsResult> network_characteristics_result();

    // Continuous detection ------------------------------------------------------

    /// An RTT Measure Request to send now, if one is due. Also ages the
    /// outstanding probes.
    [[nodiscard]] std::optional<proto::autodetect::RttRequest> poll_rtt_request(Clock::time_point now);
    /// When poll_rtt_request() has the next probe, or nullopt once probing stopped.
    [[nodiscard]] std::optional<Clock::time_point> next_rtt_request() const noexcept;
    /// Whether an output burst of `burst_bytes` should be measured.
    [[nodiscard]] bool bandwidth_measurement_due(Clock::time_point now, std::size_t burst_bytes) const noexcept;
    /// The Start and Stop to send around the burst.
    [[nodiscard]] proto::autodetect::BandwidthStart start_bandwidth_measurement(Clock::time_point now);
    [[nodiscard]] proto::autodetect::BandwidthStop stop_bandwidth_measurement() const noexcept;

    /// A client answer. Answers to nothing the server asked are ignored.
    void on_response(const proto::autodetect::Response& response, Clock::time_point now);

    [[nodiscard]] const NetworkEstimate& estimate() const noexcept { return estimate_; }

private:
    struct Probe {
        std::uint16_t sequence = 0;
        Clock::time_point sent;
    };

    [[nodiscard]] std::uint16_t next_sequence() noexcept { return sequence_++; }
    void add_rtt_sample(Clock::duration sample);
    void add_bandwidth_sample(std::uint32_t kbps, Clock::time_point now);
    void expire_probes(Clock::time_point now);

    Config config_;
    NetworkEstimate estimate_;
    std::uint16_t sequence_ = 1;

    std::deque<Probe> probes_;
    std::deque<Clock::duration> recent_rtt_;
    std::optional<Clock::time_point> last_probe_;
    unsigned probes_lost_ = 0;
    bool probing_ = true;

    std::vector<std::byte> payload_;
    std::optional<Clock::time_point> connect_time_started_;
    bool connect_time_rtt_ = false;
    bool connect_time_bandwidth_ = false;

    /// The bandwidth measurement whose results are awaited.
    std::optional<std::uint16_t> bandwidth_sequence_;
    std::uint16_t bandwidth_response_type_ = 0;
    std::optional<Clock::time_point> last_bandwidth_measurement_;
};

}  // namespace farland::server
