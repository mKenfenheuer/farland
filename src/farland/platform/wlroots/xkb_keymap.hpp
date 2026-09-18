// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace farland::platform::wlroots {

/// The keymap farland gives the compositor with its virtual keyboard
/// (docs/PLAN.md §7, Unicode input): an XKB layout compiled with
/// xkbcommon, plus keysyms added on demand.
///
/// Keys arrive as evdev codes and pass through unchanged; the keymap only
/// decides what they mean, so the layout should be the client's (CS_CORE
/// keyboardLayout). Characters the client types as Unicode are looked up in
/// the layout: a key and the modifiers that give the keysym on it. A
/// character the layout lacks goes on a spare key, one with a name but no
/// symbols (those at or below keycode 255 first, which X11 clients through
/// Xwayland can see too); the keymap text changes then, and the caller
/// sends it again. When every spare key is taken, the one used longest ago
/// is reused.
///
/// The virtual keyboard protocol leaves the modifier state to the client,
/// so the keymap also keeps an xkb_state that follows the keys.
class XkbKeymap {
public:
    /// The modifier state as zwp_virtual_keyboard_v1.modifiers sends it.
    struct Modifiers {
        std::uint32_t depressed = 0;
        std::uint32_t latched = 0;
        std::uint32_t locked = 0;
        std::uint32_t group = 0;

        friend bool operator==(const Modifiers&, const Modifiers&) = default;
    };

    /// How to type a keysym: this key while exactly these modifiers are held.
    struct Stroke {
        /// An evdev code (XKB keycode - 8).
        std::uint32_t key = 0;
        std::uint32_t modifiers = 0;
        std::uint32_t group = 0;

        friend bool operator==(const Stroke&, const Stroke&) = default;
    };

    /// Compiles `layout` ("de", or with a variant "de(nodeadkeys)"; several
    /// comma-separated ones make groups) with the evdev rules and the pc105
    /// model. Empty: xkbcommon's default ($XKB_DEFAULT_LAYOUT, else us).
    [[nodiscard]] static Result<std::unique_ptr<XkbKeymap>> create(std::string_view layout);

    XkbKeymap(const XkbKeymap&) = delete;
    XkbKeymap& operator=(const XkbKeymap&) = delete;
    XkbKeymap(XkbKeymap&&) = delete;
    XkbKeymap& operator=(XkbKeymap&&) = delete;
    ~XkbKeymap();

    /// The keymap in the XKB text format (XKB_KEYMAP_FORMAT_TEXT_V1),
    /// with the added keysyms.
    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    /// Counts keymap changes: text() differs whenever this does.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    /// Follows a key press or release; true when the modifier state changed.
    bool update_key(std::uint32_t evdev_code, bool pressed);
    [[nodiscard]] Modifiers modifiers() const;

    /// The keysym for a Unicode character (0 when there is none). Control
    /// characters map to their keys: Return, Tab, BackSpace, Escape, Delete.
    [[nodiscard]] static std::uint32_t keysym_for(char32_t codepoint) noexcept;
    /// Where the keymap has `keysym` in the active group; nullopt if nowhere.
    [[nodiscard]] std::optional<Stroke> find(std::uint32_t keysym) const;
    /// find(), and when that fails, puts the keysym on a spare key.
    /// nullopt when the keymap has no spare key or cannot be rebuilt.
    [[nodiscard]] std::optional<Stroke> find_or_add(std::uint32_t keysym);

    /// Spare keys (XKB keycodes) in the order they are used.
    [[nodiscard]] const std::vector<std::uint32_t>& spare_keycodes() const noexcept { return spare_; }

private:
    struct Extra {
        std::uint32_t keysym = 0;
        std::uint32_t keycode = 0;
        std::uint64_t last_used = 0;
    };

    XkbKeymap(xkb_context* context, xkb_keymap* keymap, std::string text);
    /// Rebuilds text_ and the keymap from the base and extras_.
    [[nodiscard]] bool rebuild();

    xkb_context* context_;
    xkb_keymap* keymap_;
    xkb_state* state_;
    /// The layout's keymap as xkbcommon printed it.
    std::string base_;
    std::string text_;
    std::vector<std::uint32_t> spare_;
    std::vector<Extra> extras_;
    std::uint64_t generation_ = 0;
    std::uint64_t clock_ = 0;
};

}  // namespace farland::platform::wlroots
