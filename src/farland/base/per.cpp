// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/base/per.hpp>

namespace farland::per {

namespace {

constexpr unsigned two_octet_length = 0x80;
constexpr unsigned fragmented_length = 0xC0;
constexpr unsigned oid_arc_limit = 0x80;

}  // namespace

Result<std::size_t> read_length(Reader& r)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint8_t first, r.u8());
    if ((first & two_octet_length) == 0) {
        return first;
    }
    if ((first & fragmented_length) == fragmented_length) {
        return fail(Errc::unsupported, "fragmented PER length", start);
    }
    FARLAND_TRY(const std::uint8_t second, r.u8());
    return (static_cast<std::size_t>(first & 0x3FU) << 8U) | second;
}

void write_length(Writer& w, std::size_t length)
{
    if (length < two_octet_length) {
        w.u8(static_cast<std::uint8_t>(length));
        return;
    }
    FARLAND_ASSERT(length <= max_length);
    w.u16be(static_cast<std::uint16_t>(0x8000U | length));
}

Result<std::uint8_t> read_choice(Reader& r)
{
    return r.u8();
}

void write_choice(Writer& w, std::uint8_t choice)
{
    w.u8(choice);
}

Result<std::uint8_t> read_selection(Reader& r)
{
    return r.u8();
}

void write_selection(Writer& w, std::uint8_t selection)
{
    w.u8(selection);
}

Result<std::uint8_t> read_number_of_sets(Reader& r)
{
    return r.u8();
}

void write_number_of_sets(Writer& w, std::uint8_t count)
{
    w.u8(count);
}

Result<void> read_padding(Reader& r, std::size_t count)
{
    return r.skip(count);
}

void write_padding(Writer& w, std::size_t count)
{
    w.zeros(count);
}

Result<std::uint32_t> read_integer(Reader& r)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::size_t length, read_length(r));
    switch (length) {
    case 1:
        return r.u8();
    case 2:
        return r.u16be();
    case 4:
        return r.u32be();
    default:
        return fail(Errc::invalid_length, "PER integer length must be 1, 2 or 4", start);
    }
}

void write_integer(Writer& w, std::uint32_t value)
{
    if (value <= 0xFFU) {
        write_length(w, 1);
        w.u8(static_cast<std::uint8_t>(value));
    } else if (value <= 0xFFFFU) {
        write_length(w, 2);
        w.u16be(static_cast<std::uint16_t>(value));
    } else {
        write_length(w, 4);
        w.u32be(value);
    }
}

Result<std::uint16_t> read_integer16(Reader& r, std::uint16_t min)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint16_t raw, r.u16be());
    if (raw > 0xFFFFU - min) {
        return fail(Errc::invalid_value, "PER integer16 above 65535", start);
    }
    return static_cast<std::uint16_t>(raw + min);
}

void write_integer16(Writer& w, std::uint16_t value, std::uint16_t min)
{
    FARLAND_ASSERT(value >= min);
    w.u16be(static_cast<std::uint16_t>(value - min));
}

Result<std::uint8_t> read_enumerated(Reader& r, std::uint8_t count)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint8_t value, r.u8());
    if (value >= count) {
        return fail(Errc::invalid_value, "PER enumerated value out of range", start);
    }
    return value;
}

void write_enumerated(Writer& w, std::uint8_t value)
{
    w.u8(value);
}

Result<ObjectIdentifier> read_object_identifier(Reader& r)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::size_t length, read_length(r));
    if (length != 5) {
        return fail(Errc::unsupported, "object identifier is not five single-octet arcs", start);
    }
    FARLAND_TRY(const auto octets, r.bytes(length));

    // X.690 8.19.4: the first octet combines the first two arcs.
    ObjectIdentifier oid{};
    const auto first = std::to_integer<std::uint8_t>(octets[0]);
    if (first < 80) {
        oid[0] = static_cast<std::uint8_t>(first / 40U);
        oid[1] = static_cast<std::uint8_t>(first % 40U);
    } else {
        oid[0] = 2;
        oid[1] = static_cast<std::uint8_t>(first - 80U);
    }
    for (std::size_t i = 1; i < octets.size(); ++i) {
        const auto arc = std::to_integer<std::uint8_t>(octets[i]);
        if (arc >= oid_arc_limit) {
            return fail(Errc::unsupported, "object identifier arc is not a single octet", start);
        }
        oid[i + 1] = arc;
    }
    return oid;
}

void write_object_identifier(Writer& w, const ObjectIdentifier& oid)
{
    FARLAND_ASSERT(oid[0] <= 2 && (oid[0] == 2 || oid[1] < 40));
    FARLAND_ASSERT((oid[0] * 40U) + oid[1] <= 0xFFU);
    write_length(w, 5);
    w.u8(static_cast<std::uint8_t>((oid[0] * 40U) + oid[1]));
    for (std::size_t i = 2; i < oid.size(); ++i) {
        FARLAND_ASSERT(oid[i] < oid_arc_limit);
        w.u8(oid[i]);
    }
}

Result<std::span<const std::byte>> read_octet_string(Reader& r, std::size_t min)
{
    FARLAND_TRY(const std::size_t length, read_length(r));
    return r.bytes(length + min);
}

void write_octet_string(Writer& w, std::span<const std::byte> value, std::size_t min)
{
    FARLAND_ASSERT(value.size() >= min);
    write_length(w, value.size() - min);
    w.bytes(value);
}

Result<std::span<const std::byte>> read_numeric_string(Reader& r, std::size_t min)
{
    FARLAND_TRY(const std::size_t length, read_length(r));
    const std::size_t digits = length + min;
    return r.bytes((digits + 1) / 2);
}

void write_numeric_string(Writer& w, std::string_view digits, std::size_t min)
{
    FARLAND_ASSERT(digits.size() >= min);
    write_length(w, digits.size() - min);
    const auto nibble = [&digits](std::size_t i) -> unsigned {
        if (i >= digits.size()) {
            return 0;
        }
        const char c = digits[i];
        FARLAND_ASSERT(c >= '0' && c <= '9');
        return static_cast<unsigned>(c - '0');
    };
    for (std::size_t i = 0; i < digits.size(); i += 2) {
        w.u8(static_cast<std::uint8_t>((nibble(i) << 4U) | nibble(i + 1)));
    }
}

}  // namespace farland::per
