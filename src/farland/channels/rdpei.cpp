// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/channels/rdpei.hpp>

#include <type_traits>
#include <utility>

namespace farland::channels::rdpei {

namespace {

/// Smallest encodings: a touch contact is contactId, fieldsPresent, x, y and
/// contactFlags of one byte each (a pen contact likewise); a frame is
/// contactCount and frameOffset.
constexpr std::size_t min_contact_size = 5;
constexpr std::size_t min_frame_size = 2;

/// Reads `count` more bytes of a variable-length integer, most significant
/// first, below the `value` from its first byte.
template <class T>
Result<T> read_tail(Reader& r, T value, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        FARLAND_TRY(const auto byte, r.u8());
        value = static_cast<T>((value << 8U) | byte);
    }
    return value;
}

/// Writes the low `count` bytes of `value`, most significant first.
void write_tail(Writer& w, std::uint64_t value, unsigned count)
{
    for (unsigned i = count; i-- > 0;) {
        w.u8(static_cast<std::uint8_t>((value >> (8U * i)) & 0xFFU));
    }
}

/// The number of bytes (1 to `max_bytes`) that hold `value` when the first
/// byte carries `first_bits` of it.
unsigned byte_count(std::uint64_t value, unsigned first_bits, unsigned max_bytes)
{
    for (unsigned n = 1; n < max_bytes; ++n) {
        if (value < (std::uint64_t{1} << (first_bits + (8U * (n - 1))))) {
            return n;
        }
    }
    return max_bytes;
}

/// A counted array: `count` checked against `limit` and against what is
/// left of the PDU at `min_size` bytes per element, then read.
template <class T, class ReadOne>
Result<std::vector<T>> read_array(Reader& r, std::size_t count, std::size_t limit, std::size_t min_size,
                                  std::string_view too_many, ReadOne read_one)
{
    if (count > limit) {
        return fail(Errc::limit_exceeded, too_many, r.offset());
    }
    if (count > r.remaining() / min_size) {
        return fail(Errc::truncated, "array extends past the PDU", r.offset());
    }
    std::vector<T> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        FARLAND_TRY(auto element, read_one(r));
        out.push_back(std::move(element));
    }
    return out;
}

Result<TouchContact> read_touch_contact(Reader& r)
{
    TouchContact c;
    FARLAND_TRY(c.contact_id, r.u8());
    FARLAND_TRY(const auto fields, read_two_byte_unsigned(r));
    if ((fields & ~touch_fields::all) != 0) {
        // Their sizes are unknown, so nothing after them can be read.
        return fail(Errc::unsupported, "unknown RDPINPUT_TOUCH_CONTACT fields", r.offset());
    }
    FARLAND_TRY(c.x, read_four_byte_signed(r));
    FARLAND_TRY(c.y, read_four_byte_signed(r));
    FARLAND_TRY(c.contact_flags, read_four_byte_unsigned(r));
    if ((fields & touch_fields::contact_rect) != 0) {
        ContactRect rect;
        FARLAND_TRY(rect.left, read_two_byte_signed(r));
        FARLAND_TRY(rect.top, read_two_byte_signed(r));
        FARLAND_TRY(rect.right, read_two_byte_signed(r));
        FARLAND_TRY(rect.bottom, read_two_byte_signed(r));
        c.contact_rect = rect;
    }
    if ((fields & touch_fields::orientation) != 0) {
        FARLAND_TRY(c.orientation, read_four_byte_unsigned(r));
    }
    if ((fields & touch_fields::pressure) != 0) {
        FARLAND_TRY(c.pressure, read_four_byte_unsigned(r));
    }
    return c;
}

Result<PenContact> read_pen_contact(Reader& r)
{
    PenContact c;
    FARLAND_TRY(c.device_id, r.u8());
    FARLAND_TRY(const auto fields, read_two_byte_unsigned(r));
    if ((fields & ~pen_fields::all) != 0) {
        return fail(Errc::unsupported, "unknown RDPINPUT_PEN_CONTACT fields", r.offset());
    }
    FARLAND_TRY(c.x, read_four_byte_signed(r));
    FARLAND_TRY(c.y, read_four_byte_signed(r));
    FARLAND_TRY(c.contact_flags, read_four_byte_unsigned(r));
    if ((fields & pen_fields::pen_flags) != 0) {
        FARLAND_TRY(c.pen_flags, read_four_byte_unsigned(r));
    }
    if ((fields & pen_fields::pressure) != 0) {
        FARLAND_TRY(c.pressure, read_four_byte_unsigned(r));
    }
    if ((fields & pen_fields::rotation) != 0) {
        FARLAND_TRY(c.rotation, read_two_byte_unsigned(r));
    }
    if ((fields & pen_fields::tilt_x) != 0) {
        FARLAND_TRY(c.tilt_x, read_two_byte_signed(r));
    }
    if ((fields & pen_fields::tilt_y) != 0) {
        FARLAND_TRY(c.tilt_y, read_two_byte_signed(r));
    }
    return c;
}

/// RDPINPUT_TOUCH_FRAME or RDPINPUT_PEN_FRAME: contactCount, frameOffset, contacts.
template <class Frame, class ReadContact>
Result<Frame> read_frame(Reader& r, std::size_t limit, std::string_view too_many, ReadContact read_contact)
{
    Frame frame;
    FARLAND_TRY(const auto count, read_two_byte_unsigned(r));
    FARLAND_TRY(frame.frame_offset, read_eight_byte_unsigned(r));
    FARLAND_TRY(frame.contacts, (read_array<typename decltype(frame.contacts)::value_type>(
                                    r, count, limit, min_contact_size, too_many, read_contact)));
    return frame;
}

/// RDPINPUT_TOUCH_EVENT_PDU or RDPINPUT_PEN_EVENT_PDU after the header.
template <class Event, class ReadFrame>
Result<Event> read_event(Reader& r, ReadFrame read_one_frame)
{
    Event event;
    FARLAND_TRY(event.encode_time, read_four_byte_unsigned(r));
    FARLAND_TRY(const auto count, read_two_byte_unsigned(r));
    FARLAND_TRY(event.frames, (read_array<typename decltype(event.frames)::value_type>(
                                  r, count, max_frames, min_frame_size, "too many input frames", read_one_frame)));
    return event;
}

Result<TouchEvent> read_touch_event(Reader& r)
{
    return read_event<TouchEvent>(r, [](Reader& fr) {
        return read_frame<TouchFrame>(fr, max_touch_contacts_per_frame, "too many touch contacts in a frame",
                                      read_touch_contact);
    });
}

Result<PenEvent> read_pen_event(Reader& r)
{
    return read_event<PenEvent>(r, [](Reader& fr) {
        return read_frame<PenFrame>(fr, max_pen_contacts_per_frame, "too many pen contacts in a frame",
                                    read_pen_contact);
    });
}

struct Header {
    std::uint16_t event_id = 0;
};

/// RDPINPUT_HEADER of a PDU that must fill `r` exactly.
Result<Header> read_header(Reader& r)
{
    FARLAND_TRY(const auto id, r.u16le());
    FARLAND_TRY(const auto length, r.u32le());
    if (length != r.size()) {
        return fail(Errc::invalid_length, "RDPINPUT_HEADER pduLength does not match the PDU", r.offset() - 4);
    }
    return Header{id};
}

template <class Pdu, class T>
Result<Pdu> finish(Reader& r, Result<T> value, std::string_view what)
{
    if (!value) {
        return std::unexpected(std::move(value).error());
    }
    FARLAND_TRY_VOID(r.expect_end(what));
    return Pdu{std::move(*value)};
}

/// Writes a PDU: header, then `body`, then the length patched in.
template <class Body>
std::vector<std::byte> write_pdu(std::uint16_t event_id, Body body)
{
    Writer w;
    w.u16le(event_id);
    w.u32le(0);
    body(w);
    w.patch_u32le(2, static_cast<std::uint32_t>(w.size()));
    return std::move(w).take();
}

void write_touch_contact(Writer& w, const TouchContact& c)
{
    const auto fields = static_cast<std::uint16_t>((c.contact_rect ? touch_fields::contact_rect : 0U) |
                                                   (c.orientation ? touch_fields::orientation : 0U) |
                                                   (c.pressure ? touch_fields::pressure : 0U));
    w.u8(c.contact_id);
    write_two_byte_unsigned(w, fields);
    write_four_byte_signed(w, c.x);
    write_four_byte_signed(w, c.y);
    write_four_byte_unsigned(w, c.contact_flags);
    if (c.contact_rect) {
        write_two_byte_signed(w, c.contact_rect->left);
        write_two_byte_signed(w, c.contact_rect->top);
        write_two_byte_signed(w, c.contact_rect->right);
        write_two_byte_signed(w, c.contact_rect->bottom);
    }
    if (c.orientation) {
        write_four_byte_unsigned(w, *c.orientation);
    }
    if (c.pressure) {
        write_four_byte_unsigned(w, *c.pressure);
    }
}

void write_pen_contact(Writer& w, const PenContact& c)
{
    const auto fields =
        static_cast<std::uint16_t>((c.pen_flags ? pen_fields::pen_flags : 0U) |
                                   (c.pressure ? pen_fields::pressure : 0U) | (c.rotation ? pen_fields::rotation : 0U) |
                                   (c.tilt_x ? pen_fields::tilt_x : 0U) | (c.tilt_y ? pen_fields::tilt_y : 0U));
    w.u8(c.device_id);
    write_two_byte_unsigned(w, fields);
    write_four_byte_signed(w, c.x);
    write_four_byte_signed(w, c.y);
    write_four_byte_unsigned(w, c.contact_flags);
    if (c.pen_flags) {
        write_four_byte_unsigned(w, *c.pen_flags);
    }
    if (c.pressure) {
        write_four_byte_unsigned(w, *c.pressure);
    }
    if (c.rotation) {
        write_two_byte_unsigned(w, *c.rotation);
    }
    if (c.tilt_x) {
        write_two_byte_signed(w, *c.tilt_x);
    }
    if (c.tilt_y) {
        write_two_byte_signed(w, *c.tilt_y);
    }
}

template <class Event, class WriteContact>
void write_event(Writer& w, const Event& event, std::size_t contact_limit, WriteContact write_contact)
{
    FARLAND_ASSERT(event.frames.size() <= max_frames);
    write_four_byte_unsigned(w, event.encode_time);
    write_two_byte_unsigned(w, static_cast<std::uint16_t>(event.frames.size()));
    for (const auto& frame : event.frames) {
        FARLAND_ASSERT(frame.contacts.size() <= contact_limit);
        write_two_byte_unsigned(w, static_cast<std::uint16_t>(frame.contacts.size()));
        write_eight_byte_unsigned(w, frame.frame_offset);
        for (const auto& contact : frame.contacts) {
            write_contact(w, contact);
        }
    }
}

}  // namespace

Result<std::uint16_t> read_two_byte_unsigned(Reader& r)
{
    FARLAND_TRY(const auto first, r.u8());
    const auto value = static_cast<std::uint16_t>(first & 0x7FU);
    return read_tail<std::uint16_t>(r, value, (first & 0x80U) != 0 ? 1 : 0);
}

Result<std::int16_t> read_two_byte_signed(Reader& r)
{
    FARLAND_TRY(const auto first, r.u8());
    FARLAND_TRY(const auto magnitude,
                read_tail<std::uint16_t>(r, static_cast<std::uint16_t>(first & 0x3FU), (first & 0x80U) != 0 ? 1 : 0));
    const auto value = static_cast<std::int16_t>(magnitude);
    return (first & 0x40U) != 0 ? static_cast<std::int16_t>(-value) : value;
}

Result<std::uint32_t> read_four_byte_unsigned(Reader& r)
{
    FARLAND_TRY(const auto first, r.u8());
    return read_tail<std::uint32_t>(r, first & 0x3FU, first >> 6U);
}

Result<std::int32_t> read_four_byte_signed(Reader& r)
{
    FARLAND_TRY(const auto first, r.u8());
    FARLAND_TRY(const auto magnitude, read_tail<std::uint32_t>(r, first & 0x1FU, first >> 6U));
    const auto value = static_cast<std::int32_t>(magnitude);
    return (first & 0x20U) != 0 ? -value : value;
}

Result<std::uint64_t> read_eight_byte_unsigned(Reader& r)
{
    FARLAND_TRY(const auto first, r.u8());
    return read_tail<std::uint64_t>(r, first & 0x1FU, first >> 5U);
}

void write_two_byte_unsigned(Writer& w, std::uint16_t value)
{
    FARLAND_ASSERT(value <= max_two_byte_unsigned);
    const unsigned n = byte_count(value, 7, 2);
    w.u8(static_cast<std::uint8_t>((n == 2 ? 0x80U : 0U) | (value >> (8U * (n - 1)))));
    write_tail(w, value, n - 1);
}

void write_two_byte_signed(Writer& w, std::int16_t value)
{
    FARLAND_ASSERT(value >= -max_two_byte_signed && value <= max_two_byte_signed);
    const auto magnitude = static_cast<std::uint16_t>(value < 0 ? -value : value);
    const unsigned n = byte_count(magnitude, 6, 2);
    w.u8(static_cast<std::uint8_t>((n == 2 ? 0x80U : 0U) | (value < 0 ? 0x40U : 0U) | (magnitude >> (8U * (n - 1)))));
    write_tail(w, magnitude, n - 1);
}

void write_four_byte_unsigned(Writer& w, std::uint32_t value)
{
    FARLAND_ASSERT(value <= max_four_byte_unsigned);
    const unsigned n = byte_count(value, 6, 4);
    w.u8(static_cast<std::uint8_t>(((n - 1) << 6U) | (value >> (8U * (n - 1)))));
    write_tail(w, value, n - 1);
}

void write_four_byte_signed(Writer& w, std::int32_t value)
{
    FARLAND_ASSERT(value >= -max_four_byte_signed && value <= max_four_byte_signed);
    const auto magnitude = static_cast<std::uint32_t>(value < 0 ? -value : value);
    const unsigned n = byte_count(magnitude, 5, 4);
    w.u8(static_cast<std::uint8_t>(((n - 1) << 6U) | (value < 0 ? 0x20U : 0U) | (magnitude >> (8U * (n - 1)))));
    write_tail(w, magnitude, n - 1);
}

void write_eight_byte_unsigned(Writer& w, std::uint64_t value)
{
    FARLAND_ASSERT(value <= max_eight_byte_unsigned);
    const unsigned n = byte_count(value, 5, 8);
    w.u8(static_cast<std::uint8_t>(((n - 1) << 5U) | (value >> (8U * (n - 1)))));
    write_tail(w, value, n - 1);
}

Result<std::size_t> frame_pdu(std::span<const std::byte> stream, std::size_t max_size)
{
    Reader r(stream);
    FARLAND_TRY_VOID(r.skip(2));
    FARLAND_TRY(const auto length, r.u32le());
    if (length < header_size) {
        return fail(Errc::invalid_length, "RDPINPUT_HEADER pduLength below the header size", 2);
    }
    if (length > max_size) {
        return fail(Errc::limit_exceeded, "RDPINPUT PDU too large", 2);
    }
    if (length > stream.size()) {
        return fail(Errc::truncated, "RDPINPUT PDU extends past the message", 2);
    }
    return std::size_t{length};
}

Result<ClientPdu> decode_client_pdu(std::span<const std::byte> bytes)
{
    Reader r(bytes);
    FARLAND_TRY(const auto header, read_header(r));
    switch (header.event_id) {
    case event_id::cs_ready: {
        CsReady pdu;
        FARLAND_TRY(pdu.flags, r.u32le());
        FARLAND_TRY(pdu.protocol_version, r.u32le());
        FARLAND_TRY(pdu.max_touch_contacts, r.u16le());
        return finish<ClientPdu>(r, Result<CsReady>(pdu), "RDPINPUT_CS_READY_PDU");
    }
    case event_id::touch:
        return finish<ClientPdu>(r, read_touch_event(r), "RDPINPUT_TOUCH_EVENT_PDU");
    case event_id::dismiss_hovering_touch_contact: {
        DismissHoveringContact pdu;
        FARLAND_TRY(pdu.contact_id, r.u8());
        return finish<ClientPdu>(r, Result<DismissHoveringContact>(pdu), "RDPINPUT_DISMISS_HOVERING_TOUCH_CONTACT_PDU");
    }
    case event_id::pen:
        return finish<ClientPdu>(r, read_pen_event(r), "RDPINPUT_PEN_EVENT_PDU");
    default:
        return fail(Errc::unsupported, "RDPINPUT eventId not sent by clients", 0);
    }
}

Result<ServerPdu> decode_server_pdu(std::span<const std::byte> bytes)
{
    Reader r(bytes);
    FARLAND_TRY(const auto header, read_header(r));
    switch (header.event_id) {
    case event_id::sc_ready: {
        ScReady pdu;
        FARLAND_TRY(pdu.protocol_version, r.u32le());
        if (!r.empty()) {
            FARLAND_TRY(pdu.supported_features, r.u32le());
        }
        return finish<ServerPdu>(r, Result<ScReady>(pdu), "RDPINPUT_SC_READY_PDU");
    }
    case event_id::suspend_input:
        return finish<ServerPdu>(r, Result<SuspendInput>(SuspendInput{}), "RDPINPUT_SUSPEND_INPUT_PDU");
    case event_id::resume_input:
        return finish<ServerPdu>(r, Result<ResumeInput>(ResumeInput{}), "RDPINPUT_RESUME_INPUT_PDU");
    default:
        return fail(Errc::unsupported, "RDPINPUT eventId not sent by servers", 0);
    }
}

std::vector<std::byte> encode_client_pdu(const ClientPdu& pdu)
{
    return std::visit(
        [](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, CsReady>) {
                return write_pdu(event_id::cs_ready, [&](Writer& w) {
                    w.u32le(p.flags);
                    w.u32le(p.protocol_version);
                    w.u16le(p.max_touch_contacts);
                });
            } else if constexpr (std::is_same_v<T, TouchEvent>) {
                return write_pdu(event_id::touch, [&](Writer& w) {
                    write_event(w, p, max_touch_contacts_per_frame, write_touch_contact);
                });
            } else if constexpr (std::is_same_v<T, DismissHoveringContact>) {
                return write_pdu(event_id::dismiss_hovering_touch_contact, [&](Writer& w) { w.u8(p.contact_id); });
            } else {
                return write_pdu(event_id::pen,
                                 [&](Writer& w) { write_event(w, p, max_pen_contacts_per_frame, write_pen_contact); });
            }
        },
        pdu);
}

std::vector<std::byte> encode_server_pdu(const ServerPdu& pdu)
{
    return std::visit(
        [](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, ScReady>) {
                return write_pdu(event_id::sc_ready, [&](Writer& w) {
                    w.u32le(p.protocol_version);
                    if (p.supported_features) {
                        w.u32le(*p.supported_features);
                    }
                });
            } else if constexpr (std::is_same_v<T, SuspendInput>) {
                return write_pdu(event_id::suspend_input, [](Writer&) {});
            } else {
                return write_pdu(event_id::resume_input, [](Writer&) {});
            }
        },
        pdu);
}

}  // namespace farland::channels::rdpei
