// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/wlroots/xkb_keymap.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <format>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

namespace farland::platform::wlroots {

namespace {

constexpr std::string_view log_component = "platform.wlroots.keymap";
/// XKB keycodes are evdev codes plus 8.
constexpr std::uint32_t evdev_offset = 8;

struct CStringDeleter {
    void operator()(char* text) const noexcept
    {
        // xkbcommon returns malloc()ed text.
        std::free(text);  // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    }
};
using CString = std::unique_ptr<char, CStringDeleter>;

/// The text of a keymap, or empty.
std::string keymap_text(xkb_keymap* keymap)
{
    const CString text(xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1));
    return text ? std::string(text.get()) : std::string();
}

/// "de(nodeadkeys),us" into "de,us" and "nodeadkeys,".
std::pair<std::string, std::string> split_layout(std::string_view layout)
{
    std::string layouts;
    std::string variants;
    bool first = true;
    while (!layout.empty()) {
        const auto comma = layout.find(',');
        std::string_view item = layout.substr(0, comma);
        layout = comma == std::string_view::npos ? std::string_view() : layout.substr(comma + 1);
        std::string_view variant;
        if (const auto open = item.find('('); open != std::string_view::npos && item.ends_with(')')) {
            variant = item.substr(open + 1, item.size() - open - 2);
            item = item.substr(0, open);
        }
        if (!first) {
            layouts += ',';
            variants += ',';
        }
        first = false;
        layouts += item;
        variants += variant;
    }
    return {layouts, variants};
}

}  // namespace

Result<std::unique_ptr<XkbKeymap>> XkbKeymap::create(std::string_view layout)
{
    xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (context == nullptr) {
        return fail(Errc::io, "cannot create an xkbcommon context");
    }
    const auto [layouts, variants] = split_layout(layout);
    xkb_rule_names names{};
    if (!layouts.empty()) {
        names.layout = layouts.c_str();
        names.variant = variants.c_str();
    }
    xkb_keymap* keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (keymap == nullptr) {
        log::error(log_component, "xkbcommon cannot compile the keymap for layout '{}'", layout);
        xkb_context_unref(context);
        return fail(Errc::invalid_value, "cannot compile the keyboard layout");
    }
    std::string text = keymap_text(keymap);
    if (text.empty()) {
        xkb_keymap_unref(keymap);
        xkb_context_unref(context);
        return fail(Errc::io, "cannot print the keymap");
    }
    return std::unique_ptr<XkbKeymap>(new XkbKeymap(context, keymap, std::move(text)));
}

XkbKeymap::XkbKeymap(xkb_context* context, xkb_keymap* keymap, std::string text)
    : context_(context), keymap_(keymap), state_(xkb_state_new(keymap)), base_(text), text_(std::move(text))
{
    // Keys that have a name but no symbols in any group take the extras;
    // keycodes up to 255 come first, since X11 clients cannot see the rest.
    for (std::uint32_t code = xkb_keymap_min_keycode(keymap_); code <= xkb_keymap_max_keycode(keymap_); ++code) {
        if (xkb_keymap_key_get_name(keymap_, code) != nullptr && xkb_keymap_num_layouts_for_key(keymap_, code) == 0) {
            spare_.push_back(code);
        }
    }
    std::ranges::stable_partition(spare_, [](std::uint32_t code) { return code <= 255; });
}

XkbKeymap::~XkbKeymap()
{
    xkb_state_unref(state_);
    xkb_keymap_unref(keymap_);
    xkb_context_unref(context_);
}

bool XkbKeymap::update_key(std::uint32_t evdev_code, bool pressed)
{
    const auto changed = xkb_state_update_key(state_, evdev_code + evdev_offset, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    return (changed & (XKB_STATE_MODS_EFFECTIVE | XKB_STATE_LAYOUT_EFFECTIVE)) != 0;
}

XkbKeymap::Modifiers XkbKeymap::modifiers() const
{
    return Modifiers{
        xkb_state_serialize_mods(state_, XKB_STATE_MODS_DEPRESSED),
        xkb_state_serialize_mods(state_, XKB_STATE_MODS_LATCHED),
        xkb_state_serialize_mods(state_, XKB_STATE_MODS_LOCKED),
        xkb_state_serialize_layout(state_, XKB_STATE_LAYOUT_EFFECTIVE),
    };
}

std::uint32_t XkbKeymap::keysym_for(char32_t codepoint) noexcept
{
    switch (codepoint) {
    case U'\n':
    case U'\r':
        return XKB_KEY_Return;
    case U'\t':
        return XKB_KEY_Tab;
    case U'\b':
        return XKB_KEY_BackSpace;
    case U'\x1b':
        return XKB_KEY_Escape;
    case U'\x7f':
        return XKB_KEY_Delete;
    default:
        break;
    }
    if (codepoint < 0x20 || (codepoint >= 0x80 && codepoint < 0xa0)) {
        return XKB_KEY_NoSymbol;
    }
    return xkb_utf32_to_keysym(static_cast<std::uint32_t>(codepoint));
}

std::optional<XkbKeymap::Stroke> XkbKeymap::find(std::uint32_t keysym) const
{
    if (keysym == XKB_KEY_NoSymbol) {
        return std::nullopt;
    }
    const auto group = xkb_state_serialize_layout(state_, XKB_STATE_LAYOUT_EFFECTIVE);
    std::optional<Stroke> best;
    for (std::uint32_t code = xkb_keymap_min_keycode(keymap_); code <= xkb_keymap_max_keycode(keymap_); ++code) {
        if (group >= xkb_keymap_num_layouts_for_key(keymap_, code)) {
            continue;
        }
        const auto levels = xkb_keymap_num_levels_for_key(keymap_, code, group);
        for (xkb_level_index_t level = 0; level < levels; ++level) {
            const xkb_keysym_t* syms = nullptr;
            if (xkb_keymap_key_get_syms_by_level(keymap_, code, group, level, &syms) != 1 || syms == nullptr ||
                *syms != keysym) {
                continue;
            }
            std::array<xkb_mod_mask_t, 8> masks{};
            const auto count =
                xkb_keymap_key_get_mods_for_level(keymap_, code, group, level, masks.data(), masks.size());
            if (count == 0) {
                continue;
            }
            // The fewest modifiers: Shift+a rather than Shift+Lock+a.
            const auto mask = *std::ranges::min_element(std::span(masks).first(count), {},
                                                        [](xkb_mod_mask_t m) { return std::popcount(m); });
            // Keys X11 clients can see (up to keycode 255) first, then the
            // fewest modifiers.
            const Stroke stroke{code - evdev_offset, mask, group};
            const auto rank = [](const Stroke& st) {
                return std::pair(st.key + evdev_offset > 255, std::popcount(st.modifiers));
            };
            if (!best || rank(stroke) < rank(*best)) {
                best = stroke;
            }
        }
    }
    return best;
}

std::optional<XkbKeymap::Stroke> XkbKeymap::find_or_add(std::uint32_t keysym)
{
    if (keysym == XKB_KEY_NoSymbol) {
        return std::nullopt;
    }
    ++clock_;
    if (const auto it = std::ranges::find(extras_, keysym, &Extra::keysym); it != extras_.end()) {
        it->last_used = clock_;
    }
    auto stroke = find(keysym);
    // A key above 255 still works for Wayland clients, but not for X11
    // ones: a spare key X11 can see is better.
    const bool x11_spare = !spare_.empty() && spare_.front() <= 255;
    if (stroke && (stroke->key + evdev_offset <= 255 || !x11_spare)) {
        return stroke;
    }
    if (spare_.empty()) {
        return stroke;
    }
    std::uint32_t keycode = 0;
    if (extras_.size() < spare_.size()) {
        keycode = spare_[extras_.size()];
        extras_.push_back(Extra{keysym, keycode, clock_});
    } else {
        auto oldest = std::ranges::min_element(extras_, {}, &Extra::last_used);
        keycode = oldest->keycode;
        *oldest = Extra{keysym, keycode, clock_};
    }
    if (!rebuild()) {
        std::erase_if(extras_, [keysym](const Extra& e) { return e.keysym == keysym; });
        static_cast<void>(rebuild());
        return stroke;
    }
    // Extras use one level; the group is the one active now, which the
    // extra key has as its only group anyway (a key with one group
    // answers every group).
    return Stroke{keycode - evdev_offset, 0, xkb_state_serialize_layout(state_, XKB_STATE_LAYOUT_EFFECTIVE)};
}

bool XkbKeymap::rebuild()
{
    // xkbcommon prints each section as `xkb_symbols "..." {` ... `};` at the
    // start of a line; the extra keys go at the end of xkb_symbols.
    const auto symbols = base_.find("xkb_symbols");
    const auto end = symbols == std::string::npos ? std::string::npos : base_.find("\n};", symbols);
    if (end == std::string::npos) {
        log::warn(log_component, "unexpected keymap text: cannot add keys");
        return false;
    }
    std::string extra;
    for (const auto& e : extras_) {
        const char* name = xkb_keymap_key_get_name(keymap_, e.keycode);
        if (name == nullptr) {
            continue;
        }
        extra += std::format("\n\tkey <{}> {{ [ 0x{:x} ] }};", name, e.keysym);
    }
    std::string text = base_.substr(0, end) + extra + base_.substr(end);
    xkb_keymap* keymap =
        xkb_keymap_new_from_string(context_, text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (keymap == nullptr) {
        log::warn(log_component, "xkbcommon rejected the keymap with {} added keys", extras_.size());
        return false;
    }
    // Carry the modifier and group state over; key codes stay the same.
    const auto mods = modifiers();
    xkb_state* state = xkb_state_new(keymap);
    xkb_state_update_mask(state, mods.depressed, mods.latched, mods.locked, 0, 0,
                          xkb_state_serialize_layout(state_, XKB_STATE_LAYOUT_LOCKED));
    xkb_state_unref(state_);
    xkb_keymap_unref(keymap_);
    keymap_ = keymap;
    state_ = state;
    text_ = std::move(text);
    ++generation_;
    return true;
}

}  // namespace farland::platform::wlroots
