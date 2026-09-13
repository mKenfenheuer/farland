// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/ber.hpp>

#include <array>

namespace farland::ber {

namespace {

constexpr unsigned class_shift = 6;
constexpr unsigned constructed_bit = 0x20;
constexpr unsigned low_tag_mask = 0x1F;
constexpr unsigned long_form_bit = 0x80;

/// X.690 8.3.2: in a multi-octet INTEGER, the first nine bits must not be
/// all zeros or all ones.
bool redundant_leading_octet(std::byte first, std::byte second) noexcept
{
    const auto lead = std::to_integer<unsigned>(first);
    const bool next_negative = (std::to_integer<unsigned>(second) & 0x80U) != 0;
    return (lead == 0x00U && !next_negative) || (lead == 0xFFU && next_negative);
}

std::span<const std::byte> strip_redundant(std::span<const std::byte> digits) noexcept
{
    while (digits.size() > 1 && redundant_leading_octet(digits[0], digits[1])) {
        digits = digits.subspan(1);
    }
    return digits;
}

Result<std::span<const std::byte>> integer_digits(const Tlv& tlv, Rules rules)
{
    if (tlv.value.empty()) {
        return fail(Errc::invalid_length, "INTEGER has no content octets", tlv.value_offset);
    }
    const auto digits = strip_redundant(tlv.value);
    if (rules == Rules::der && digits.size() != tlv.value.size()) {
        return fail(Errc::invalid_value, "DER INTEGER is not minimally encoded", tlv.value_offset);
    }
    return digits;
}

}  // namespace

Result<Tag> read_tag(Reader& r, Rules /*rules*/)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint8_t first, r.u8());
    Tag tag{static_cast<TagClass>(first >> class_shift), (first & constructed_bit) != 0, first & low_tag_mask};
    if (tag.number != low_tag_mask) {
        return tag;
    }

    // High-tag-number form, X.690 8.1.2.4. The rules below hold for BER and DER alike.
    std::uint32_t number = 0;
    for (unsigned i = 0;; ++i) {
        if (i == 3) {
            return fail(Errc::limit_exceeded, "tag number too large", start);
        }
        FARLAND_TRY(const std::uint8_t octet, r.u8());
        if (i == 0 && octet == long_form_bit) {
            return fail(Errc::invalid_value, "tag number has a leading zero octet", start);
        }
        number = (number << 7U) | (octet & 0x7FU);
        if ((octet & long_form_bit) == 0) {
            break;
        }
    }
    if (number < low_tag_mask) {
        return fail(Errc::invalid_value, "tag number below 31 in high-tag-number form", start);
    }
    tag.number = number;
    return tag;
}

Result<std::size_t> read_length(Reader& r, Rules rules)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint8_t first, r.u8());
    if ((first & long_form_bit) == 0) {
        return first;
    }
    const unsigned count = first & 0x7FU;
    if (count == 0) {
        return fail(Errc::unsupported, "indefinite length", start);
    }
    if (count > max_length_octets) {
        return fail(Errc::limit_exceeded, "length uses too many octets", start);
    }
    std::size_t length = 0;
    for (unsigned i = 0; i < count; ++i) {
        FARLAND_TRY(const std::uint8_t octet, r.u8());
        length = (length << 8U) | octet;
    }
    if (rules == Rules::der) {
        if (length < long_form_bit) {
            return fail(Errc::invalid_length, "DER length below 128 not in short form", start);
        }
        if ((length >> (8U * (count - 1U))) == 0) {
            return fail(Errc::invalid_length, "DER length has a leading zero octet", start);
        }
    }
    return length;
}

Result<Tlv> read_tlv(Reader& r, Rules rules)
{
    FARLAND_TRY(const Tag tag, read_tag(r, rules));
    FARLAND_TRY(const std::size_t length, read_length(r, rules));
    const std::size_t value_offset = r.offset();
    FARLAND_TRY(const auto value, r.bytes(length));
    return Tlv{tag, value, value_offset};
}

Result<Tlv> expect_tlv(Reader& r, Tag expected, Rules rules)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const Tlv tlv, read_tlv(r, rules));
    if (tlv.tag != expected) {
        return fail(Errc::invalid_value, "unexpected tag", start);
    }
    return tlv;
}

Result<Tag> peek_tag(const Reader& r, Rules rules)
{
    Reader copy = r;
    return read_tag(copy, rules);
}

Result<bool> decode_boolean(const Tlv& tlv, Rules rules)
{
    if (tlv.value.size() != 1) {
        return fail(Errc::invalid_length, "BOOLEAN must have one content octet", tlv.value_offset);
    }
    const auto value = std::to_integer<std::uint8_t>(tlv.value[0]);
    if (rules == Rules::der && value != 0x00 && value != 0xFF) {
        return fail(Errc::invalid_value, "DER BOOLEAN must be 0x00 or 0xFF", tlv.value_offset);
    }
    return value != 0;
}

Result<std::int64_t> decode_integer(const Tlv& tlv, Rules rules)
{
    FARLAND_TRY(const auto digits, integer_digits(tlv, rules));
    if (digits.size() > sizeof(std::int64_t)) {
        return fail(Errc::limit_exceeded, "INTEGER does not fit in 64 bits", tlv.value_offset);
    }
    const bool negative = (std::to_integer<unsigned>(digits[0]) & 0x80U) != 0;
    std::uint64_t bits = negative ? ~std::uint64_t{0} : std::uint64_t{0};
    for (const std::byte octet : digits) {
        bits = (bits << 8U) | std::to_integer<std::uint64_t>(octet);
    }
    return static_cast<std::int64_t>(bits);
}

Result<std::uint64_t> decode_unsigned(const Tlv& tlv, Rules rules)
{
    FARLAND_TRY(auto digits, integer_digits(tlv, rules));
    if ((std::to_integer<unsigned>(digits[0]) & 0x80U) != 0) {
        return fail(Errc::invalid_value, "negative INTEGER where a non-negative one is required", tlv.value_offset);
    }
    if (digits.size() > 1 && std::to_integer<unsigned>(digits[0]) == 0) {
        digits = digits.subspan(1);  // the sign octet in front of a value with its top bit set
    }
    if (digits.size() > sizeof(std::uint64_t)) {
        return fail(Errc::limit_exceeded, "INTEGER does not fit in 64 bits", tlv.value_offset);
    }
    std::uint64_t value = 0;
    for (const std::byte octet : digits) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(octet);
    }
    return value;
}

Result<bool> read_boolean(Reader& r, Rules rules, Tag tag)
{
    FARLAND_TRY(const Tlv tlv, expect_tlv(r, tag, rules));
    return decode_boolean(tlv, rules);
}

Result<std::int64_t> read_integer(Reader& r, Rules rules, Tag tag)
{
    FARLAND_TRY(const Tlv tlv, expect_tlv(r, tag, rules));
    return decode_integer(tlv, rules);
}

Result<std::uint64_t> read_unsigned(Reader& r, Rules rules, Tag tag)
{
    FARLAND_TRY(const Tlv tlv, expect_tlv(r, tag, rules));
    return decode_unsigned(tlv, rules);
}

Result<std::span<const std::byte>> read_octet_string(Reader& r, Rules rules, Tag tag)
{
    FARLAND_TRY(const Tlv tlv, expect_tlv(r, tag, rules));
    return tlv.value;
}

Result<Reader> read_constructed(Reader& r, Tag tag, Rules rules)
{
    FARLAND_TRY(const Tlv tlv, expect_tlv(r, tag, rules));
    return tlv.reader();
}

void write_tag(Writer& w, Tag tag)
{
    const auto leading = static_cast<unsigned>(tag.cls) << class_shift | (tag.constructed ? constructed_bit : 0U);
    if (tag.number < low_tag_mask) {
        w.u8(static_cast<std::uint8_t>(leading | tag.number));
        return;
    }
    FARLAND_ASSERT(tag.number <= max_tag_number);
    w.u8(static_cast<std::uint8_t>(leading | low_tag_mask));
    bool started = false;
    for (const unsigned shift : {14U, 7U, 0U}) {
        const unsigned group = (tag.number >> shift) & 0x7FU;
        if (!started && group == 0 && shift != 0) {
            continue;
        }
        started = true;
        w.u8(static_cast<std::uint8_t>(group | (shift != 0 ? long_form_bit : 0U)));
    }
}

void write_length(Writer& w, std::size_t length)
{
    if (length < long_form_bit) {
        w.u8(static_cast<std::uint8_t>(length));
        return;
    }
    unsigned count = 0;
    for (std::size_t rest = length; rest != 0; rest >>= 8U) {
        ++count;
    }
    FARLAND_ASSERT(count <= max_length_octets);
    w.u8(static_cast<std::uint8_t>(long_form_bit | count));
    for (unsigned i = count; i-- > 0;) {
        w.u8(static_cast<std::uint8_t>(length >> (8U * i)));
    }
}

void write_tlv(Writer& w, Tag tag, std::span<const std::byte> value)
{
    write_tag(w, tag);
    write_length(w, value.size());
    w.bytes(value);
}

void write_boolean(Writer& w, bool value, Tag tag)
{
    const std::array octet{value ? std::byte{0xFF} : std::byte{0x00}};
    write_tlv(w, tag, octet);
}

void write_integer(Writer& w, std::int64_t value, Tag tag)
{
    const auto bits = static_cast<std::uint64_t>(value);
    std::array<std::byte, sizeof(bits)> octets{};
    for (std::size_t i = 0; i < octets.size(); ++i) {
        octets[i] = static_cast<std::byte>(static_cast<std::uint8_t>(bits >> (8U * (octets.size() - 1U - i))));
    }
    write_tlv(w, tag, strip_redundant(octets));
}

void write_unsigned(Writer& w, std::uint64_t value, Tag tag)
{
    // One extra leading zero octet so that values with the top bit set stay positive.
    std::array<std::byte, sizeof(value) + 1> octets{};
    for (std::size_t i = 1; i < octets.size(); ++i) {
        octets[i] = static_cast<std::byte>(static_cast<std::uint8_t>(value >> (8U * (octets.size() - 1U - i))));
    }
    write_tlv(w, tag, strip_redundant(octets));
}

void write_octet_string(Writer& w, std::span<const std::byte> value, Tag tag)
{
    write_tlv(w, tag, value);
}

}  // namespace farland::ber
