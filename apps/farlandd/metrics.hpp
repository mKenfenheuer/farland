// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

/// What farlandd is doing, in the Prometheus text format (docs/ROADMAP.md
/// M7, S5): how many sessions there are and what they are doing, and per
/// session the numbers the agents already report — frames, bytes, round trip
/// and the bandwidth auto-detect measured.
///
/// The figures come from the daemon's loop, and a scrape arrives on a thread
/// of its own, so this keeps the two apart the way SessionView does: the loop
/// publishes a snapshot, the scrape renders the last one.
///
/// Serving it is off unless `[metrics] listen` names an address, and the
/// address the example configuration suggests is on the loopback. It is a
/// listening socket in a root daemon, so the server answers exactly one
/// thing: `GET /metrics`. It reads a bounded request with a short timeout,
/// writes the answer, and closes; no keep-alive, no other path, no request
/// body.
namespace farland::daemon {

/// One session's numbers, as its agent last reported them.
struct SessionMetrics {
    std::uint32_t id = 0;
    std::string account;
    /// "starting", "running" or "ending".
    std::string state;
    bool connected = false;
    std::uint64_t uptime_seconds = 0;
    std::uint32_t idle_seconds = 0;
    std::uint64_t frames_sent = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    std::uint32_t rtt_ms = 0;
    std::uint32_t bandwidth_kbps = 0;

    friend bool operator==(const SessionMetrics&, const SessionMetrics&) = default;
};

/// Everything a scrape reports.
struct MetricsSnapshot {
    std::vector<SessionMetrics> sessions;
    std::uint64_t connections_total = 0;
    std::uint64_t sessions_started_total = 0;
    /// Ended sessions by the reason they ended, as broker::EndReason names it.
    std::map<std::string, std::uint64_t> sessions_ended_total;
    /// Refused connections by why, as the admission decision named it.
    std::map<std::string, std::uint64_t> connections_refused_total;

    friend bool operator==(const MetricsSnapshot&, const MetricsSnapshot&) = default;
};

/// The snapshot in the Prometheus text exposition format, version 0.0.4.
[[nodiscard]] std::string render_metrics(const MetricsSnapshot& snapshot);

/// The counters the daemon keeps, and the last snapshot a scrape gets. Safe
/// from either thread.
class Metrics {
public:
    /// Daemon loop: the sessions as they are now.
    void publish(std::vector<SessionMetrics> sessions);
    /// Daemon loop: one more connection admitted.
    void connection();
    /// Daemon loop: one more session started.
    void session_started();
    /// Daemon loop: a session ended for `reason`.
    void session_ended(std::string reason);
    /// Daemon loop: a connection refused for `reason`.
    void connection_refused(std::string reason);

    /// Scrape thread: what to answer with.
    [[nodiscard]] MetricsSnapshot snapshot() const;

private:
    mutable std::mutex mutex_;
    MetricsSnapshot state_;
};

/// Serves `GET /metrics` on `address` ("host:port"), on a thread of its own.
class MetricsServer {
public:
    /// `metrics` must outlive the server.
    explicit MetricsServer(const Metrics& metrics);
    MetricsServer(const MetricsServer&) = delete;
    MetricsServer& operator=(const MetricsServer&) = delete;
    MetricsServer(MetricsServer&&) = delete;
    MetricsServer& operator=(MetricsServer&&) = delete;
    ~MetricsServer();

    /// Binds and starts serving. `address` is "host:port"; the host may be
    /// an IPv4 address, an IPv6 address in brackets, or empty for every
    /// interface.
    [[nodiscard]] Result<void> start(const std::string& address);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

/// Splits "host:port", "[v6]:port" or ":port". Fails on a port outside
/// 1..65535 or a shape it cannot read.
[[nodiscard]] Result<std::pair<std::string, std::uint16_t>> split_listen_address(const std::string& address);

}  // namespace farland::daemon
