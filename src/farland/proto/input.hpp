// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include <cstdint>
#include <span>
#include <variant>
#include <vector>

/// Client input events, in slow-path (TS_INPUT_PDU_DATA, [MS-RDPBCGR]
/// 2.2.8.1.1.3) and fast-path (TS_FP_INPUT_PDU, 2.2.8.1.2) form. Both decode
/// to the same event types; keyboard flags always use the slow-path values.
namespace farland::proto {

/// Keyboard flags, [MS-RDPBCGR] 2.2.8.1.1.3.1.1.1.
namespace kbd_flags {
inline constexpr std::uint16_t extended = 0x0100;
inline constexpr std::uint16_t extended1 = 0x0200;
inline constexpr std::uint16_t down = 0x4000;
inline constexpr std::uint16_t release = 0x8000;
}  // namespace kbd_flags

/// Pointer flags, [MS-RDPBCGR] 2.2.8.1.1.3.1.1.3.
namespace ptr_flags {
inline constexpr std::uint16_t hwheel = 0x0400;
inline constexpr std::uint16_t wheel = 0x0200;
inline constexpr std::uint16_t wheel_negative = 0x0100;
inline constexpr std::uint16_t wheel_rotation_mask = 0x01FF;
inline constexpr std::uint16_t move = 0x0800;
inline constexpr std::uint16_t down = 0x8000;
inline constexpr std::uint16_t button1 = 0x1000;  ///< left
inline constexpr std::uint16_t button2 = 0x2000;  ///< right
inline constexpr std::uint16_t button3 = 0x4000;  ///< middle
}  // namespace ptr_flags

/// Extended pointer flags, [MS-RDPBCGR] 2.2.8.1.1.3.1.1.4.
namespace ptrx_flags {
inline constexpr std::uint16_t down = 0x8000;
inline constexpr std::uint16_t button1 = 0x0001;
inline constexpr std::uint16_t button2 = 0x0002;
}  // namespace ptrx_flags

/// Toggle key state in synchronize events, [MS-RDPBCGR] 2.2.8.1.1.3.1.1.5.
namespace sync_flags {
inline constexpr std::uint32_t scroll_lock = 0x1;
inline constexpr std::uint32_t num_lock = 0x2;
inline constexpr std::uint32_t caps_lock = 0x4;
inline constexpr std::uint32_t kana_lock = 0x8;
}  // namespace sync_flags

struct KeyboardEvent {
    std::uint16_t flags = 0;  ///< kbd_flags
    std::uint16_t code = 0;   ///< Scancode (set 1)
};
struct UnicodeKeyboardEvent {
    std::uint16_t flags = 0;  ///< kbd_flags::release only
    std::uint16_t code = 0;   ///< UTF-16 code unit
};
struct MouseEvent {
    std::uint16_t flags = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
};
struct ExtendedMouseEvent {
    std::uint16_t flags = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
};
struct RelativeMouseEvent {
    std::uint16_t flags = 0;
    std::int16_t dx = 0;
    std::int16_t dy = 0;
};
struct SyncEvent {
    std::uint32_t toggle_flags = 0;
};
struct QoeTimestampEvent {
    std::uint32_t timestamp = 0;
};

using InputEvent = std::variant<KeyboardEvent, UnicodeKeyboardEvent, MouseEvent, ExtendedMouseEvent, RelativeMouseEvent,
                                SyncEvent, QoeTimestampEvent>;

/// Upper bound on slow-path events per PDU (the field allows 65535).
inline constexpr std::size_t max_slow_path_input_events = 512;

/// TS_INPUT_PDU_DATA: the payload of a slow-path Input PDU.
[[nodiscard]] Result<std::vector<InputEvent>> decode_slow_path_input(Reader& payload);
/// Encodes TS_INPUT_PDU_DATA. QoE timestamps have no slow-path form and are skipped.
void encode_slow_path_input(Writer& w, std::span<const InputEvent> events);

/// A complete fast-path input PDU, header included. Encrypted PDUs are
/// rejected: RDP encryption is never used under Enhanced RDP Security.
[[nodiscard]] Result<std::vector<InputEvent>> decode_fastpath_input(Reader& pdu);
void encode_fastpath_input(Writer& w, std::span<const InputEvent> events);

}  // namespace farland::proto
