// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/proto/input.hpp>

namespace farland::proto {

namespace {

// Slow-path messageType values, [MS-RDPBCGR] 2.2.8.1.1.3.1.1.
constexpr std::uint16_t input_event_sync = 0x0000;
constexpr std::uint16_t input_event_scancode = 0x0004;
constexpr std::uint16_t input_event_unicode = 0x0005;
constexpr std::uint16_t input_event_mouse = 0x8001;
constexpr std::uint16_t input_event_mousex = 0x8002;
constexpr std::uint16_t input_event_mouserel = 0x8004;

// Fast-path eventCode values and flags, [MS-RDPBCGR] 2.2.8.1.2.2.
constexpr std::uint8_t fp_scancode = 0;
constexpr std::uint8_t fp_mouse = 1;
constexpr std::uint8_t fp_mousex = 2;
constexpr std::uint8_t fp_sync = 3;
constexpr std::uint8_t fp_unicode = 4;
constexpr std::uint8_t fp_relmouse = 5;
constexpr std::uint8_t fp_qoe_timestamp = 6;
constexpr std::uint8_t fp_kbd_release = 0x01;
constexpr std::uint8_t fp_kbd_extended = 0x02;
constexpr std::uint8_t fp_kbd_extended1 = 0x04;
constexpr std::uint8_t fp_action_fastpath = 0;
constexpr std::uint8_t fp_flag_secure_checksum = 0x1;
constexpr std::uint8_t fp_flag_encrypted = 0x2;

std::uint16_t keyboard_flags_from_fastpath(std::uint8_t flags)
{
    std::uint16_t out = 0;
    if ((flags & fp_kbd_release) != 0) {
        out |= kbd_flags::release;
    }
    if ((flags & fp_kbd_extended) != 0) {
        out |= kbd_flags::extended;
    }
    if ((flags & fp_kbd_extended1) != 0) {
        out |= kbd_flags::extended1;
    }
    return out;
}

std::uint8_t keyboard_flags_to_fastpath(std::uint16_t flags)
{
    std::uint8_t out = 0;
    if ((flags & kbd_flags::release) != 0) {
        out |= fp_kbd_release;
    }
    if ((flags & kbd_flags::extended) != 0) {
        out |= fp_kbd_extended;
    }
    if ((flags & kbd_flags::extended1) != 0) {
        out |= fp_kbd_extended1;
    }
    return out;
}

template <class Event>
Result<Event> read_pointer_event(Reader& r)
{
    Event e;
    FARLAND_TRY(e.flags, r.u16le());
    FARLAND_TRY(e.x, r.u16le());
    FARLAND_TRY(e.y, r.u16le());
    return e;
}

Result<RelativeMouseEvent> read_relative_event(Reader& r)
{
    RelativeMouseEvent e;
    FARLAND_TRY(e.flags, r.u16le());
    FARLAND_TRY(const std::uint16_t dx, r.u16le());
    FARLAND_TRY(const std::uint16_t dy, r.u16le());
    e.dx = static_cast<std::int16_t>(dx);
    e.dy = static_cast<std::int16_t>(dy);
    return e;
}

}  // namespace

Result<std::vector<InputEvent>> decode_slow_path_input(Reader& payload)
{
    const std::size_t start = payload.offset();
    FARLAND_TRY(const std::uint16_t count, payload.u16le());
    if (count > max_slow_path_input_events) {
        return fail(Errc::limit_exceeded, "too many slow-path input events", start);
    }
    FARLAND_TRY_VOID(payload.skip(2));  // pad2Octets
    std::vector<InputEvent> events;
    events.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        FARLAND_TRY_VOID(payload.skip(4));  // eventTime: ignored, [MS-RDPBCGR] 2.2.8.1.1.3.1
        const std::size_t type_offset = payload.offset();
        FARLAND_TRY(const std::uint16_t type, payload.u16le());
        switch (type) {
        case input_event_sync: {
            FARLAND_TRY_VOID(payload.skip(2));
            FARLAND_TRY(const std::uint32_t toggles, payload.u32le());
            events.emplace_back(SyncEvent{toggles});
            break;
        }
        case input_event_scancode: {
            FARLAND_TRY(const std::uint16_t flags, payload.u16le());
            FARLAND_TRY(const std::uint16_t code, payload.u16le());
            FARLAND_TRY_VOID(payload.skip(2));
            events.emplace_back(KeyboardEvent{flags, code});
            break;
        }
        case input_event_unicode: {
            FARLAND_TRY(const std::uint16_t flags, payload.u16le());
            FARLAND_TRY(const std::uint16_t code, payload.u16le());
            FARLAND_TRY_VOID(payload.skip(2));
            events.emplace_back(UnicodeKeyboardEvent{flags, code});
            break;
        }
        case input_event_mouse: {
            FARLAND_TRY(const auto e, read_pointer_event<MouseEvent>(payload));
            events.emplace_back(e);
            break;
        }
        case input_event_mousex: {
            FARLAND_TRY(const auto e, read_pointer_event<ExtendedMouseEvent>(payload));
            events.emplace_back(e);
            break;
        }
        case input_event_mouserel: {
            FARLAND_TRY(const auto e, read_relative_event(payload));
            events.emplace_back(e);
            break;
        }
        default:
            return fail(Errc::unsupported, "unknown slow-path input event", type_offset);
        }
    }
    return events;
}

void encode_slow_path_input(Writer& w, std::span<const InputEvent> events)
{
    const std::size_t count_pos = w.size();
    w.u16le(0);
    w.u16le(0);
    std::uint16_t count = 0;
    for (const auto& event : events) {
        if (std::holds_alternative<QoeTimestampEvent>(event)) {
            continue;
        }
        w.u32le(0);  // eventTime
        std::visit(
            [&w](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, SyncEvent>) {
                    w.u16le(input_event_sync);
                    w.u16le(0);
                    w.u32le(e.toggle_flags);
                } else if constexpr (std::is_same_v<T, KeyboardEvent> || std::is_same_v<T, UnicodeKeyboardEvent>) {
                    w.u16le(std::is_same_v<T, KeyboardEvent> ? input_event_scancode : input_event_unicode);
                    w.u16le(e.flags);
                    w.u16le(e.code);
                    w.u16le(0);
                } else if constexpr (std::is_same_v<T, MouseEvent> || std::is_same_v<T, ExtendedMouseEvent>) {
                    w.u16le(std::is_same_v<T, MouseEvent> ? input_event_mouse : input_event_mousex);
                    w.u16le(e.flags);
                    w.u16le(e.x);
                    w.u16le(e.y);
                } else if constexpr (std::is_same_v<T, RelativeMouseEvent>) {
                    w.u16le(input_event_mouserel);
                    w.u16le(e.flags);
                    w.u16le(static_cast<std::uint16_t>(e.dx));
                    w.u16le(static_cast<std::uint16_t>(e.dy));
                }
            },
            event);
        ++count;
    }
    w.patch_u16le(count_pos, count);
}

Result<std::vector<InputEvent>> decode_fastpath_input(Reader& pdu)
{
    const std::size_t start = pdu.offset();
    FARLAND_TRY(const std::uint8_t header, pdu.u8());
    if ((header & 0x03U) != fp_action_fastpath) {
        return fail(Errc::invalid_value, "not a fast-path PDU", start);
    }
    const auto flags = static_cast<std::uint8_t>(header >> 6U);
    if ((flags & (fp_flag_encrypted | fp_flag_secure_checksum)) != 0) {
        // [MS-RDPBCGR] 3.3.5.8.1: drop the connection.
        return fail(Errc::invalid_value, "RDP-encrypted fast-path input under Enhanced RDP Security", start);
    }
    FARLAND_TRY(const std::uint8_t length1, pdu.u8());
    std::size_t length = length1;
    if ((length1 & 0x80U) != 0) {
        FARLAND_TRY(const std::uint8_t length2, pdu.u8());
        length = (static_cast<std::size_t>(length1 & 0x7FU) << 8U) | length2;
    }
    if (length != pdu.size()) {
        return fail(Errc::invalid_length, "fast-path length does not match the PDU", start + 1);
    }
    std::size_t count = (header >> 2U) & 0x0FU;
    if (count == 0) {
        FARLAND_TRY(count, pdu.u8());  // numEvents follows when the header count is zero
    }

    std::vector<InputEvent> events;
    events.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t event_offset = pdu.offset();
        FARLAND_TRY(const std::uint8_t event_header, pdu.u8());
        const auto code = static_cast<std::uint8_t>(event_header >> 5U);
        const auto event_flags = static_cast<std::uint8_t>(event_header & 0x1FU);
        switch (code) {
        case fp_scancode: {
            FARLAND_TRY(const std::uint8_t key, pdu.u8());
            events.emplace_back(KeyboardEvent{keyboard_flags_from_fastpath(event_flags), key});
            break;
        }
        case fp_mouse: {
            FARLAND_TRY(const auto e, read_pointer_event<MouseEvent>(pdu));
            events.emplace_back(e);
            break;
        }
        case fp_mousex: {
            FARLAND_TRY(const auto e, read_pointer_event<ExtendedMouseEvent>(pdu));
            events.emplace_back(e);
            break;
        }
        case fp_sync:
            events.emplace_back(SyncEvent{event_flags});
            break;
        case fp_unicode: {
            FARLAND_TRY(const std::uint16_t unit, pdu.u16le());
            const auto release =
                static_cast<std::uint16_t>((event_flags & fp_kbd_release) != 0 ? kbd_flags::release : 0);
            events.emplace_back(UnicodeKeyboardEvent{release, unit});
            break;
        }
        case fp_relmouse: {
            FARLAND_TRY(const auto e, read_relative_event(pdu));
            events.emplace_back(e);
            break;
        }
        case fp_qoe_timestamp: {
            FARLAND_TRY(const std::uint32_t timestamp, pdu.u32le());
            events.emplace_back(QoeTimestampEvent{timestamp});
            break;
        }
        default:
            return fail(Errc::unsupported, "unknown fast-path input event", event_offset);
        }
    }
    FARLAND_TRY_VOID(pdu.expect_end("fast-path input PDU"));
    return events;
}

void encode_fastpath_input(Writer& w, std::span<const InputEvent> events)
{
    FARLAND_ASSERT(events.size() <= 0xFF);
    // A zero count in the header means "numEvents follows" ([MS-RDPBCGR] 2.2.8.1.2),
    // so an empty PDU needs the separate octet as much as a 16-event one.
    const bool count_in_header = !events.empty() && events.size() < 16;
    const std::size_t start = w.size();
    w.u8(static_cast<std::uint8_t>(count_in_header ? events.size() << 2U : 0U));
    w.u16be(0);  // two-octet length, patched below
    if (!count_in_header) {
        w.u8(static_cast<std::uint8_t>(events.size()));
    }
    const auto event_header = [&w](std::uint8_t code, std::uint8_t flags) {
        w.u8(static_cast<std::uint8_t>((static_cast<unsigned>(code) << 5U) | (flags & 0x1FU)));
    };
    for (const auto& event : events) {
        std::visit(
            [&](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, KeyboardEvent>) {
                    FARLAND_ASSERT(e.code <= 0xFF);
                    event_header(fp_scancode, keyboard_flags_to_fastpath(e.flags));
                    w.u8(static_cast<std::uint8_t>(e.code));
                } else if constexpr (std::is_same_v<T, UnicodeKeyboardEvent>) {
                    event_header(fp_unicode, (e.flags & kbd_flags::release) != 0 ? fp_kbd_release : 0);
                    w.u16le(e.code);
                } else if constexpr (std::is_same_v<T, MouseEvent> || std::is_same_v<T, ExtendedMouseEvent>) {
                    event_header(std::is_same_v<T, MouseEvent> ? fp_mouse : fp_mousex, 0);
                    w.u16le(e.flags);
                    w.u16le(e.x);
                    w.u16le(e.y);
                } else if constexpr (std::is_same_v<T, RelativeMouseEvent>) {
                    event_header(fp_relmouse, 0);
                    w.u16le(e.flags);
                    w.u16le(static_cast<std::uint16_t>(e.dx));
                    w.u16le(static_cast<std::uint16_t>(e.dy));
                } else if constexpr (std::is_same_v<T, SyncEvent>) {
                    event_header(fp_sync, static_cast<std::uint8_t>(e.toggle_flags));
                } else if constexpr (std::is_same_v<T, QoeTimestampEvent>) {
                    event_header(fp_qoe_timestamp, 0);
                    w.u32le(e.timestamp);
                }
            },
            event);
    }
    const std::size_t length = w.size() - start;
    FARLAND_ASSERT(length <= 0x7FFF);
    w.patch_u16be(start + 1, static_cast<std::uint16_t>(0x8000U | length));
}

}  // namespace farland::proto
