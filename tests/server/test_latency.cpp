// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Where a frame's time went (docs/ROADMAP.md M4). The clock is given to the
// tracker, so a whole run happens in one test with no waiting.

#include <farland/server/latency.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>

using farland::server::LatencyTracker;
using Clock = LatencyTracker::Clock;
using namespace std::chrono_literals;

namespace {

/// One frame through every stage, with the times given in milliseconds from
/// `start`.
void one_frame(LatencyTracker& tracker, std::uint32_t id, Clock::time_point start, int captured, int taken, int read,
               int sent, int acknowledged)
{
    tracker.frame_taken(start + std::chrono::milliseconds(taken),
                        captured < 0 ? std::nullopt : std::optional(start + std::chrono::milliseconds(captured)));
    tracker.frame_read(start + std::chrono::milliseconds(read));
    tracker.frame_encoded(start + std::chrono::milliseconds(sent));
    tracker.frame_sent(id, start + std::chrono::milliseconds(sent));
    tracker.frame_acknowledged(id, start + std::chrono::milliseconds(acknowledged));
}

[[nodiscard]] double ms(LatencyTracker::Duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

}  // namespace

TEST_CASE("Latency: an empty run reports nothing", "[server][latency]")
{
    const LatencyTracker tracker;
    const auto summary = tracker.summary();
    CHECK(summary.frames == 0);
    CHECK(LatencyTracker::describe(summary).starts_with("no frames were acknowledged"));
}

TEST_CASE("Latency: one frame is split into its stages", "[server][latency]")
{
    LatencyTracker tracker;
    const auto start = Clock::now();
    // Made at 0, taken at 10, read at 12, sent at 20, acknowledged at 25.
    one_frame(tracker, 1, start, 0, 10, 12, 20, 25);

    const auto summary = tracker.summary();
    REQUIRE(summary.frames == 1);
    CHECK(ms(summary.waiting_p50) == 10.0);
    CHECK(ms(summary.reading_p50) == 2.0);
    CHECK(ms(summary.encoding_p50) == 8.0);
    CHECK(ms(summary.client_p50) == 5.0);
    // The server's part is the first three; the total adds the client's.
    CHECK(ms(summary.server_p50) == 20.0);
    CHECK(ms(summary.total_p50) == 25.0);
    CHECK(summary.without_capture_time == 0);
}

TEST_CASE("Latency: percentiles come from the whole run", "[server][latency]")
{
    LatencyTracker tracker;
    const auto start = Clock::now();
    // Twenty frames, the last two slow: p50 sees the ordinary ones and p95
    // sees a slow one. That is the point of keeping every sample rather than
    // a smoothed average, which would hide both.
    for (int i = 0; i < 20; ++i) {
        const int base = i * 100;
        const int encode = (i >= 18) ? 40 : 8;
        one_frame(tracker, static_cast<std::uint32_t>(i + 1), start, base, base + 2, base + 4, base + 4 + encode,
                  base + 4 + encode + 5);
    }
    const auto summary = tracker.summary();
    REQUIRE(summary.frames == 20);
    CHECK(ms(summary.encoding_p50) == 8.0);
    CHECK(ms(summary.encoding_p95) == 40.0);
    CHECK(ms(summary.server_max) == 44.0);  // 2 waiting + 2 reading + 40 encoding
    // One frame in ten being slow does not move the middle.
    CHECK(ms(summary.server_p50) == 12.0);
}

TEST_CASE("Latency: a source without a clock still reports the rest", "[server][latency]")
{
    LatencyTracker tracker;
    const auto start = Clock::now();
    one_frame(tracker, 1, start, -1, 10, 12, 20, 25);

    const auto summary = tracker.summary();
    REQUIRE(summary.frames == 1);
    CHECK(summary.without_capture_time == 1);
    // No capture time means no wait to report, not a guess.
    CHECK(ms(summary.waiting_p50) == 0.0);
    CHECK(ms(summary.reading_p50) == 2.0);
    CHECK(ms(summary.server_p50) == 10.0);
    CHECK(LatencyTracker::describe(summary).find("counted as zero") != std::string::npos);
}

TEST_CASE("Latency: a capture time from the future is not one", "[server][latency]")
{
    LatencyTracker tracker;
    const auto start = Clock::now();
    // The compositor's clock is not ours: taken before it was made.
    one_frame(tracker, 1, start, 30, 10, 12, 20, 25);

    const auto summary = tracker.summary();
    REQUIRE(summary.frames == 1);
    CHECK(summary.without_capture_time == 1);
    CHECK(ms(summary.waiting_p50) == 0.0);
}

TEST_CASE("Latency: an acknowledgement nobody sent is ignored", "[server][latency]")
{
    LatencyTracker tracker;
    const auto start = Clock::now();
    tracker.frame_acknowledged(7, start);
    CHECK(tracker.summary().frames == 0);

    // And one for a frame that was sent measures only that frame.
    one_frame(tracker, 1, start, 0, 10, 12, 20, 25);
    tracker.frame_acknowledged(1, start + 500ms);  // again: already accounted for
    CHECK(tracker.summary().frames == 1);
}

TEST_CASE("Latency: acknowledging one frame drops the ones before it", "[server][latency]")
{
    LatencyTracker tracker;
    const auto start = Clock::now();
    // Three frames go out; the client acknowledges only the last. The two
    // before it are gone unmeasured rather than credited with its time.
    for (std::uint32_t id = 1; id <= 3; ++id) {
        const auto at = start + std::chrono::milliseconds(id * 10);
        tracker.frame_taken(at, at);
        tracker.frame_read(at);
        tracker.frame_encoded(at);
        tracker.frame_sent(id, at);
    }
    tracker.frame_acknowledged(3, start + 40ms);
    const auto summary = tracker.summary();
    CHECK(summary.frames == 1);
    CHECK(ms(summary.client_p50) == 10.0);
    // The first two are forgotten, so a late acknowledgement adds nothing.
    tracker.frame_acknowledged(1, start + 100ms);
    CHECK(tracker.summary().frames == 1);
}

TEST_CASE("Latency: a client that never answers does not grow the tracker", "[server][latency]")
{
    LatencyTracker tracker;
    const auto start = Clock::now();
    for (std::uint32_t id = 1; id <= 500; ++id) {
        const auto at = start + std::chrono::milliseconds(id);
        tracker.frame_taken(at, at);
        tracker.frame_read(at);
        tracker.frame_encoded(at);
        tracker.frame_sent(id, at);
    }
    CHECK(tracker.summary().frames == 0);
    // The oldest were forgotten, so an old id measures nothing; a recent one
    // still does.
    tracker.frame_acknowledged(1, start + 600ms);
    CHECK(tracker.summary().frames == 0);
    tracker.frame_acknowledged(500, start + 600ms);
    CHECK(tracker.summary().frames == 1);
}

TEST_CASE("Latency: the report names every stage", "[server][latency]")
{
    LatencyTracker tracker;
    const auto start = Clock::now();
    one_frame(tracker, 1, start, 0, 10, 12, 20, 25);
    const auto text = LatencyTracker::describe(tracker.summary());

    CHECK(text.find("waiting for the scheduler") != std::string::npos);
    CHECK(text.find("reading the pixels") != std::string::npos);
    CHECK(text.find("encoding and framing") != std::string::npos);
    CHECK(text.find("wire, decode and ack") != std::string::npos);
    CHECK(text.find("measured end to end") != std::string::npos);
    // The honest caveat is part of the report, not a footnote elsewhere.
    CHECK(text.find("the client's own compositor and screen are not in this") != std::string::npos);
}
