// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-FileCopyrightText: 2012 Marc-Andre Moreau <marcandre.moreau@gmail.com>
// SPDX-License-Identifier: Apache-2.0
//
// freerdp_pairs is derived from FreeRDP 3 (winpr/libwinpr/input/keycode.c and
// scancode.c) and modified: FreeRDP maps evdev codes to Windows virtual keys
// (KEYCODE_TO_VKCODE_EVDEV) and virtual keys to scancodes
// (GetVirtualScanCodesFromVirtualKeyCode). The pairs are both steps chained
// for every evdev code that has a virtual key, on keyboard type 4 (IBM
// enhanced) or, where type 4 has no scancode, type 7 (Japanese). They were
// produced by compiling those two files and printing the chain.

#include <farland/platform/keymap.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>

#if __has_include(<linux/input-event-codes.h>)
#include <linux/input-event-codes.h>
#endif

namespace platform = farland::platform;
namespace evdev = farland::platform::evdev;

namespace {

#if __has_include(<linux/input-event-codes.h>)
static_assert(evdev::key_esc == KEY_ESC);
static_assert(evdev::key_1 == KEY_1);
static_assert(evdev::key_0 == KEY_0);
static_assert(evdev::key_minus == KEY_MINUS);
static_assert(evdev::key_equal == KEY_EQUAL);
static_assert(evdev::key_backspace == KEY_BACKSPACE);
static_assert(evdev::key_tab == KEY_TAB);
static_assert(evdev::key_q == KEY_Q);
static_assert(evdev::key_enter == KEY_ENTER);
static_assert(evdev::key_leftctrl == KEY_LEFTCTRL);
static_assert(evdev::key_a == KEY_A);
static_assert(evdev::key_grave == KEY_GRAVE);
static_assert(evdev::key_leftshift == KEY_LEFTSHIFT);
static_assert(evdev::key_z == KEY_Z);
static_assert(evdev::key_rightshift == KEY_RIGHTSHIFT);
static_assert(evdev::key_kpasterisk == KEY_KPASTERISK);
static_assert(evdev::key_leftalt == KEY_LEFTALT);
static_assert(evdev::key_space == KEY_SPACE);
static_assert(evdev::key_capslock == KEY_CAPSLOCK);
static_assert(evdev::key_f1 == KEY_F1);
static_assert(evdev::key_f10 == KEY_F10);
static_assert(evdev::key_numlock == KEY_NUMLOCK);
static_assert(evdev::key_scrolllock == KEY_SCROLLLOCK);
static_assert(evdev::key_kp7 == KEY_KP7);
static_assert(evdev::key_kpdot == KEY_KPDOT);
static_assert(evdev::key_zenkakuhankaku == KEY_ZENKAKUHANKAKU);
static_assert(evdev::key_102nd == KEY_102ND);
static_assert(evdev::key_f11 == KEY_F11);
static_assert(evdev::key_f12 == KEY_F12);
static_assert(evdev::key_ro == KEY_RO);
static_assert(evdev::key_katakana == KEY_KATAKANA);
static_assert(evdev::key_hiragana == KEY_HIRAGANA);
static_assert(evdev::key_henkan == KEY_HENKAN);
static_assert(evdev::key_katakanahiragana == KEY_KATAKANAHIRAGANA);
static_assert(evdev::key_muhenkan == KEY_MUHENKAN);
static_assert(evdev::key_kpjpcomma == KEY_KPJPCOMMA);
static_assert(evdev::key_kpenter == KEY_KPENTER);
static_assert(evdev::key_rightctrl == KEY_RIGHTCTRL);
static_assert(evdev::key_kpslash == KEY_KPSLASH);
static_assert(evdev::key_sysrq == KEY_SYSRQ);
static_assert(evdev::key_rightalt == KEY_RIGHTALT);
static_assert(evdev::key_home == KEY_HOME);
static_assert(evdev::key_up == KEY_UP);
static_assert(evdev::key_pageup == KEY_PAGEUP);
static_assert(evdev::key_left == KEY_LEFT);
static_assert(evdev::key_right == KEY_RIGHT);
static_assert(evdev::key_end == KEY_END);
static_assert(evdev::key_down == KEY_DOWN);
static_assert(evdev::key_pagedown == KEY_PAGEDOWN);
static_assert(evdev::key_insert == KEY_INSERT);
static_assert(evdev::key_delete == KEY_DELETE);
static_assert(evdev::key_mute == KEY_MUTE);
static_assert(evdev::key_volumedown == KEY_VOLUMEDOWN);
static_assert(evdev::key_volumeup == KEY_VOLUMEUP);
static_assert(evdev::key_power == KEY_POWER);
static_assert(evdev::key_kpequal == KEY_KPEQUAL);
static_assert(evdev::key_pause == KEY_PAUSE);
static_assert(evdev::key_kpcomma == KEY_KPCOMMA);
static_assert(evdev::key_hangeul == KEY_HANGEUL);
static_assert(evdev::key_hanja == KEY_HANJA);
static_assert(evdev::key_yen == KEY_YEN);
static_assert(evdev::key_leftmeta == KEY_LEFTMETA);
static_assert(evdev::key_rightmeta == KEY_RIGHTMETA);
static_assert(evdev::key_compose == KEY_COMPOSE);
static_assert(evdev::key_stop == KEY_STOP);
static_assert(evdev::key_help == KEY_HELP);
static_assert(evdev::key_menu == KEY_MENU);
static_assert(evdev::key_calc == KEY_CALC);
static_assert(evdev::key_sleep == KEY_SLEEP);
static_assert(evdev::key_wakeup == KEY_WAKEUP);
static_assert(evdev::key_mail == KEY_MAIL);
static_assert(evdev::key_bookmarks == KEY_BOOKMARKS);
static_assert(evdev::key_computer == KEY_COMPUTER);
static_assert(evdev::key_back == KEY_BACK);
static_assert(evdev::key_forward == KEY_FORWARD);
static_assert(evdev::key_nextsong == KEY_NEXTSONG);
static_assert(evdev::key_playpause == KEY_PLAYPAUSE);
static_assert(evdev::key_previoussong == KEY_PREVIOUSSONG);
static_assert(evdev::key_stopcd == KEY_STOPCD);
static_assert(evdev::key_homepage == KEY_HOMEPAGE);
static_assert(evdev::key_refresh == KEY_REFRESH);
static_assert(evdev::key_f13 == KEY_F13);
static_assert(evdev::key_f23 == KEY_F23);
static_assert(evdev::key_f24 == KEY_F24);
static_assert(evdev::key_search == KEY_SEARCH);
static_assert(evdev::key_media == KEY_MEDIA);
static_assert(evdev::btn_left == BTN_LEFT);
static_assert(evdev::btn_right == BTN_RIGHT);
static_assert(evdev::btn_middle == BTN_MIDDLE);
static_assert(evdev::btn_side == BTN_SIDE);
static_assert(evdev::btn_extra == BTN_EXTRA);
#endif

struct Pair {
    std::uint16_t scancode;  ///< 0x100 marks the E0 prefix, as FreeRDP's KBDEXT does
    std::uint32_t evdev;
};

constexpr auto freerdp_pairs = std::to_array<Pair>({
    // clang-format off
    {0x001, 1},  // type 4
    {0x002, 2},  // type 4
    {0x003, 3},  // type 4
    {0x004, 4},  // type 4
    {0x005, 5},  // type 4
    {0x006, 6},  // type 4
    {0x007, 7},  // type 4
    {0x008, 8},  // type 4
    {0x009, 9},  // type 4
    {0x00a, 10},  // type 4
    {0x00b, 11},  // type 4
    {0x00c, 12},  // type 4
    {0x00d, 13},  // type 4
    {0x00e, 14},  // type 4
    {0x00f, 15},  // type 4
    {0x07c, 15},  // type 4
    {0x010, 16},  // type 4
    {0x011, 17},  // type 4
    {0x012, 18},  // type 4
    {0x013, 19},  // type 4
    {0x014, 20},  // type 4
    {0x015, 21},  // type 4
    {0x016, 22},  // type 4
    {0x017, 23},  // type 4
    {0x018, 24},  // type 4
    {0x019, 25},  // type 4
    {0x01a, 26},  // type 4
    {0x01b, 27},  // type 4
    {0x01c, 28},  // type 4
    {0x01d, 29},  // type 4
    {0x01e, 30},  // type 4
    {0x01f, 31},  // type 4
    {0x020, 32},  // type 4
    {0x021, 33},  // type 4
    {0x022, 34},  // type 4
    {0x023, 35},  // type 4
    {0x024, 36},  // type 4
    {0x025, 37},  // type 4
    {0x026, 38},  // type 4
    {0x027, 39},  // type 4
    {0x028, 40},  // type 4
    {0x029, 41},  // type 4
    {0x02a, 42},  // type 4
    {0x02b, 43},  // type 4
    {0x02c, 44},  // type 4
    {0x02d, 45},  // type 4
    {0x02e, 46},  // type 4
    {0x02f, 47},  // type 4
    {0x030, 48},  // type 4
    {0x031, 49},  // type 4
    {0x032, 50},  // type 4
    {0x033, 51},  // type 4
    {0x034, 52},  // type 4
    {0x035, 53},  // type 4
    {0x036, 54},  // type 4
    {0x037, 55},  // type 4
    {0x038, 56},  // type 4
    {0x039, 57},  // type 4
    {0x03a, 58},  // type 4
    {0x03b, 59},  // type 4
    {0x03c, 60},  // type 4
    {0x03d, 61},  // type 4
    {0x03e, 62},  // type 4
    {0x03f, 63},  // type 4
    {0x040, 64},  // type 4
    {0x041, 65},  // type 4
    {0x042, 66},  // type 4
    {0x043, 67},  // type 4
    {0x044, 68},  // type 4
    {0x045, 69},  // type 4
    {0x046, 70},  // type 4
    {0x047, 71},  // type 4
    {0x048, 72},  // type 4
    {0x049, 73},  // type 4
    {0x04a, 74},  // type 4
    {0x04b, 75},  // type 4
    {0x04c, 76},  // type 4
    {0x04d, 77},  // type 4
    {0x04e, 78},  // type 4
    {0x04f, 79},  // type 4
    {0x050, 80},  // type 4
    {0x051, 81},  // type 4
    {0x052, 82},  // type 4
    {0x053, 83},  // type 4
    {0x056, 86},  // type 4
    {0x057, 87},  // type 4
    {0x058, 88},  // type 4
    {0x073, 89},  // type 4
    {0x05b, 90},  // type 4
    {0x070, 92},  // type 7
    {0x079, 93},  // type 7
    {0x11c, 96},  // type 4
    {0x11d, 97},  // type 4
    {0x135, 98},  // type 4
    {0x137, 99},  // type 4
    {0x138, 100},  // type 4
    {0x147, 102},  // type 4
    {0x148, 103},  // type 4
    {0x149, 104},  // type 4
    {0x14b, 105},  // type 4
    {0x14d, 106},  // type 4
    {0x14f, 107},  // type 4
    {0x150, 108},  // type 4
    {0x151, 109},  // type 4
    {0x152, 110},  // type 4
    {0x153, 111},  // type 4
    {0x120, 113},  // type 4
    {0x12e, 114},  // type 4
    {0x130, 115},  // type 4
    {0x146, 119},  // type 4
    {0x07e, 121},  // type 4
    {0x07d, 124},  // type 7
    {0x15b, 125},  // type 4
    {0x15c, 126},  // type 4
    {0x063, 138},  // type 4
    {0x15d, 139},  // type 4
    {0x070, 147},  // type 7
    {0x16c, 155},  // type 4
    {0x166, 156},  // type 4
    {0x16a, 158},  // type 4
    {0x169, 159},  // type 4
    {0x119, 163},  // type 4
    {0x122, 164},  // type 4
    {0x110, 165},  // type 4
    {0x124, 166},  // type 4
    {0x132, 172},  // type 4
    {0x167, 173},  // type 4
    {0x064, 183},  // type 4
    {0x065, 184},  // type 4
    {0x066, 185},  // type 4
    {0x067, 186},  // type 4
    {0x068, 187},  // type 4
    {0x069, 188},  // type 4
    {0x06a, 189},  // type 4
    {0x06b, 190},  // type 4
    {0x06c, 191},  // type 4
    {0x06d, 192},  // type 4
    {0x06e, 193},  // type 4
    {0x076, 194},  // type 4
    {0x165, 217},  // type 4
    // clang-format on
});

/// Where farland differs from FreeRDP on purpose.
struct Deviation {
    std::uint16_t scancode;
    std::optional<std::uint32_t> farland;
};

const auto deviations = std::to_array<Deviation>({
    // The Menu key. The kernel reports it as KEY_COMPOSE for PS/2 and USB
    // keyboards (HID usage 0x65), and xkeyboard-config maps KEY_COMPOSE to
    // Menu; FreeRDP picks KEY_MENU.
    {0x15D, evdev::key_compose},
    // An artifact in FreeRDP: VK_DBE_KATAKANA and VK_OEM_FINISH are both 0xF1,
    // and 5B is VK_OEM_FINISH. Katakana is 78.
    {0x05B, std::nullopt},
    // FreeRDP's type 7 table has VK_CONVERT at 70 and VK_HKTG at 79. Its own
    // scancode.h (RDP_SCANCODE_HIRAGANA 70, RDP_SCANCODE_CONVERT_JP 79),
    // Windows' Japanese keyboard layout and the kernel have them the other way round.
    {0x070, evdev::key_katakanahiragana},
    {0x079, evdev::key_henkan},
});

std::optional<std::uint32_t> lookup(std::uint16_t scancode)
{
    return platform::scancode_to_evdev(scancode & 0xFFU, (scancode & 0x100U) != 0, false);
}

std::optional<std::uint32_t> plain(std::uint16_t scancode)
{
    return platform::scancode_to_evdev(scancode, false, false);
}

std::optional<std::uint32_t> extended(std::uint16_t scancode)
{
    return platform::scancode_to_evdev(scancode, true, false);
}

}  // namespace

TEST_CASE("the scancode table agrees with FreeRDP's evdev table")
{
    for (const Pair& pair : freerdp_pairs) {
        CAPTURE(pair.scancode, pair.evdev);
        const Deviation* deviation = nullptr;
        for (const Deviation& d : deviations) {
            if (d.scancode == pair.scancode) {
                deviation = &d;
            }
        }
        if (deviation != nullptr) {
            CHECK(lookup(pair.scancode) == deviation->farland);
        } else {
            CHECK(lookup(pair.scancode) == pair.evdev);
        }
    }
    // Every deviation is still one.
    for (const Deviation& d : deviations) {
        CAPTURE(d.scancode);
        bool listed = false;
        for (const Pair& pair : freerdp_pairs) {
            listed = listed || (pair.scancode == d.scancode && pair.evdev != d.farland);
        }
        CHECK(listed);
    }
}

TEST_CASE("the 83-key block maps to equal evdev codes")
{
    for (std::uint16_t scancode = 0x01; scancode <= 0x53; ++scancode) {
        CAPTURE(scancode);
        CHECK(plain(scancode) == scancode);
    }
}

TEST_CASE("extended keys")
{
    CHECK(plain(0x1D) == evdev::key_leftctrl);
    CHECK(extended(0x1D) == evdev::key_rightctrl);
    CHECK(plain(0x38) == evdev::key_leftalt);
    CHECK(extended(0x38) == evdev::key_rightalt);
    CHECK(plain(0x1C) == evdev::key_enter);
    CHECK(extended(0x1C) == evdev::key_kpenter);
    CHECK(plain(0x35) == 53);  // KEY_SLASH
    CHECK(extended(0x35) == evdev::key_kpslash);
    CHECK(plain(0x48) == 72);  // KEY_KP8
    CHECK(extended(0x48) == evdev::key_up);
    CHECK(extended(0x50) == evdev::key_down);
    CHECK(extended(0x4B) == evdev::key_left);
    CHECK(extended(0x4D) == evdev::key_right);
    CHECK(extended(0x47) == evdev::key_home);
    CHECK(extended(0x4F) == evdev::key_end);
    CHECK(extended(0x49) == evdev::key_pageup);
    CHECK(extended(0x51) == evdev::key_pagedown);
    CHECK(extended(0x52) == evdev::key_insert);
    CHECK(extended(0x53) == evdev::key_delete);
    CHECK(extended(0x5B) == evdev::key_leftmeta);
    CHECK(extended(0x5C) == evdev::key_rightmeta);
    CHECK(extended(0x5D) == evdev::key_compose);
    CHECK(extended(0x5E) == evdev::key_power);
    CHECK(extended(0x5F) == evdev::key_sleep);
    CHECK(extended(0x63) == evdev::key_wakeup);
    CHECK(extended(0x20) == evdev::key_mute);
    CHECK(extended(0x2E) == evdev::key_volumedown);
    CHECK(extended(0x30) == evdev::key_volumeup);
    CHECK(extended(0x22) == evdev::key_playpause);
    CHECK(extended(0x24) == evdev::key_stopcd);
    CHECK(extended(0x19) == evdev::key_nextsong);
    CHECK(extended(0x10) == evdev::key_previoussong);
    CHECK(extended(0x6A) == evdev::key_back);
    CHECK(extended(0x69) == evdev::key_forward);
    CHECK(extended(0x67) == evdev::key_refresh);
    CHECK(extended(0x68) == evdev::key_stop);
    CHECK(extended(0x65) == evdev::key_search);
    CHECK(extended(0x66) == evdev::key_bookmarks);
    CHECK(extended(0x32) == evdev::key_homepage);
    CHECK(extended(0x6C) == evdev::key_mail);
    CHECK(extended(0x21) == evdev::key_calc);
    CHECK(extended(0x6B) == evdev::key_computer);
    CHECK(extended(0x6D) == evdev::key_media);
}

TEST_CASE("Pause, Break, Print Screen and SysRq")
{
    CHECK(platform::scancode_to_evdev(0x1D, false, true) == evdev::key_pause);
    CHECK_FALSE(platform::scancode_to_evdev(0x45, false, true).has_value());
    CHECK_FALSE(platform::scancode_to_evdev(0x1D, true, true).has_value());
    CHECK(extended(0x46) == evdev::key_pause);
    CHECK(plain(0x46) == evdev::key_scrolllock);
    CHECK(plain(0x45) == evdev::key_numlock);
    CHECK(extended(0x45) == evdev::key_numlock);
    CHECK(extended(0x37) == evdev::key_sysrq);
    CHECK(plain(0x37) == evdev::key_kpasterisk);
    CHECK(plain(0x54) == evdev::key_sysrq);
}

TEST_CASE("fake shifts are not keys")
{
    CHECK_FALSE(extended(0x2A).has_value());
    CHECK_FALSE(extended(0x36).has_value());
    CHECK(plain(0x2A) == evdev::key_leftshift);
    CHECK(plain(0x36) == evdev::key_rightshift);
}

TEST_CASE("Japanese, Korean and Brazilian keys")
{
    CHECK(plain(0x29) == evdev::key_grave);  // Hankaku/Zenkaku; the jp layout maps the position
    CHECK(plain(0x70) == evdev::key_katakanahiragana);
    CHECK(plain(0x73) == evdev::key_ro);
    CHECK(plain(0x77) == evdev::key_hiragana);
    CHECK(plain(0x78) == evdev::key_katakana);
    CHECK(plain(0x79) == evdev::key_henkan);
    CHECK(plain(0x7B) == evdev::key_muhenkan);
    CHECK(plain(0x7D) == evdev::key_yen);
    CHECK(plain(0x5C) == evdev::key_kpjpcomma);
    CHECK(plain(0x72) == evdev::key_hangeul);
    CHECK(plain(0xF2) == evdev::key_hangeul);
    CHECK(plain(0x71) == evdev::key_hanja);
    CHECK(plain(0xF1) == evdev::key_hanja);
    CHECK(plain(0x7E) == evdev::key_kpcomma);
    CHECK(plain(0x56) == evdev::key_102nd);
}

TEST_CASE("F11 to F24")
{
    CHECK(plain(0x57) == evdev::key_f11);
    CHECK(plain(0x58) == evdev::key_f12);
    for (std::uint16_t i = 0; i <= 10; ++i) {
        CAPTURE(i);
        CHECK(plain(static_cast<std::uint16_t>(0x64 + i)) == evdev::key_f13 + i);
    }
    CHECK(plain(0x76) == evdev::key_f24);
}

TEST_CASE("anything else is not a key")
{
    CHECK_FALSE(plain(0x00).has_value());
    CHECK_FALSE(extended(0x00).has_value());
    CHECK_FALSE(plain(0x55).has_value());
    CHECK_FALSE(plain(0x80).has_value());  // a set 1 break code; RDP sends releases as a flag
    CHECK_FALSE(plain(0xAA).has_value());
    CHECK_FALSE(extended(0xF2).has_value());
    CHECK_FALSE(plain(0x100).has_value());
    CHECK_FALSE(plain(0xFFFF).has_value());

    std::size_t bad = 0;
    for (std::uint32_t scancode = 0; scancode <= 0xFFFF; ++scancode) {
        for (unsigned prefixes = 0; prefixes < 4; ++prefixes) {
            const auto code = platform::scancode_to_evdev(static_cast<std::uint16_t>(scancode), (prefixes & 1U) != 0,
                                                          (prefixes & 2U) != 0);
            if (code.has_value() && (*code == 0 || *code > evdev::key_table_max)) {
                ++bad;
            }
        }
    }
    CHECK(bad == 0);
}
