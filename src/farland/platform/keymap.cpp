// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/platform/keymap.hpp>

#include <array>
#include <span>

namespace farland::platform {

namespace {

struct Entry {
    std::uint8_t scancode;
    std::uint32_t evdev;
};

// Scancodes without a prefix beyond the 83-key block (01 to 53), whose evdev
// codes equal the scancodes by design. The positions follow the USB HID to
// set 1 table Windows uses (Microsoft, "Keyboard Scan Code Specification"),
// and the evdev codes are the ones the kernel reports for the same keys.
constexpr auto plain_keys = std::to_array<Entry>({
    {0x54, evdev::key_sysrq},  // Print Screen while Alt is held
    {0x56, evdev::key_102nd},
    {0x57, evdev::key_f11},
    {0x58, evdev::key_f12},
    {0x59, evdev::key_kpequal},
    {0x5C, evdev::key_kpjpcomma},
    {0x63, evdev::key_help},
    // 64 to 6E are F13 to F23, filled in below.
    {0x70, evdev::key_katakanahiragana},
    {0x71, evdev::key_hanja},    // as Windows clients send Hanja
    {0x72, evdev::key_hangeul},  // as Windows clients send Hangul
    {0x73, evdev::key_ro},
    {0x76, evdev::key_f24},
    {0x77, evdev::key_hiragana},
    {0x78, evdev::key_katakana},
    {0x79, evdev::key_henkan},
    {0x7B, evdev::key_muhenkan},
    {0x7C, evdev::key_tab},  // Windows' type 4 keyboard table has a second Tab here
    {0x7D, evdev::key_yen},
    {0x7E, evdev::key_kpcomma},  // Brazilian keypad separator (ABNT C2)
    {0xF1, evdev::key_hanja},    // as Korean keyboards send it, without a break code
    {0xF2, evdev::key_hangeul},
});

// E0-prefixed scancodes. E0 2A and E0 36 are the fake shifts and stay unmapped.
constexpr auto extended_keys = std::to_array<Entry>({
    {0x10, evdev::key_previoussong},
    {0x19, evdev::key_nextsong},
    {0x1C, evdev::key_kpenter},
    {0x1D, evdev::key_rightctrl},
    {0x20, evdev::key_mute},
    {0x21, evdev::key_calc},
    {0x22, evdev::key_playpause},
    {0x24, evdev::key_stopcd},
    {0x2E, evdev::key_volumedown},
    {0x30, evdev::key_volumeup},
    {0x32, evdev::key_homepage},
    {0x35, evdev::key_kpslash},
    {0x37, evdev::key_sysrq},  // Print Screen
    {0x38, evdev::key_rightalt},
    {0x45, evdev::key_numlock},  // what a Windows keyboard hook reports for Num Lock; some clients pass it on
    {0x46, evdev::key_pause},    // Ctrl+Pause (Break)
    {0x47, evdev::key_home},
    {0x48, evdev::key_up},
    {0x49, evdev::key_pageup},
    {0x4B, evdev::key_left},
    {0x4D, evdev::key_right},
    {0x4F, evdev::key_end},
    {0x50, evdev::key_down},
    {0x51, evdev::key_pagedown},
    {0x52, evdev::key_insert},
    {0x53, evdev::key_delete},
    {0x5B, evdev::key_leftmeta},
    {0x5C, evdev::key_rightmeta},
    // The Menu (Application) key. The kernel reports it as KEY_COMPOSE for PS/2
    // and USB keyboards (HID usage 0x65), and xkeyboard-config maps that to Menu.
    {0x5D, evdev::key_compose},
    {0x5E, evdev::key_power},
    {0x5F, evdev::key_sleep},
    {0x63, evdev::key_wakeup},
    {0x65, evdev::key_search},
    {0x66, evdev::key_bookmarks},
    {0x67, evdev::key_refresh},
    {0x68, evdev::key_stop},
    {0x69, evdev::key_forward},
    {0x6A, evdev::key_back},
    {0x6B, evdev::key_computer},
    {0x6C, evdev::key_mail},
    {0x6D, evdev::key_media},
});

using Table = std::array<std::uint32_t, 256>;

constexpr Table make_table(std::span<const Entry> entries)
{
    Table table{};
    for (const Entry& entry : entries) {
        table[entry.scancode] = entry.evdev;
    }
    return table;
}

constexpr Table plain_table = [] {
    Table table = make_table(plain_keys);
    for (std::uint32_t code = evdev::key_esc; code <= evdev::key_kpdot; ++code) {
        table[code] = code;
    }
    constexpr std::uint32_t f13_scancode = 0x64;
    for (std::uint32_t i = 0; i <= evdev::key_f23 - evdev::key_f13; ++i) {
        table[f13_scancode + i] = evdev::key_f13 + i;
    }
    return table;
}();

constexpr Table extended_table = make_table(extended_keys);

static_assert(plain_table[0x1E] == evdev::key_a);
static_assert(plain_table[0x6E] == evdev::key_f23);
static_assert(extended_table[0x2A] == 0 && extended_table[0x36] == 0, "fake shifts stay unmapped");

// The only E1-prefixed key: Pause is E1 1D, 45 ([MS-RDPBCGR] 2.2.8.1.1.3.1.1.1).
constexpr std::uint16_t e1_pause_scancode = 0x1D;

}  // namespace

std::optional<std::uint32_t> scancode_to_evdev(std::uint16_t scancode, bool extended, bool extended1) noexcept
{
    if (extended1) {
        if (!extended && scancode == e1_pause_scancode) {
            return evdev::key_pause;
        }
        return std::nullopt;
    }
    if (scancode >= plain_table.size()) {
        return std::nullopt;
    }
    const std::uint32_t code = (extended ? extended_table : plain_table)[scancode];
    if (code == 0) {
        return std::nullopt;
    }
    return code;
}

}  // namespace farland::platform
