// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>

/// RDP scancodes to Linux evdev key codes (docs/PLAN.md §3.3). The table is
/// fixed and layout-independent: RDP sends key positions (IBM PC/AT set 1, as
/// Windows reports them), evdev names key positions too, and the compositor
/// applies the keyboard layout.
namespace farland::platform {

/// Linux evdev codes, with the values of linux/input-event-codes.h (not
/// available on every platform this library builds on).
namespace evdev {
inline constexpr std::uint32_t key_esc = 1;
inline constexpr std::uint32_t key_1 = 2;
inline constexpr std::uint32_t key_0 = 11;
inline constexpr std::uint32_t key_minus = 12;
inline constexpr std::uint32_t key_equal = 13;
inline constexpr std::uint32_t key_backspace = 14;
inline constexpr std::uint32_t key_tab = 15;
inline constexpr std::uint32_t key_q = 16;
inline constexpr std::uint32_t key_enter = 28;
inline constexpr std::uint32_t key_leftctrl = 29;
inline constexpr std::uint32_t key_a = 30;
inline constexpr std::uint32_t key_grave = 41;
inline constexpr std::uint32_t key_leftshift = 42;
inline constexpr std::uint32_t key_z = 44;
inline constexpr std::uint32_t key_rightshift = 54;
inline constexpr std::uint32_t key_kpasterisk = 55;
inline constexpr std::uint32_t key_leftalt = 56;
inline constexpr std::uint32_t key_space = 57;
inline constexpr std::uint32_t key_capslock = 58;
inline constexpr std::uint32_t key_f1 = 59;
inline constexpr std::uint32_t key_f10 = 68;
inline constexpr std::uint32_t key_numlock = 69;
inline constexpr std::uint32_t key_scrolllock = 70;
inline constexpr std::uint32_t key_kp7 = 71;
inline constexpr std::uint32_t key_kpdot = 83;
inline constexpr std::uint32_t key_zenkakuhankaku = 85;
inline constexpr std::uint32_t key_102nd = 86;
inline constexpr std::uint32_t key_f11 = 87;
inline constexpr std::uint32_t key_f12 = 88;
inline constexpr std::uint32_t key_ro = 89;
inline constexpr std::uint32_t key_katakana = 90;
inline constexpr std::uint32_t key_hiragana = 91;
inline constexpr std::uint32_t key_henkan = 92;
inline constexpr std::uint32_t key_katakanahiragana = 93;
inline constexpr std::uint32_t key_muhenkan = 94;
inline constexpr std::uint32_t key_kpjpcomma = 95;
inline constexpr std::uint32_t key_kpenter = 96;
inline constexpr std::uint32_t key_rightctrl = 97;
inline constexpr std::uint32_t key_kpslash = 98;
inline constexpr std::uint32_t key_sysrq = 99;
inline constexpr std::uint32_t key_rightalt = 100;
inline constexpr std::uint32_t key_home = 102;
inline constexpr std::uint32_t key_up = 103;
inline constexpr std::uint32_t key_pageup = 104;
inline constexpr std::uint32_t key_left = 105;
inline constexpr std::uint32_t key_right = 106;
inline constexpr std::uint32_t key_end = 107;
inline constexpr std::uint32_t key_down = 108;
inline constexpr std::uint32_t key_pagedown = 109;
inline constexpr std::uint32_t key_insert = 110;
inline constexpr std::uint32_t key_delete = 111;
inline constexpr std::uint32_t key_mute = 113;
inline constexpr std::uint32_t key_volumedown = 114;
inline constexpr std::uint32_t key_volumeup = 115;
inline constexpr std::uint32_t key_power = 116;
inline constexpr std::uint32_t key_kpequal = 117;
inline constexpr std::uint32_t key_pause = 119;
inline constexpr std::uint32_t key_kpcomma = 121;
inline constexpr std::uint32_t key_hangeul = 122;
inline constexpr std::uint32_t key_hanja = 123;
inline constexpr std::uint32_t key_yen = 124;
inline constexpr std::uint32_t key_leftmeta = 125;
inline constexpr std::uint32_t key_rightmeta = 126;
inline constexpr std::uint32_t key_compose = 127;
inline constexpr std::uint32_t key_stop = 128;
inline constexpr std::uint32_t key_help = 138;
inline constexpr std::uint32_t key_menu = 139;
inline constexpr std::uint32_t key_calc = 140;
inline constexpr std::uint32_t key_sleep = 142;
inline constexpr std::uint32_t key_wakeup = 143;
inline constexpr std::uint32_t key_mail = 155;
inline constexpr std::uint32_t key_bookmarks = 156;
inline constexpr std::uint32_t key_computer = 157;
inline constexpr std::uint32_t key_back = 158;
inline constexpr std::uint32_t key_forward = 159;
inline constexpr std::uint32_t key_nextsong = 163;
inline constexpr std::uint32_t key_playpause = 164;
inline constexpr std::uint32_t key_previoussong = 165;
inline constexpr std::uint32_t key_stopcd = 166;
inline constexpr std::uint32_t key_homepage = 172;
inline constexpr std::uint32_t key_refresh = 173;
inline constexpr std::uint32_t key_f13 = 183;
inline constexpr std::uint32_t key_f23 = 193;
inline constexpr std::uint32_t key_f24 = 194;
inline constexpr std::uint32_t key_search = 217;
inline constexpr std::uint32_t key_media = 226;
/// The highest key code in the table; every mapped code is at most this.
inline constexpr std::uint32_t key_table_max = 255;

inline constexpr std::uint32_t btn_left = 0x110;
inline constexpr std::uint32_t btn_right = 0x111;
inline constexpr std::uint32_t btn_middle = 0x112;
/// X1, "back" (PTR_XFLAGS_BUTTON1).
inline constexpr std::uint32_t btn_side = 0x113;
/// X2, "forward" (PTR_XFLAGS_BUTTON2).
inline constexpr std::uint32_t btn_extra = 0x114;
}  // namespace evdev

/// The evdev key for the scancode of one RDP keyboard event. `extended` and
/// `extended1` are KBDFLAGS_EXTENDED (the E0 prefix) and KBDFLAGS_EXTENDED1
/// (the E1 prefix), [MS-RDPBCGR] 2.2.8.1.1.3.1.1.1.
///
/// Returns nullopt for scancodes that are not a key: unknown codes, and the
/// "fake shifts" E0 2A and E0 36 that PS/2 keyboards wrap around navigation
/// keys (some clients pass them on).
///
/// Sequences the caller must know about:
/// - Pause is E1 1D followed by a plain 45, which alone would be Num Lock.
///   E1 1D maps to KEY_PAUSE; the caller drops the 45 that follows
///   (InputTranslator does). Ctrl+Pause (Break) is E0 46, also KEY_PAUSE.
/// - Print Screen is E0 37, and 54 while Alt is held (SysRq); both map to KEY_SYSRQ.
/// - Korean keyboards send Hangul and Hanja as F2 and F1, Windows clients as
///   72 and 71; both map to KEY_HANGEUL and KEY_HANJA.
[[nodiscard]] std::optional<std::uint32_t> scancode_to_evdev(std::uint16_t scancode, bool extended,
                                                             bool extended1) noexcept;

}  // namespace farland::platform
