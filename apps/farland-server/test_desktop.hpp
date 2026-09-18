// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/server/test_pattern.hpp>

#include "desktop.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

namespace farland::app {

/// The synthetic test pattern (docs/PLAN.md §3.3, the test backend) as a
/// desktop that outlives connections, for farland-agent with
/// `[session] desktop = "test"` (CI and first tests of farlandd): its frame
/// clock keeps running while no client watches, and what input drew (the
/// last keys, the pointer's crosshair) stays until the next connection. Its
/// one screen is virtual and takes the client's size.
///
/// Used on one thread at a time, like every Desktop.
class TestDesktop final : public Desktop {
public:
    using Clock = std::chrono::steady_clock;

    TestDesktop(std::uint32_t width, std::uint32_t height, unsigned frames_per_second = 30);

    [[nodiscard]] platform::FrameSource& frames() override { return frames_; }
    [[nodiscard]] platform::CursorSource* cursor() override { return nullptr; }
    [[nodiscard]] platform::InputSink& input() override { return input_; }
    [[nodiscard]] std::vector<int> dispatch_fds() const override { return {}; }
    void dispatch() override {}
    [[nodiscard]] bool closed() const override { return closed_.load(); }
    [[nodiscard]] bool resizable() const override { return true; }
    void request_screen_sizes(std::span<const std::pair<std::uint32_t, std::uint32_t>> sizes) override;

    /// The frame number the pattern shows now: frames since the desktop started.
    [[nodiscard]] std::uint64_t frame_number() const;
    /// Ends the desktop, as a logout ends a compositor. Any thread.
    void close() noexcept { closed_ = true; }

private:
    class Frames final : public platform::FrameSource {
    public:
        explicit Frames(TestDesktop& desktop) : desktop_(desktop) {}
        [[nodiscard]] int wake_fd() const noexcept override { return -1; }
        [[nodiscard]] std::optional<platform::Frame> take_frame() override;
        [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> size() const override;

    private:
        TestDesktop& desktop_;
    };

    /// Input in evdev codes (the translator's output) drawn on the pattern:
    /// keys as cells, the pointer as the crosshair.
    class Input final : public platform::InputSink {
    public:
        explicit Input(TestDesktop& desktop) : desktop_(desktop) {}
        void key(std::uint32_t evdev_code, bool pressed) override;
        void pointer_motion_absolute(double x, double y) override;
        void pointer_motion_relative(double dx, double dy) override;
        void button(std::uint32_t evdev_button, bool pressed) override;
        void scroll_discrete(std::int32_t /*x_v120*/, std::int32_t /*y_v120*/) override {}
        void text(char32_t /*codepoint*/) override {}
        void flush() override {}

    private:
        TestDesktop& desktop_;
    };

    void pointer_event(std::uint16_t flags);

    server::TestPattern pattern_;
    Clock::time_point started_ = Clock::now();
    Clock::duration interval_;
    std::optional<std::uint64_t> shown_;  ///< frame number of the last frame taken
    bool changed_ = true;                 ///< input or a resize since then
    std::uint64_t sequence_ = 0;
    double pointer_x_ = 0;
    double pointer_y_ = 0;
    bool button_down_ = false;
    std::atomic<bool> closed_{false};
    Frames frames_{*this};
    Input input_{*this};
};

}  // namespace farland::app
