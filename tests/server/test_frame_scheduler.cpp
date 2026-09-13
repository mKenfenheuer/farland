// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/server/frame_scheduler.hpp>

#include <catch2/catch_test_macros.hpp>

using farland::server::FrameScheduler;
using namespace std::chrono_literals;

namespace {

const FrameScheduler::Clock::time_point t0{};

FrameScheduler::Config config(std::size_t window = 2, unsigned fps = 50)
{
    FrameScheduler::Config c;
    c.max_fps = fps;
    c.max_frames_in_flight = window;
    return c;
}

}  // namespace

TEST_CASE("Frames are only due after damage")
{
    FrameScheduler s(config());
    CHECK_FALSE(s.due(t0));
    CHECK_FALSE(s.wait(t0).has_value());
    s.damage();
    CHECK(s.due(t0));
    CHECK(s.wait(t0) == 0ms);
    s.frame_sent(1, t0);
    CHECK_FALSE(s.pending());
    CHECK_FALSE(s.due(t0 + 100ms));
}

TEST_CASE("The frame rate is capped")
{
    FrameScheduler s(config(8, 50));  // 20 ms per frame
    s.damage();
    s.frame_sent(1, t0);
    s.damage();
    CHECK_FALSE(s.due(t0 + 10ms));
    CHECK(s.wait(t0 + 10ms) == 10ms);
    CHECK(s.due(t0 + 20ms));
}

TEST_CASE("A full window waits for acknowledgements and keeps the damage")
{
    FrameScheduler s(config(2));
    s.damage();
    s.frame_sent(1, t0);
    s.damage();
    s.frame_sent(2, t0 + 20ms);
    s.damage();
    CHECK(s.frames_in_flight() == 2);
    CHECK_FALSE(s.due(t0 + 100ms));
    CHECK(s.wait(t0 + 100ms) == 900ms);  // until frame 1's acknowledgement is overdue
    CHECK(s.pending());

    s.frame_acknowledged(1, t0 + 110ms);
    CHECK(s.frames_in_flight() == 1);
    CHECK(s.round_trip() == 110ms);
    CHECK(s.due(t0 + 110ms));
}

TEST_CASE("Acknowledging a frame also retires older ones; unknown IDs are ignored")
{
    FrameScheduler s(config(4));
    for (std::uint32_t id = 1; id <= 3; ++id) {
        s.damage();
        s.frame_sent(id, t0 + (id * 20ms));
    }
    s.frame_acknowledged(99, t0 + 80ms);
    CHECK(s.frames_in_flight() == 3);
    s.frame_acknowledged(2, t0 + 100ms);
    CHECK(s.frames_in_flight() == 1);
    CHECK(s.round_trip() == 60ms);
    s.frame_acknowledged(3, t0 + 140ms);
    const FrameScheduler::Clock::duration previous = 60ms;
    CHECK(s.round_trip() == (previous * 7 + 80ms) / 8);  // 62.5 ms, in the clock's resolution
}

TEST_CASE("Lost acknowledgements expire, and suspension stops the gating")
{
    FrameScheduler s(config(1));
    s.damage();
    s.frame_sent(1, t0);
    s.damage();
    CHECK_FALSE(s.due(t0 + 500ms));
    CHECK(s.due(t0 + 1s));  // frame 1 timed out
    s.frame_sent(2, t0 + 1s);

    s.set_acknowledgements_suspended(true);
    CHECK(s.frames_in_flight() == 0);
    for (std::uint32_t id = 3; id < 10; ++id) {
        s.damage();
        const auto now = t0 + 1s + (id * 20ms);
        REQUIRE(s.due(now));
        s.frame_sent(id, now);
    }
    CHECK(s.frames_in_flight() == 0);
}

TEST_CASE("Without acknowledgements only the frame rate limits")
{
    auto c = config(1);
    c.acknowledgements = false;
    FrameScheduler s(c);
    for (std::uint32_t id = 0; id < 5; ++id) {
        s.damage();
        const auto now = t0 + (id * 20ms);
        REQUIRE(s.due(now));
        s.frame_sent(id, now);
    }
}
