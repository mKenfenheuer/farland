// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// What a Prometheus scrape of farlandd says, and the address it is served on.

#include "metrics.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <thread>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using farland::daemon::Metrics;
using farland::daemon::MetricsSnapshot;
using farland::daemon::render_metrics;
using farland::daemon::SessionMetrics;
using farland::daemon::split_listen_address;

namespace {

SessionMetrics session(std::uint32_t id, std::string account, std::string state)
{
    SessionMetrics s;
    s.id = id;
    s.account = std::move(account);
    s.state = std::move(state);
    return s;
}

/// Whether a line stands on its own in the exposition, rather than as the
/// prefix of a longer one: `farland_sessions 1` is not
/// `farland_sessions_connected 1`.
[[nodiscard]] bool has_line(const std::string& text, const std::string& line)
{
    const auto needle = line + "\n";
    const auto at = text.find(needle);
    return at != std::string::npos && (at == 0 || text[at - 1] == '\n');
}

}  // namespace

TEST_CASE("Metrics: an idle daemon still says so", "[daemon][metrics]")
{
    const auto text = render_metrics(MetricsSnapshot{});

    // Every metric is declared even with nothing to report, so a dashboard
    // does not have to wait for the first session to learn the names.
    CHECK_THAT(text, ContainsSubstring("# TYPE farland_sessions gauge"));
    CHECK_THAT(text, ContainsSubstring("# TYPE farland_connections_total counter"));
    CHECK(has_line(text, R"(farland_sessions{state="running"} 0)"));
    CHECK(has_line(text, "farland_sessions_connected 0"));
    CHECK(has_line(text, "farland_connections_total 0"));
    CHECK(has_line(text, "farland_sessions_started_total 0"));
}

TEST_CASE("Metrics: the sessions and what they are doing", "[daemon][metrics]")
{
    MetricsSnapshot snapshot;
    auto alice = session(7, "alice", "running");
    alice.connected = true;
    alice.uptime_seconds = 3600;
    alice.idle_seconds = 12;
    alice.frames_sent = 4500;
    alice.bytes_sent = 900000;
    alice.bytes_received = 3000;
    alice.rtt_ms = 25;
    alice.bandwidth_kbps = 48000;
    snapshot.sessions.push_back(alice);
    snapshot.sessions.push_back(session(8, "bob", "starting"));

    const auto text = render_metrics(snapshot);

    CHECK(has_line(text, R"(farland_sessions{state="running"} 1)"));
    CHECK(has_line(text, R"(farland_sessions{state="starting"} 1)"));
    CHECK(has_line(text, R"(farland_sessions{state="ending"} 0)"));
    CHECK(has_line(text, "farland_sessions_connected 1"));
    CHECK(has_line(text, R"(farland_session_uptime_seconds{session="7",account="alice"} 3600)"));
    CHECK(has_line(text, R"(farland_session_idle_seconds{session="7",account="alice"} 12)"));
    CHECK(has_line(text, R"(farland_session_frames_sent_total{session="7",account="alice"} 4500)"));
    CHECK(has_line(text, R"(farland_session_bytes_sent_total{session="7",account="alice"} 900000)"));
    CHECK(has_line(text, R"(farland_session_bytes_received_total{session="7",account="alice"} 3000)"));
    // Prometheus counts in base units: milliseconds become seconds, and the
    // agent's kbit/s becomes bit/s.
    CHECK(has_line(text, R"(farland_session_round_trip_seconds{session="7",account="alice"} 0.025)"));
    CHECK(has_line(text, R"(farland_session_bandwidth_bits_per_second{session="7",account="alice"} 48000000)"));
    // A session with nothing measured is still listed, at zero.
    CHECK(has_line(text, R"(farland_session_round_trip_seconds{session="8",account="bob"} 0.000)"));
}

TEST_CASE("Metrics: an account name cannot break out of its label", "[daemon][metrics]")
{
    MetricsSnapshot snapshot;
    snapshot.sessions.push_back(session(1, R"(od"d\one)", "running"));
    snapshot.sessions_ended_total[R"(a "reason")"] = 2;

    const auto text = render_metrics(snapshot);

    CHECK_THAT(text, ContainsSubstring(R"(account="od\"d\\one")"));
    CHECK_THAT(text, ContainsSubstring(R"(farland_sessions_ended_total{reason="a \"reason\""} 2)"));
}

TEST_CASE("Metrics: the counters the loop keeps", "[daemon][metrics]")
{
    Metrics metrics;
    metrics.connection();
    metrics.connection();
    metrics.session_started();
    metrics.session_ended("disconnected_timeout");
    metrics.session_ended("disconnected_timeout");
    metrics.session_ended("requested");
    metrics.connection_refused("denied");

    const auto snapshot = metrics.snapshot();
    CHECK(snapshot.connections_total == 2);
    CHECK(snapshot.sessions_started_total == 1);
    CHECK(snapshot.sessions_ended_total.at("disconnected_timeout") == 2);
    CHECK(snapshot.sessions_ended_total.at("requested") == 1);
    CHECK(snapshot.connections_refused_total.at("denied") == 1);

    // Publishing replaces the sessions and leaves the counters alone.
    metrics.publish({session(3, "carol", "running")});
    const auto after = metrics.snapshot();
    CHECK(after.sessions.size() == 1);
    CHECK(after.connections_total == 2);
}

TEST_CASE("Metrics: a scrape and the loop can run at once", "[daemon][metrics]")
{
    Metrics metrics;
    std::jthread loop([&metrics] {
        for (int i = 0; i < 2000; ++i) {
            metrics.connection();
            metrics.publish({session(1, "alice", "running")});
        }
    });
    for (int i = 0; i < 2000; ++i) {
        const auto text = render_metrics(metrics.snapshot());
        CHECK_THAT(text, ContainsSubstring("farland_connections_total"));
    }
}

TEST_CASE("Metrics: the address to listen on", "[daemon][metrics]")
{
    const auto loopback = split_listen_address("127.0.0.1:9128");
    REQUIRE(loopback.has_value());
    CHECK(loopback->first == "127.0.0.1");
    CHECK(loopback->second == 9128);

    // An empty host is every interface, which is what ":9128" asks for.
    const auto any = split_listen_address(":9128");
    REQUIRE(any.has_value());
    CHECK(any->first.empty());
    CHECK(any->second == 9128);

    // IPv6 has colons of its own, so the address is bracketed.
    const auto v6 = split_listen_address("[::1]:9128");
    REQUIRE(v6.has_value());
    CHECK(v6->first == "::1");
    CHECK(v6->second == 9128);

    CHECK_FALSE(split_listen_address("127.0.0.1").has_value());
    CHECK_FALSE(split_listen_address("::1:9128").has_value());
    CHECK_FALSE(split_listen_address("[::1]9128").has_value());
    CHECK_FALSE(split_listen_address("127.0.0.1:0").has_value());
    CHECK_FALSE(split_listen_address("127.0.0.1:65536").has_value());
    CHECK_FALSE(split_listen_address("127.0.0.1:http").has_value());
    CHECK_FALSE(split_listen_address("127.0.0.1:91 28").has_value());
}
