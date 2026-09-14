// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/platform/input_translator.hpp>

#include <algorithm>
#include <array>
#include <type_traits>
#include <variant>

namespace farland::platform {

namespace {

namespace kbd = proto::kbd_flags;
namespace ptr = proto::ptr_flags;
namespace ptrx = proto::ptrx_flags;

// The second event of Pause (E1 1D, 45), which alone would be Num Lock.
constexpr std::uint16_t pause_numlock_scancode = 0x45;

constexpr std::array<std::uint32_t, 5> button_codes = {
    evdev::btn_left, evdev::btn_right, evdev::btn_middle, evdev::btn_side, evdev::btn_extra,
};
constexpr std::size_t button_x1 = 3;
constexpr std::size_t button_x2 = 4;

constexpr bool is_high_surrogate(std::uint16_t unit)
{
    return unit >= 0xD800 && unit <= 0xDBFF;
}

constexpr bool is_low_surrogate(std::uint16_t unit)
{
    return unit >= 0xDC00 && unit <= 0xDFFF;
}

/// The signed wheel rotation of a TS_POINTER_EVENT: WheelRotationMask is a
/// 9-bit two's complement value whose sign bit is PTRFLAGS_WHEEL_NEGATIVE.
constexpr std::int32_t wheel_rotation(std::uint16_t flags)
{
    const auto value = static_cast<std::int32_t>(flags & ptr::wheel_rotation_mask);
    return (flags & ptr::wheel_negative) != 0 ? value - 0x200 : value;
}

static_assert(wheel_rotation(ptr::wheel_negative | 0x88) == -120);
static_assert(wheel_rotation(0x78) == 120);

/// One axis of an absolute position, mapped from client to desktop pixels.
double map_axis(double value, std::uint32_t client_size, std::uint32_t desktop_size, std::int32_t origin)
{
    const double scaled = value * static_cast<double>(desktop_size) / static_cast<double>(client_size);
    return static_cast<double>(origin) + std::clamp(scaled, 0.0, static_cast<double>(desktop_size - 1));
}

}  // namespace

void InputTranslator::set_geometry(std::uint32_t client_width, std::uint32_t client_height, std::uint32_t desktop_width,
                                   std::uint32_t desktop_height, std::int32_t desktop_x,
                                   std::int32_t desktop_y) noexcept
{
    if (client_width == 0 || client_height == 0 || desktop_width == 0 || desktop_height == 0) {
        geometry_.reset();
        return;
    }
    geometry_ = Geometry{client_width, client_height, desktop_width, desktop_height, desktop_x, desktop_y};
}

void InputTranslator::translate(std::span<const proto::InputEvent> events)
{
    for (const auto& event : events) {
        std::visit(
            [this](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (!std::is_same_v<T, proto::QoeTimestampEvent>) {
                    handle(e);
                }
            },
            event);
    }
    finish();
}

void InputTranslator::translate(std::span<const channels::rdpei::Contact> contacts)
{
    for (const auto& contact : contacts) {
        if (contact.kind == channels::rdpei::ContactKind::touch) {
            handle_touch(contact);
        } else {
            handle_pen(contact);
        }
    }
    finish();
}

void InputTranslator::release_all()
{
    for (std::size_t id = 0; id < touches_.size(); ++id) {
        if (touches_.test(id)) {
            sink_->touch_cancel(static_cast<std::uint32_t>(id));
            emitted_ = true;
        }
    }
    touches_.reset();
    primary_touch_.reset();
    pen_button_.reset();
    release_everything();
    finish();
}

void InputTranslator::handle_touch(const channels::rdpei::Contact& contact)
{
    using Action = channels::rdpei::ContactAction;
    const std::uint8_t id = contact.id;
    if (contact.action == Action::down) {
        if (touches_.test(id) || primary_touch_ == id) {
            return;
        }
        if (sink_->accepts_touch()) {
            const auto [x, y] = map_point(contact.x, contact.y);
            sink_->touch_down(id, x, y);
            touches_.set(id);
            emitted_ = true;
        } else if (!primary_touch_) {
            primary_touch_ = id;
            move_pointer(contact.x, contact.y);
            set_button(0, true);
        }
        return;
    }
    // Hover and leave need nothing: touchscreens have no hover.
    const bool lifted = contact.action == Action::up || contact.action == Action::cancel;
    if (touches_.test(id)) {
        if (contact.action == Action::move) {
            const auto [x, y] = map_point(contact.x, contact.y);
            sink_->touch_motion(id, x, y);
            emitted_ = true;
        } else if (lifted) {
            if (contact.action == Action::cancel) {
                sink_->touch_cancel(id);
            } else {
                sink_->touch_up(id);
            }
            touches_.reset(id);
            emitted_ = true;
        }
    } else if (primary_touch_ == id) {
        if (contact.action == Action::move) {
            move_pointer(contact.x, contact.y);
        } else if (lifted) {
            set_button(0, false);
            primary_touch_.reset();
        }
    }
}

void InputTranslator::handle_pen(const channels::rdpei::Contact& contact)
{
    using Action = channels::rdpei::ContactAction;
    switch (contact.action) {
    case Action::hover:
    case Action::move:
        move_pointer(contact.x, contact.y);
        break;
    case Action::down:
        move_pointer(contact.x, contact.y);
        if (!pen_button_) {
            // The barrel button held as the pen comes down makes it a right click, as on Windows.
            const std::size_t index = (contact.pen_flags & channels::rdpei::pen_flags::barrel_pressed) != 0 ? 1 : 0;
            pen_button_ = index;
            set_button(index, true);
        }
        break;
    case Action::up:
    case Action::cancel:
        if (pen_button_) {
            set_button(*pen_button_, false);
            pen_button_.reset();
        }
        break;
    case Action::leave:
        break;
    }
}

void InputTranslator::handle(const proto::KeyboardEvent& event)
{
    const bool extended = (event.flags & kbd::extended) != 0;
    const bool extended1 = (event.flags & kbd::extended1) != 0;
    if (pause_pending_) {
        pause_pending_ = false;
        if (!extended && event.code == pause_numlock_scancode) {
            return;
        }
    }
    const auto code = scancode_to_evdev(event.code, extended, extended1);
    if (!code.has_value()) {
        return;
    }
    // Only E1 1D maps to a key, and a 45 follows it.
    pause_pending_ = extended1;
    set_key(*code, (event.flags & kbd::release) == 0);
}

void InputTranslator::handle(const proto::UnicodeKeyboardEvent& event)
{
    if ((event.flags & kbd::release) != 0) {
        return;
    }
    const std::uint16_t unit = event.code;
    if (is_high_surrogate(unit)) {
        high_surrogate_ = unit;  // replaces an unpaired one
        return;
    }
    if (is_low_surrogate(unit)) {
        if (high_surrogate_ == 0) {
            return;
        }
        const std::uint32_t high = high_surrogate_ - 0xD800U;
        const std::uint32_t low = unit - 0xDC00U;
        high_surrogate_ = 0;
        text(static_cast<char32_t>(0x10000U + (high << 10U) + low));
        return;
    }
    high_surrogate_ = 0;
    if (unit != 0) {
        text(static_cast<char32_t>(unit));
    }
}

void InputTranslator::handle(const proto::MouseEvent& event)
{
    // [MS-RDPBCGR] 2.2.8.1.1.3.1.1.3: a wheel event ignores the position and
    // every other flag, and the vertical wheel wins if both are set.
    if ((event.flags & ptr::wheel) != 0) {
        scroll(0, -wheel_rotation(event.flags));
        return;
    }
    if ((event.flags & ptr::hwheel) != 0) {
        scroll(wheel_rotation(event.flags), 0);
        return;
    }
    const bool move = (event.flags & ptr::move) != 0;
    constexpr std::uint16_t buttons = ptr::button1 | ptr::button2 | ptr::button3;
    if (move || (event.flags & buttons) != 0) {
        move_to(event.x, event.y, move);
    }
    const bool down = (event.flags & ptr::down) != 0;
    if ((event.flags & ptr::button1) != 0) {
        set_button(0, down);
    }
    if ((event.flags & ptr::button2) != 0) {
        set_button(1, down);
    }
    if ((event.flags & ptr::button3) != 0) {
        set_button(2, down);
    }
}

void InputTranslator::handle(const proto::ExtendedMouseEvent& event)
{
    const bool x1 = (event.flags & ptrx::button1) != 0;
    const bool x2 = (event.flags & ptrx::button2) != 0;
    if (!x1 && !x2) {
        return;
    }
    move_to(event.x, event.y, false);
    const bool down = (event.flags & ptrx::down) != 0;
    if (x1) {
        set_button(button_x1, down);
    }
    if (x2) {
        set_button(button_x2, down);
    }
}

void InputTranslator::handle(const proto::RelativeMouseEvent& event)
{
    // [MS-RDPBCGR] 2.2.8.1.1.3.1.1.7: buttons act at the position after the delta.
    const bool x1 = (event.flags & ptrx::button1) != 0;
    const bool x2 = (event.flags & ptrx::button2) != 0;
    const bool b1 = (event.flags & ptr::button1) != 0;
    const bool b2 = (event.flags & ptr::button2) != 0;
    const bool b3 = (event.flags & ptr::button3) != 0;
    const bool any_button = x1 || x2 || b1 || b2 || b3;
    if (((event.flags & ptr::move) != 0 || any_button) && (event.dx != 0 || event.dy != 0)) {
        sink_->pointer_motion_relative(event.dx, event.dy);
        emitted_ = true;
        position_.reset();
    }
    const bool down = (event.flags & ptr::down) != 0;
    if (b1) {
        set_button(0, down);
    }
    if (b2) {
        set_button(1, down);
    }
    if (b3) {
        set_button(2, down);
    }
    if (x1) {
        set_button(button_x1, down);
    }
    if (x2) {
        set_button(button_x2, down);
    }
}

void InputTranslator::handle(const proto::SyncEvent& event)
{
    release_everything();
    sync_ = LockState{
        .scroll_lock = (event.toggle_flags & proto::sync_flags::scroll_lock) != 0,
        .num_lock = (event.toggle_flags & proto::sync_flags::num_lock) != 0,
        .caps_lock = (event.toggle_flags & proto::sync_flags::caps_lock) != 0,
        .kana_lock = (event.toggle_flags & proto::sync_flags::kana_lock) != 0,
    };
}

void InputTranslator::set_key(std::uint32_t code, bool pressed)
{
    if (keys_down_.test(code) == pressed) {
        return;
    }
    keys_down_.set(code, pressed);
    sink_->key(code, pressed);
    emitted_ = true;
}

void InputTranslator::set_button(std::size_t index, bool pressed)
{
    if (buttons_down_.test(index) == pressed) {
        return;
    }
    buttons_down_.set(index, pressed);
    sink_->button(button_codes.at(index), pressed);
    emitted_ = true;
}

void InputTranslator::move_to(std::uint16_t x, std::uint16_t y, bool always)
{
    const std::pair position{x, y};
    if (!always && position_ == position) {
        return;
    }
    position_ = position;
    if (geometry_.has_value()) {
        const Geometry& g = *geometry_;
        sink_->pointer_motion_absolute(map_axis(x, g.client_width, g.desktop_width, g.desktop_x),
                                       map_axis(y, g.client_height, g.desktop_height, g.desktop_y));
    } else {
        sink_->pointer_motion_absolute(x, y);
    }
    emitted_ = true;
}

std::pair<double, double> InputTranslator::map_point(std::int32_t x, std::int32_t y) const
{
    if (!geometry_.has_value()) {
        return {static_cast<double>(x), static_cast<double>(y)};
    }
    const Geometry& g = *geometry_;
    return {map_axis(static_cast<double>(x), g.client_width, g.desktop_width, g.desktop_x),
            map_axis(static_cast<double>(y), g.client_height, g.desktop_height, g.desktop_y)};
}

void InputTranslator::move_pointer(std::int32_t x, std::int32_t y)
{
    const auto [px, py] = map_point(x, y);
    sink_->pointer_motion_absolute(px, py);
    position_.reset();  // the next mouse event moves the pointer back where it says
    emitted_ = true;
}

void InputTranslator::scroll(std::int32_t x_v120, std::int32_t y_v120)
{
    if (x_v120 == 0 && y_v120 == 0) {
        return;
    }
    sink_->scroll_discrete(x_v120, y_v120);
    emitted_ = true;
}

void InputTranslator::text(char32_t codepoint)
{
    sink_->text(codepoint);
    emitted_ = true;
}

void InputTranslator::release_everything()
{
    for (std::uint32_t code = 0; code < keys_down_.size(); ++code) {
        if (keys_down_.test(code)) {
            set_key(code, false);
        }
    }
    for (std::size_t index = 0; index < buttons_down_.size(); ++index) {
        if (buttons_down_.test(index)) {
            set_button(index, false);
        }
    }
    high_surrogate_ = 0;
    pause_pending_ = false;
}

void InputTranslator::finish()
{
    if (emitted_) {
        emitted_ = false;
        sink_->flush();
    }
}

}  // namespace farland::platform
