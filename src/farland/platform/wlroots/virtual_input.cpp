// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/base/unique_fd.hpp>
#include <farland/platform/wlroots/virtual_input.hpp>
#include <farland/platform/wlroots/wayland/connection.hpp>
#include <farland/platform/wlroots/xkb_keymap.hpp>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <limits>
#include <sys/mman.h>
#include <unistd.h>
#include <virtual-keyboard-unstable-v1-client-protocol.h>
#include <wayland-client.h>
#include <wlr-virtual-pointer-unstable-v1-client-protocol.h>

namespace farland::platform::wlroots {

namespace {

constexpr std::string_view log_component = "platform.wlroots.input";
/// A wheel notch is 120 v120 units, and 15 logical pixels of scrolling, as
/// libinput reports a mouse wheel.
constexpr std::int32_t notch = 120;
constexpr double pixels_per_notch = 15.0;

/// Milliseconds on the monotonic clock, as input event times go.
std::uint32_t now_ms()
{
    timespec ts{};
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(ts.tv_sec) * 1000) +
                                      (static_cast<std::uint64_t>(ts.tv_nsec) / 1'000'000));
}

double squared_distance(const Rect& r, double x, double y)
{
    const double cx = std::clamp(x, static_cast<double>(r.x), static_cast<double>(r.x) + r.width - 1);
    const double cy = std::clamp(y, static_cast<double>(r.y), static_cast<double>(r.y) + r.height - 1);
    return ((cx - x) * (cx - x)) + ((cy - y) * (cy - y));
}

}  // namespace

std::optional<std::pair<double, double>> map_to_layout(std::span<const ScreenMapping> screens, double x, double y)
{
    const ScreenMapping* best = nullptr;
    double best_distance = std::numeric_limits<double>::infinity();
    for (const auto& screen : screens) {
        if (screen.target.width <= 0 || screen.target.height <= 0) {
            continue;
        }
        const double distance = squared_distance(screen.target, x, y);
        if (distance < best_distance) {
            best_distance = distance;
            best = &screen;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    const auto& t = best->target;
    const auto& o = best->output;
    const double fx = std::clamp((x - t.x) / t.width, 0.0, 1.0);
    const double fy = std::clamp((y - t.y) / t.height, 0.0, 1.0);
    // Stay on the last pixel rather than just beyond the output.
    const double lx = std::min(o.x + (fx * o.width), static_cast<double>(o.x) + std::max(o.width - 1, 0));
    const double ly = std::min(o.y + (fy * o.height), static_cast<double>(o.y) + std::max(o.height - 1, 0));
    return std::pair{lx, ly};
}

Result<std::unique_ptr<VirtualInput>> VirtualInput::create(wayland::Connection& connection, wl_seat* seat,
                                                           std::string_view keymap_layout)
{
    const auto* keyboards = connection.find("zwp_virtual_keyboard_manager_v1");
    const auto* pointers = connection.find("zwlr_virtual_pointer_manager_v1");
    if (keyboards == nullptr || pointers == nullptr) {
        log::error(log_component, "the compositor lacks {}",
                   keyboards == nullptr ? "zwp_virtual_keyboard_manager_v1" : "zwlr_virtual_pointer_manager_v1");
        return fail(Errc::unsupported, "the compositor has no virtual keyboard or pointer");
    }
    FARLAND_TRY(auto keymap, XkbKeymap::create(keymap_layout));
    auto* keyboard_manager =
        connection.bind<zwp_virtual_keyboard_manager_v1>(*keyboards, &zwp_virtual_keyboard_manager_v1_interface, 1);
    auto* pointer_manager =
        connection.bind<zwlr_virtual_pointer_manager_v1>(*pointers, &zwlr_virtual_pointer_manager_v1_interface, 2);
    auto* keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboard_manager, seat);
    auto* pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pointer_manager, seat);
    // The devices stay when their managers go.
    zwp_virtual_keyboard_manager_v1_destroy(keyboard_manager);
    zwlr_virtual_pointer_manager_v1_destroy(pointer_manager);
    std::unique_ptr<VirtualInput> input(new VirtualInput(connection, keyboard, pointer, std::move(keymap)));
    input->send_keymap();
    connection.flush();
    return input;
}

VirtualInput::VirtualInput(wayland::Connection& connection, zwp_virtual_keyboard_v1* keyboard,
                           zwlr_virtual_pointer_v1* pointer, std::unique_ptr<XkbKeymap> keymap)
    : connection_(&connection), keyboard_(keyboard), pointer_(pointer), keymap_(std::move(keymap))
{
}

VirtualInput::~VirtualInput()
{
    zwp_virtual_keyboard_v1_destroy(keyboard_);
    zwlr_virtual_pointer_v1_destroy(pointer_);
    connection_->flush();
}

void VirtualInput::set_layout(std::vector<ScreenMapping> screens, Rect layout_box)
{
    screens_ = std::move(screens);
    layout_box_ = layout_box;
}

void VirtualInput::send_keymap()
{
    const auto& text = keymap_->text();
    const UniqueFd fd(::memfd_create("farland-keymap", MFD_CLOEXEC));
    // The compositor maps size bytes, which include the terminating zero.
    const std::size_t size = text.size() + 1;
    bool ok = fd.valid() && ::ftruncate(fd.get(), static_cast<off_t>(size)) == 0;
    std::size_t offset = 0;
    while (ok && offset < text.size()) {
        const auto rest = std::string_view(text).substr(offset);
        const auto written = ::write(fd.get(), rest.data(), rest.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        ok = written > 0;
        offset += ok ? static_cast<std::size_t>(written) : 0;
    }
    if (!ok) {
        log::error(log_component, "cannot hand the keymap over: {}", std::strerror(errno));
        return;
    }
    zwp_virtual_keyboard_v1_keymap(keyboard_, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd.get(),
                                   static_cast<std::uint32_t>(size));
    keymap_generation_ = keymap_->generation();
    send_modifiers();
}

void VirtualInput::send_modifiers()
{
    const auto mods = keymap_->modifiers();
    zwp_virtual_keyboard_v1_modifiers(keyboard_, mods.depressed, mods.latched, mods.locked, mods.group);
}

void VirtualInput::key(std::uint32_t evdev_code, bool pressed)
{
    zwp_virtual_keyboard_v1_key(keyboard_, now_ms(), evdev_code,
                                pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
    if (keymap_->update_key(evdev_code, pressed)) {
        send_modifiers();
    }
}

void VirtualInput::pointer_motion_absolute(double x, double y)
{
    double lx = x;
    double ly = y;
    if (const auto mapped = map_to_layout(screens_, x, y)) {
        std::tie(lx, ly) = *mapped;
    }
    if (!layout_box_ || layout_box_->width <= 0 || layout_box_->height <= 0) {
        return;
    }
    const auto& box = *layout_box_;
    // motion_absolute spans the whole layout: x / x_extent of its width.
    const auto px =
        static_cast<std::uint32_t>(std::clamp(std::lround(lx - box.x), 0L, static_cast<long>(box.width) - 1));
    const auto py =
        static_cast<std::uint32_t>(std::clamp(std::lround(ly - box.y), 0L, static_cast<long>(box.height) - 1));
    zwlr_virtual_pointer_v1_motion_absolute(pointer_, now_ms(), px, py, static_cast<std::uint32_t>(box.width),
                                            static_cast<std::uint32_t>(box.height));
    pointer_frame_ = true;
}

void VirtualInput::pointer_motion_relative(double dx, double dy)
{
    zwlr_virtual_pointer_v1_motion(pointer_, now_ms(), wl_fixed_from_double(dx), wl_fixed_from_double(dy));
    pointer_frame_ = true;
}

void VirtualInput::button(std::uint32_t evdev_button, bool pressed)
{
    zwlr_virtual_pointer_v1_button(pointer_, now_ms(), evdev_button,
                                   pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
    pointer_frame_ = true;
}

void VirtualInput::scroll_discrete(std::int32_t x_v120, std::int32_t y_v120)
{
    scroll_x_ += x_v120;
    scroll_y_ += y_v120;
    const std::int32_t steps_x = scroll_x_ / notch;
    const std::int32_t steps_y = scroll_y_ / notch;
    scroll_x_ -= steps_x * notch;
    scroll_y_ -= steps_y * notch;
    if (steps_x == 0 && steps_y == 0) {
        return;
    }
    const auto time = now_ms();
    zwlr_virtual_pointer_v1_axis_source(pointer_, WL_POINTER_AXIS_SOURCE_WHEEL);
    if (steps_y != 0) {
        zwlr_virtual_pointer_v1_axis_discrete(pointer_, time, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                              wl_fixed_from_double(steps_y * pixels_per_notch), steps_y);
    }
    if (steps_x != 0) {
        zwlr_virtual_pointer_v1_axis_discrete(pointer_, time, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                              wl_fixed_from_double(steps_x * pixels_per_notch), steps_x);
    }
    pointer_frame_ = true;
}

void VirtualInput::text(char32_t codepoint)
{
    const auto keysym = XkbKeymap::keysym_for(codepoint);
    const auto stroke = keymap_->find_or_add(keysym);
    if (!stroke) {
        log::debug(log_component, "cannot type U+{:04X}", static_cast<std::uint32_t>(codepoint));
        return;
    }
    log::trace(log_component, "typing U+{:04X} on key {} with modifiers {:#x}", static_cast<std::uint32_t>(codepoint),
               stroke->key, stroke->modifiers);
    if (keymap_->generation() != keymap_generation_) {
        send_keymap();
    }
    // Exactly the stroke's modifiers for this one key, then the real state again.
    const auto time = now_ms();
    zwp_virtual_keyboard_v1_modifiers(keyboard_, stroke->modifiers, 0, 0, stroke->group);
    zwp_virtual_keyboard_v1_key(keyboard_, time, stroke->key, WL_KEYBOARD_KEY_STATE_PRESSED);
    zwp_virtual_keyboard_v1_key(keyboard_, time, stroke->key, WL_KEYBOARD_KEY_STATE_RELEASED);
    send_modifiers();
}

void VirtualInput::flush()
{
    if (pointer_frame_) {
        zwlr_virtual_pointer_v1_frame(pointer_);
        pointer_frame_ = false;
    }
    connection_->flush();
}

}  // namespace farland::platform::wlroots
