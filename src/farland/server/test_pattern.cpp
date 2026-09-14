// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/server/test_pattern.hpp>

#include <algorithm>
#include <array>
#include <format>

namespace farland::server {

namespace {

constexpr std::size_t bytes_per_pixel = 4;
constexpr std::uint32_t square_size = 96;
constexpr std::size_t key_cells = 16;

constexpr std::array<std::uint32_t, 8> bar_colors{
    0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000,
};

/// Position of a point bouncing between 0 and `range` with the given speed.
std::uint32_t bounce(std::uint64_t t, std::uint32_t range, std::uint32_t speed)
{
    if (range == 0) {
        return 0;
    }
    const std::uint64_t period = 2ULL * range;
    const auto pos = static_cast<std::uint32_t>((t * speed) % period);
    return pos < range ? pos : static_cast<std::uint32_t>(period) - pos;
}

}  // namespace

TestPattern::TestPattern(std::uint32_t width, std::uint32_t height)
{
    resize(width, height);
}

void TestPattern::resize(std::uint32_t width, std::uint32_t height)
{
    width_ = width;
    height_ = height;
    pixels_.assign(static_cast<std::size_t>(width) * height * bytes_per_pixel, std::byte{0});
    pointer_x_ = std::min(pointer_x_, width - 1);
    pointer_y_ = std::min(pointer_y_, height - 1);
}

void TestPattern::fill(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h, std::uint32_t rgb)
{
    const std::uint32_t x1 = std::min(x + w, width_);
    const std::uint32_t y1 = std::min(y + h, height_);
    const std::array pixel{static_cast<std::byte>(rgb & 0xFFU), static_cast<std::byte>((rgb >> 8U) & 0xFFU),
                           static_cast<std::byte>((rgb >> 16U) & 0xFFU), std::byte{0xFF}};
    for (std::uint32_t row = y; row < y1; ++row) {
        for (std::uint32_t col = x; col < x1; ++col) {
            const std::size_t offset = ((static_cast<std::size_t>(row) * width_) + col) * bytes_per_pixel;
            std::ranges::copy(pixel, std::span(pixels_).subspan(offset, bytes_per_pixel).begin());
        }
    }
}

codec::ImageView TestPattern::render(std::uint64_t frame)
{
    // Background: a vertical blue-to-dark gradient.
    for (std::uint32_t row = 0; row < height_; ++row) {
        const auto shade = static_cast<std::uint32_t>((row * 160U) / std::max(height_, 1U));
        fill(0, row, width_, 1, (shade / 4U) << 8U | (40U + shade));
    }
    // Color bars across the top quarter.
    const std::uint32_t bar_width = std::max(width_ / static_cast<std::uint32_t>(bar_colors.size()), 1U);
    for (std::size_t i = 0; i < bar_colors.size(); ++i) {
        fill(static_cast<std::uint32_t>(i) * bar_width, 0, bar_width, height_ / 4U, bar_colors.at(i));
    }
    // A bouncing square, so only a few tiles change per frame.
    const std::uint32_t sx = bounce(frame, width_ > square_size ? width_ - square_size : 0, 7);
    const std::uint32_t sy =
        (height_ / 4U) +
        bounce(frame, height_ > (height_ / 4U) + square_size ? height_ - (height_ / 4U) - square_size : 0, 5);
    fill(sx, sy, square_size, square_size, 0xFFA000);
    // The last keys pressed, as cells whose color encodes the scancode.
    for (std::size_t i = 0; i < recent_keys_.size(); ++i) {
        const std::uint32_t code = recent_keys_.at(i);
        fill(8 + (static_cast<std::uint32_t>(i) * 20U), height_ > 28 ? height_ - 28 : 0, 16, 20,
             ((code * 0x9E3779B1U) >> 8U) & 0xFFFFFFU);
    }
    // Touch and pen contacts: a square coloured by id, filled while it
    // touches, small while it hovers; pens are white.
    for (const auto& c : contacts_) {
        const std::uint32_t rgb =
            c.kind == channels::rdpei::ContactKind::pen ? 0xFFFFFFU : (((c.id + 1U) * 0x9E3779B1U) >> 8U) | 0x404040U;
        square(c.x, c.y, c.engaged ? 33 : 9, rgb & 0xFFFFFFU);
    }
    // Crosshair at the pointer, red while a button is held.
    const std::uint32_t color = button_down_ ? 0xFF2020 : 0xFFFFFF;
    fill(pointer_x_ > 12 ? pointer_x_ - 12 : 0, pointer_y_, 25, 1, color);
    fill(pointer_x_, pointer_y_ > 12 ? pointer_y_ - 12 : 0, 1, 25, color);
    return codec::ImageView{pixels_, width_, height_, static_cast<std::size_t>(width_) * bytes_per_pixel};
}

void TestPattern::apply(const proto::InputEvent& event)
{
    if (const auto* mouse = std::get_if<proto::MouseEvent>(&event)) {
        pointer_x_ = std::min<std::uint32_t>(mouse->x, width_ - 1);
        pointer_y_ = std::min<std::uint32_t>(mouse->y, height_ - 1);
        constexpr std::uint16_t buttons =
            proto::ptr_flags::button1 | proto::ptr_flags::button2 | proto::ptr_flags::button3;
        if ((mouse->flags & buttons) != 0 && (mouse->flags & proto::ptr_flags::wheel) == 0) {
            button_down_ = (mouse->flags & proto::ptr_flags::down) != 0;
        }
    } else if (const auto* key = std::get_if<proto::KeyboardEvent>(&event)) {
        if ((key->flags & proto::kbd_flags::release) == 0) {
            recent_keys_.push_back(key->code);
            if (recent_keys_.size() > key_cells) {
                recent_keys_.erase(recent_keys_.begin());
            }
        }
    }
}

void TestPattern::square(std::uint32_t x, std::uint32_t y, std::uint32_t size, std::uint32_t rgb)
{
    const std::uint32_t half = size / 2;
    fill(x > half ? x - half : 0, y > half ? y - half : 0, size, size, rgb);
}

void TestPattern::apply(const channels::rdpei::Contact& contact)
{
    using Action = channels::rdpei::ContactAction;
    const auto it =
        std::ranges::find_if(contacts_, [&](const Point& p) { return p.kind == contact.kind && p.id == contact.id; });
    if (contact.action == Action::up || contact.action == Action::cancel || contact.action == Action::leave) {
        if (it != contacts_.end()) {
            contacts_.erase(it);
        }
        return;
    }
    if (width_ == 0 || height_ == 0) {
        return;
    }
    const Point point{
        .kind = contact.kind,
        .id = contact.id,
        .x = static_cast<std::uint32_t>(std::clamp<std::int64_t>(contact.x, 0, std::int64_t{width_} - 1)),
        .y = static_cast<std::uint32_t>(std::clamp<std::int64_t>(contact.y, 0, std::int64_t{height_} - 1)),
        .engaged = contact.action != Action::hover,
    };
    if (it != contacts_.end()) {
        *it = point;
    } else {
        contacts_.push_back(point);
    }
}

std::string TestPattern::describe(const channels::rdpei::Contact& contact)
{
    using Action = channels::rdpei::ContactAction;
    const char* action = "move";
    switch (contact.action) {
    case Action::down:
        action = "down";
        break;
    case Action::move:
        break;
    case Action::up:
        action = "up";
        break;
    case Action::cancel:
        action = "cancel";
        break;
    case Action::hover:
        action = "hover";
        break;
    case Action::leave:
        action = "leave";
        break;
    }
    return std::format("{} {} {} {},{}{}", contact.kind == channels::rdpei::ContactKind::pen ? "pen" : "touch",
                       contact.id, action, contact.x, contact.y,
                       contact.pressure ? std::format(" pressure {}", *contact.pressure) : std::string());
}

std::string TestPattern::describe(const proto::InputEvent& event)
{
    return std::visit(
        [](const auto& e) -> std::string {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, proto::KeyboardEvent>) {
                return std::format("key 0x{:02x}{}{}", e.code,
                                   (e.flags & proto::kbd_flags::extended) != 0 ? " ext" : "",
                                   (e.flags & proto::kbd_flags::release) != 0 ? " up" : " down");
            } else if constexpr (std::is_same_v<T, proto::UnicodeKeyboardEvent>) {
                return std::format("unicode U+{:04X}{}", e.code,
                                   (e.flags & proto::kbd_flags::release) != 0 ? " up" : " down");
            } else if constexpr (std::is_same_v<T, proto::MouseEvent>) {
                return std::format("mouse {},{} flags 0x{:04x}", e.x, e.y, e.flags);
            } else if constexpr (std::is_same_v<T, proto::ExtendedMouseEvent>) {
                return std::format("xmouse {},{} flags 0x{:04x}", e.x, e.y, e.flags);
            } else if constexpr (std::is_same_v<T, proto::RelativeMouseEvent>) {
                return std::format("relative mouse {:+},{:+}", e.dx, e.dy);
            } else if constexpr (std::is_same_v<T, proto::SyncEvent>) {
                return std::format("sync toggles 0x{:x}", e.toggle_flags);
            } else {
                return std::format("qoe timestamp {}", e.timestamp);
            }
        },
        event);
}

}  // namespace farland::server
