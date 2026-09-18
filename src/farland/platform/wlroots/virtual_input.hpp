// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/platform/backend.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

struct wl_seat;
struct zwp_virtual_keyboard_v1;
struct zwlr_virtual_pointer_v1;

namespace farland::platform::wayland {
class Connection;
}

namespace farland::platform::wlroots {

class XkbKeymap;

/// Where one screen shows on the client's desktop, and where its output
/// lies in the compositor's layout (logical pixels).
struct ScreenMapping {
    Rect target;
    Rect output;

    friend bool operator==(const ScreenMapping&, const ScreenMapping&) = default;
};

/// A point on the client's desktop in the compositor's layout: inside the
/// target of the screen under it, or clamped into the nearest target when it
/// lies outside all of them (a border between letterboxed screens).
/// nullopt without screens.
[[nodiscard]] std::optional<std::pair<double, double>> map_to_layout(std::span<const ScreenMapping> screens, double x,
                                                                     double y);

/// Client input for a wlroots compositor (docs/PLAN.md §3.3):
/// zwp_virtual_keyboard_v1 with farland's own keymap (XkbKeymap) and
/// zwlr_virtual_pointer_v1.
///
/// - Keys go out as evdev codes, and the modifier state follows them (the
///   protocol leaves it to the client).
/// - text() types a Unicode character: the key that has it in the layout,
///   with its modifiers set for that one stroke, or a spare key the keymap
///   gets for it (the keymap is sent again then).
/// - Absolute motion takes client desktop pixels and goes through the screen
///   mappings (set_layout()) to the compositor's layout, which
///   motion_absolute spans as a whole.
/// - Wheel rotation is sent in whole notches (axis_discrete); the rest waits
///   for the next event.
/// - There is no virtual touch protocol, so accepts_touch() is false and
///   the translator drives the pointer with the first finger.
class VirtualInput final : public InputSink {
public:
    /// Needs the virtual keyboard and pointer managers among the globals.
    [[nodiscard]] static Result<std::unique_ptr<VirtualInput>> create(wayland::Connection& connection, wl_seat* seat,
                                                                      std::string_view keymap_layout);
    VirtualInput(const VirtualInput&) = delete;
    VirtualInput& operator=(const VirtualInput&) = delete;
    VirtualInput(VirtualInput&&) = delete;
    VirtualInput& operator=(VirtualInput&&) = delete;
    ~VirtualInput() override;

    /// The screens and the bounding box of every output in the compositor's
    /// layout. Until this is called, positions are layout pixels.
    void set_layout(std::vector<ScreenMapping> screens, Rect layout_box);

    void key(std::uint32_t evdev_code, bool pressed) override;
    void pointer_motion_absolute(double x, double y) override;
    void pointer_motion_relative(double dx, double dy) override;
    void button(std::uint32_t evdev_button, bool pressed) override;
    void scroll_discrete(std::int32_t x_v120, std::int32_t y_v120) override;
    void text(char32_t codepoint) override;
    void flush() override;

private:
    VirtualInput(wayland::Connection& connection, zwp_virtual_keyboard_v1* keyboard, zwlr_virtual_pointer_v1* pointer,
                 std::unique_ptr<XkbKeymap> keymap);
    void send_keymap();
    void send_modifiers();

    wayland::Connection* connection_;
    zwp_virtual_keyboard_v1* keyboard_;
    zwlr_virtual_pointer_v1* pointer_;
    std::unique_ptr<XkbKeymap> keymap_;
    std::uint64_t keymap_generation_ = 0;
    std::vector<ScreenMapping> screens_;
    std::optional<Rect> layout_box_;
    std::int32_t scroll_x_ = 0;
    std::int32_t scroll_y_ = 0;
    bool pointer_frame_ = false;
};

}  // namespace farland::platform::wlroots
