// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/channels/rdpei_server.hpp>
#include <farland/platform/backend.hpp>
#include <farland/platform/keymap.hpp>
#include <farland/proto/input.hpp>

#include <bitset>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace farland::platform {

/// Toggle key state from a client's Synchronize Event, [MS-RDPBCGR] 2.2.8.1.1.3.1.1.5.
struct LockState {
    bool scroll_lock = false;
    bool num_lock = false;
    bool caps_lock = false;
    bool kana_lock = false;

    friend bool operator==(const LockState&, const LockState&) = default;
};

/// Turns parsed RDP input events into InputSink calls.
///
/// Keys go through scancode_to_evdev(). The translator remembers which keys
/// and buttons it pressed on the sink: a press of something already down is
/// dropped, and so is a release of something that is not. RDP clients
/// auto-repeat by sending the press again; Wayland clients repeat a held key
/// themselves (wl_keyboard.repeat_info), and libei wants every press paired
/// with one release, so keeping the key down is all a repeat needs.
///
/// Unicode events (docs/PLAN.md §7) become InputSink::text() on each press,
/// repeats included; releases are ignored. A character outside the BMP comes
/// as a UTF-16 surrogate pair in two events and is combined. Lone surrogates
/// and U+0000 are dropped.
///
/// Absolute positions are scaled from the client's desktop to the backend's
/// (set_geometry()). Button events carry a position as well; when it differs
/// from the last one sent, the pointer moves there before the button. Wheel
/// rotation comes in units of 1/120 notch, the same as v120. A positive
/// vertical rotation is away from the user and scrolls up, so its sign flips;
/// a positive horizontal rotation scrolls right (Windows' WM_MOUSEHWHEEL, and
/// FreeRDP's X11, Wayland and Windows clients) and passes unchanged.
///
/// A Synchronize Event "resets the server key state to all keys up"
/// ([MS-RDPBCGR] 2.2.8.1.1.3.1.1.5; key-downs for keys still held follow), so
/// everything pressed is released. The toggle state it carries cannot be
/// applied here, because the server does not know the compositor's lock
/// state. It is kept for take_sync(): a backend that can read the actual state
/// taps the lock keys that differ.
///
/// Touch and pen contacts ([MS-RDPEI], validated by RdpeiServer) use the same
/// geometry. A touch contact becomes a touch slot of the same id when the
/// sink accepts touch; otherwise the first contact down drives the pointer
/// with the left button, as a mouse would, and the others are dropped. A pen
/// moves the pointer while it hovers and holds the left button while it
/// touches (the right one when its barrel button is pressed as it comes
/// down): libei has no tablet devices, so pressure, tilt and rotation are
/// dropped.
///
/// Nothing a client sends makes the translator fail; what does not map is dropped.
class InputTranslator {
public:
    explicit InputTranslator(InputSink& sink) noexcept : sink_(&sink) {}

    /// Maps client desktop coordinates (client_width x client_height) onto the
    /// backend desktop (desktop_width x desktop_height with its top-left corner
    /// at desktop_x, desktop_y), clamping to the desktop. Until this is called,
    /// or while any size is zero, positions pass through unchanged.
    void set_geometry(std::uint32_t client_width, std::uint32_t client_height, std::uint32_t desktop_width,
                      std::uint32_t desktop_height, std::int32_t desktop_x = 0, std::int32_t desktop_y = 0) noexcept;

    /// Translates the events of one input PDU, then calls InputSink::flush()
    /// if anything reached the sink.
    void translate(std::span<const proto::InputEvent> events);
    /// Translates the contacts of one touch or pen frame, then flushes if
    /// anything reached the sink.
    void translate(std::span<const channels::rdpei::Contact> contacts);

    /// Releases every key and button still down and cancels every touch,
    /// then flushes if there were any. For the end of the session and for
    /// input focus changes.
    void release_all();

    /// The toggle state of the newest Synchronize Event since the last call;
    /// nullopt if none arrived.
    [[nodiscard]] std::optional<LockState> take_sync() noexcept { return std::exchange(sync_, std::nullopt); }

private:
    struct Geometry {
        std::uint32_t client_width = 0;
        std::uint32_t client_height = 0;
        std::uint32_t desktop_width = 0;
        std::uint32_t desktop_height = 0;
        std::int32_t desktop_x = 0;
        std::int32_t desktop_y = 0;
    };

    /// Buttons by index: left, right, middle, X1, X2.
    static constexpr std::size_t button_count = 5;

    void handle(const proto::KeyboardEvent& event);
    void handle(const proto::UnicodeKeyboardEvent& event);
    void handle(const proto::MouseEvent& event);
    void handle(const proto::ExtendedMouseEvent& event);
    void handle(const proto::RelativeMouseEvent& event);
    void handle(const proto::SyncEvent& event);
    void handle_touch(const channels::rdpei::Contact& contact);
    void handle_pen(const channels::rdpei::Contact& contact);

    void set_key(std::uint32_t code, bool pressed);
    void set_button(std::size_t index, bool pressed);
    void move_to(std::uint16_t x, std::uint16_t y, bool always);
    /// A contact's position (which may lie outside the client desktop) in
    /// desktop pixels.
    [[nodiscard]] std::pair<double, double> map_point(std::int32_t x, std::int32_t y) const;
    /// Moves the pointer to a contact's position.
    void move_pointer(std::int32_t x, std::int32_t y);
    void scroll(std::int32_t x_v120, std::int32_t y_v120);
    void text(char32_t codepoint);
    void release_everything();
    void finish();

    InputSink* sink_;
    std::optional<Geometry> geometry_;
    std::bitset<evdev::key_table_max + 1> keys_down_;
    std::bitset<button_count> buttons_down_;
    /// The client position of the last absolute motion; nullopt when unknown.
    std::optional<std::pair<std::uint16_t, std::uint16_t>> position_;
    std::optional<LockState> sync_;
    /// A high surrogate waiting for its low half; 0 if none.
    std::uint16_t high_surrogate_ = 0;
    /// E1 1D (Pause) was seen, so a 45 that follows is part of it.
    bool pause_pending_ = false;
    /// Touch contacts down on the sink, by contact id.
    std::bitset<256> touches_;
    /// The touch contact emulating the pointer when the sink has no touch.
    std::optional<std::uint8_t> primary_touch_;
    /// The button the pen holds down while it touches.
    std::optional<std::size_t> pen_button_;
    /// Something reached the sink since the last flush().
    bool emitted_ = false;
};

}  // namespace farland::platform
