// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/assert.hpp>
#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

/// ASN.1 Basic and Distinguished Encoding Rules (ITU-T X.690), as used by
/// T.125 MCS Connect-Initial/Response (BER, [MS-RDPBCGR] 2.2.1.3 / 2.2.1.4)
/// and CredSSP TSRequest (DER, [MS-CSSP] 2.2.1).
///
/// Only definite lengths are supported; RDP never uses indefinite ones.
/// Encoding always produces DER, which every BER decoder accepts.
namespace farland::ber {

enum class Rules : std::uint8_t { ber, der };

enum class TagClass : std::uint8_t { universal = 0, application = 1, context = 2, private_use = 3 };

struct Tag {
    TagClass cls = TagClass::universal;
    bool constructed = false;
    std::uint32_t number = 0;

    friend constexpr bool operator==(const Tag&, const Tag&) = default;
};

namespace tags {
inline constexpr Tag boolean{TagClass::universal, false, 1};
inline constexpr Tag integer{TagClass::universal, false, 2};
inline constexpr Tag bit_string{TagClass::universal, false, 3};
inline constexpr Tag octet_string{TagClass::universal, false, 4};
inline constexpr Tag null{TagClass::universal, false, 5};
inline constexpr Tag object_identifier{TagClass::universal, false, 6};
inline constexpr Tag enumerated{TagClass::universal, false, 10};
inline constexpr Tag sequence{TagClass::universal, true, 16};
inline constexpr Tag set{TagClass::universal, true, 17};
}  // namespace tags

/// [APPLICATION n], e.g. MCS Connect-Initial is [APPLICATION 101].
[[nodiscard]] constexpr Tag application(std::uint32_t number, bool constructed = true) noexcept
{
    return Tag{TagClass::application, constructed, number};
}

/// [n] context-specific, e.g. the explicitly tagged TSRequest fields.
[[nodiscard]] constexpr Tag context(std::uint32_t number, bool constructed = true) noexcept
{
    return Tag{TagClass::context, constructed, number};
}

/// Largest tag number accepted: three base-128 octets. RDP uses at most 101.
inline constexpr std::uint32_t max_tag_number = (1U << 21U) - 1U;

/// Largest number of length octets accepted in the long form.
inline constexpr unsigned max_length_octets = 4;

struct Tlv {
    Tag tag;
    std::span<const std::byte> value;
    /// Absolute offset of the first content octet.
    std::size_t value_offset = 0;

    /// A reader over the content octets, keeping absolute offsets.
    [[nodiscard]] Reader reader() const noexcept { return Reader(value, value_offset); }
};

// Decoding ---------------------------------------------------------------

[[nodiscard]] Result<Tag> read_tag(Reader& r, Rules rules);
[[nodiscard]] Result<std::size_t> read_length(Reader& r, Rules rules);
[[nodiscard]] Result<Tlv> read_tlv(Reader& r, Rules rules);
/// `read_tlv`, failing with `Errc::invalid_value` unless the tag is `expected`.
[[nodiscard]] Result<Tlv> expect_tlv(Reader& r, Tag expected, Rules rules);
/// The next tag, without consuming anything. For OPTIONAL fields.
[[nodiscard]] Result<Tag> peek_tag(const Reader& r, Rules rules);

[[nodiscard]] Result<bool> decode_boolean(const Tlv& tlv, Rules rules);
[[nodiscard]] Result<std::int64_t> decode_integer(const Tlv& tlv, Rules rules);
/// INTEGER that must be non-negative, up to 2^64 - 1.
[[nodiscard]] Result<std::uint64_t> decode_unsigned(const Tlv& tlv, Rules rules);

[[nodiscard]] Result<bool> read_boolean(Reader& r, Rules rules, Tag tag = tags::boolean);
[[nodiscard]] Result<std::int64_t> read_integer(Reader& r, Rules rules, Tag tag = tags::integer);
[[nodiscard]] Result<std::uint64_t> read_unsigned(Reader& r, Rules rules, Tag tag = tags::integer);
/// Primitive OCTET STRING only; RDP never sends constructed strings.
[[nodiscard]] Result<std::span<const std::byte>> read_octet_string(Reader& r, Rules rules,
                                                                   Tag tag = tags::octet_string);
/// Reads a TLV with the given tag and returns a reader over its contents.
[[nodiscard]] Result<Reader> read_constructed(Reader& r, Tag tag, Rules rules);

// Encoding (DER) ---------------------------------------------------------

void write_tag(Writer& w, Tag tag);
void write_length(Writer& w, std::size_t length);
void write_tlv(Writer& w, Tag tag, std::span<const std::byte> value);
void write_boolean(Writer& w, bool value, Tag tag = tags::boolean);
void write_integer(Writer& w, std::int64_t value, Tag tag = tags::integer);
void write_unsigned(Writer& w, std::uint64_t value, Tag tag = tags::integer);
void write_octet_string(Writer& w, std::span<const std::byte> value, Tag tag = tags::octet_string);

/// Encodes the contents with `body(Writer&)`, then writes them as one TLV.
template <std::invocable<Writer&> Body>
void write_constructed(Writer& w, Tag tag, Body&& body)
{
    FARLAND_ASSERT(tag.constructed);
    Writer contents;
    std::forward<Body>(body)(contents);
    write_tlv(w, tag, contents.view());
}

}  // namespace farland::ber
