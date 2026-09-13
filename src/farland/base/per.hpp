// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

/// ASN.1 ALIGNED Packed Encoding Rules (ITU-T X.691): the subset used by T.124
/// GCC (Conference Create Request/Response, [MS-RDPBCGR] 2.2.1.3 / 2.2.1.4)
/// and the T.125 MCS domain PDUs (Erect Domain, Attach User, Channel Join,
/// Send Data, [MS-RDPBCGR] 2.2.1.5 - 2.2.1.8).
///
/// This is not a general PER codec. Each function encodes one construct the
/// way mstsc, Windows servers and FreeRDP put it on the wire.
namespace farland::per {

/// Largest length a two-octet determinant can carry (X.691 10.9.3.7).
inline constexpr std::size_t max_length = 0x3FFF;

/// Length determinant. Fragmented lengths (>= 16K) are `Errc::unsupported`.
[[nodiscard]] Result<std::size_t> read_length(Reader& r);
void write_length(Writer& w, std::size_t length);

/// CHOICE index, SEQUENCE optional-field bitmap (selection) and SET OF count:
/// single octets in the RDP subset.
[[nodiscard]] Result<std::uint8_t> read_choice(Reader& r);
void write_choice(Writer& w, std::uint8_t choice);
[[nodiscard]] Result<std::uint8_t> read_selection(Reader& r);
void write_selection(Writer& w, std::uint8_t selection);
[[nodiscard]] Result<std::uint8_t> read_number_of_sets(Reader& r);
void write_number_of_sets(Writer& w, std::uint8_t count);

/// Alignment padding. Content is not checked on input and is zero on output.
[[nodiscard]] Result<void> read_padding(Reader& r, std::size_t count);
void write_padding(Writer& w, std::size_t count);

/// Unconstrained whole number with a length prefix of 1, 2 or 4 octets.
[[nodiscard]] Result<std::uint32_t> read_integer(Reader& r);
void write_integer(Writer& w, std::uint32_t value);

/// Constrained 16-bit whole number with lower bound `min` (e.g. MCS channel
/// IDs, which start at 1001).
[[nodiscard]] Result<std::uint16_t> read_integer16(Reader& r, std::uint16_t min);
void write_integer16(Writer& w, std::uint16_t value, std::uint16_t min);

/// ENUMERATED with `count` alternatives.
[[nodiscard]] Result<std::uint8_t> read_enumerated(Reader& r, std::uint8_t count);
void write_enumerated(Writer& w, std::uint8_t value);

/// OBJECT IDENTIFIER with six single-octet arcs (the shape of the T.124 key).
using ObjectIdentifier = std::array<std::uint8_t, 6>;
/// { itu-t(0) recommendation(0) t(20) t124(124) version(0) 1 }, [MS-RDPBCGR] 2.2.1.3.
inline constexpr ObjectIdentifier t124_02_98_oid{0, 0, 20, 124, 0, 1};
[[nodiscard]] Result<ObjectIdentifier> read_object_identifier(Reader& r);
void write_object_identifier(Writer& w, const ObjectIdentifier& oid);

/// OCTET STRING with lower size bound `min` (the determinant stores size - min).
[[nodiscard]] Result<std::span<const std::byte>> read_octet_string(Reader& r, std::size_t min);
void write_octet_string(Writer& w, std::span<const std::byte> value, std::size_t min);

/// NumericString with lower size bound `min`. Returns the packed octets (two
/// digits per octet) without interpreting them; RDP only ever sends the
/// conference name "1". On output, digits are packed as (digit - '0') nibbles,
/// matching what mstsc and Windows servers send.
[[nodiscard]] Result<std::span<const std::byte>> read_numeric_string(Reader& r, std::size_t min);
void write_numeric_string(Writer& w, std::string_view digits, std::size_t min);

}  // namespace farland::per
