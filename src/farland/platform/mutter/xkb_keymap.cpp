// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/platform/mutter/mutter_session.hpp>

#include <algorithm>
#include <cstdlib>
#include <memory>

#if FARLAND_HAVE_XKBCOMMON
#include <xkbcommon/xkbcommon.h>
#endif

namespace farland::platform::mutter {

#if FARLAND_HAVE_XKBCOMMON

namespace {

struct ContextDeleter {
    void operator()(xkb_context* context) const noexcept { xkb_context_unref(context); }
};
struct KeymapDeleter {
    void operator()(xkb_keymap* keymap) const noexcept { xkb_keymap_unref(keymap); }
};

/// "us,de(nodeadkeys)" becomes the layouts "us,de" and the variants ",nodeadkeys".
bool split_layout(std::string_view text, std::string& layouts, std::string& variants)
{
    bool first = true;
    while (true) {
        const auto comma = text.find(',');
        std::string_view entry = text.substr(0, comma);
        std::string_view variant;
        if (const auto open = entry.find('('); open != std::string_view::npos) {
            if (!entry.ends_with(')')) {
                return false;
            }
            variant = entry.substr(open + 1, entry.size() - open - 2);
            entry = entry.substr(0, open);
        }
        if (entry.empty()) {
            return false;
        }
        if (!first) {
            layouts += ',';
            variants += ',';
        }
        first = false;
        layouts += entry;
        variants += variant;
        if (comma == std::string_view::npos) {
            return true;
        }
        text = text.substr(comma + 1);
    }
}

}  // namespace

std::optional<std::string> xkb_keymap_for_layout(std::string_view layout)
{
    const bool plain = std::ranges::all_of(layout, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == ',' || c == '(' || c == ')';
    });
    std::string layouts;
    std::string variants;
    if (layout.empty() || layout.size() > 256 || !plain || !split_layout(layout, layouts, variants)) {
        return std::nullopt;
    }
    const std::unique_ptr<xkb_context, ContextDeleter> context(xkb_context_new(XKB_CONTEXT_NO_FLAGS));
    if (!context) {
        return std::nullopt;
    }
    const xkb_rule_names names{"evdev", "pc105", layouts.c_str(), variants.c_str(), nullptr};
    const std::unique_ptr<xkb_keymap, KeymapDeleter> keymap(
        xkb_keymap_new_from_names(context.get(), &names, XKB_KEYMAP_COMPILE_NO_FLAGS));
    if (!keymap) {
        return std::nullopt;
    }
    char* text = xkb_keymap_get_as_string(keymap.get(), XKB_KEYMAP_FORMAT_TEXT_V1);
    if (text == nullptr) {
        return std::nullopt;
    }
    std::string result(text);
    std::free(text);  // NOLINT(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory): xkbcommon's allocation
    return result;
}

#else

std::optional<std::string> xkb_keymap_for_layout(std::string_view /*layout*/)
{
    return std::nullopt;
}

#endif

}  // namespace farland::platform::mutter
