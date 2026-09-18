// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "test_desktop.hpp"

#include <farland/proto/input.hpp>

#include <algorithm>
#include <cmath>

namespace farland::app {

namespace {

constexpr std::uint32_t btn_left = 0x110;  ///< BTN_LEFT, linux/input-event-codes.h
constexpr std::uint32_t btn_right = 0x111;
constexpr std::uint32_t btn_middle = 0x112;

std::uint16_t coordinate(double value, std::uint32_t size)
{
    const double limit = std::max<double>(static_cast<double>(size) - 1, 0);
    return static_cast<std::uint16_t>(std::lround(std::clamp(value, 0.0, std::min(limit, 65535.0))));
}

}  // namespace

TestDesktop::TestDesktop(std::uint32_t width, std::uint32_t height, unsigned frames_per_second)
    : pattern_(std::max(width, 1U), std::max(height, 1U)),
      interval_(std::chrono::microseconds(1'000'000 / std::max(frames_per_second, 1U)))
{
}

std::uint64_t TestDesktop::frame_number() const
{
    return static_cast<std::uint64_t>((Clock::now() - started_) / interval_);
}

void TestDesktop::request_screen_sizes(std::span<const std::pair<std::uint32_t, std::uint32_t>> sizes)
{
    if (sizes.empty()) {
        return;
    }
    const auto [width, height] = sizes.front();
    if (width == 0 || height == 0 || (width == pattern_.width() && height == pattern_.height())) {
        return;
    }
    pattern_.resize(width, height);
    changed_ = true;
}

std::optional<platform::Frame> TestDesktop::Frames::take_frame()
{
    auto& d = desktop_;
    const std::uint64_t number = d.frame_number();
    if (d.shown_ == number && !d.changed_) {
        return std::nullopt;
    }
    d.shown_ = number;
    d.changed_ = false;
    platform::Frame frame;
    frame.image = d.pattern_.render(number);
    frame.sequence = ++d.sequence_;
    return frame;
}

std::pair<std::uint32_t, std::uint32_t> TestDesktop::Frames::size() const
{
    return {desktop_.pattern_.width(), desktop_.pattern_.height()};
}

void TestDesktop::Input::key(std::uint32_t evdev_code, bool pressed)
{
    // The pattern shows a cell per key, coloured by its code; here that is
    // the evdev code the translator made of the client's scancode.
    const auto flags = pressed ? std::uint16_t{0} : proto::kbd_flags::release;
    desktop_.pattern_.apply(proto::InputEvent{proto::KeyboardEvent{flags, static_cast<std::uint16_t>(evdev_code)}});
    desktop_.changed_ = true;
}

void TestDesktop::Input::pointer_motion_absolute(double x, double y)
{
    desktop_.pointer_x_ = x;
    desktop_.pointer_y_ = y;
    desktop_.pointer_event(proto::ptr_flags::move);
}

void TestDesktop::Input::pointer_motion_relative(double dx, double dy)
{
    desktop_.pointer_x_ += dx;
    desktop_.pointer_y_ += dy;
    desktop_.pointer_event(proto::ptr_flags::move);
}

void TestDesktop::Input::button(std::uint32_t evdev_button, bool pressed)
{
    std::uint16_t flags = 0;
    if (evdev_button == btn_left) {
        flags = proto::ptr_flags::button1;
    } else if (evdev_button == btn_right) {
        flags = proto::ptr_flags::button2;
    } else if (evdev_button == btn_middle) {
        flags = proto::ptr_flags::button3;
    } else {
        return;
    }
    desktop_.button_down_ = pressed;
    desktop_.pointer_event(static_cast<std::uint16_t>(flags | (pressed ? proto::ptr_flags::down : 0U)));
}

void TestDesktop::pointer_event(std::uint16_t flags)
{
    const proto::MouseEvent event{flags, coordinate(pointer_x_, pattern_.width()),
                                  coordinate(pointer_y_, pattern_.height())};
    pattern_.apply(proto::InputEvent{event});
    changed_ = true;
}

}  // namespace farland::app
