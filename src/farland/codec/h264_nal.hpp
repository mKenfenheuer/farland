// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/// H.264 byte stream framing ([ITU-H.264-201201] Annex B and 7.3.1): enough to
/// check what an encoder produced and what an AVC420 stream carries, without
/// decoding it.
namespace farland::codec::h264 {

/// nal_unit_type values farland looks at ([ITU-H.264-201201] Table 7-1).
namespace nal_type {
inline constexpr std::uint8_t slice = 1;  ///< Coded slice of a non-IDR picture.
inline constexpr std::uint8_t idr = 5;    ///< Coded slice of an IDR picture.
inline constexpr std::uint8_t sei = 6;
inline constexpr std::uint8_t sps = 7;
inline constexpr std::uint8_t pps = 8;
inline constexpr std::uint8_t aud = 9;  ///< Access unit delimiter.
inline constexpr std::uint8_t filler = 12;
}  // namespace nal_type

/// One NAL unit: the header byte and the payload, without the start code.
struct NalUnit {
    std::uint8_t type = 0;     ///< nal_unit_type (5 bits).
    std::uint8_t ref_idc = 0;  ///< nal_ref_idc (2 bits).
    std::span<const std::byte> data;
};

/// Most NAL units one access unit may contain here.
inline constexpr std::size_t max_nal_units = 4096;

/// Splits an Annex B byte stream into NAL units. The stream starts with a
/// start code (00 00 01, optionally preceded by zero bytes); zero bytes before
/// the next start code or at the end are trailing_zero_8bits and dropped.
/// Rejects an empty stream, a missing leading start code, empty NAL units, a
/// set forbidden_zero_bit and more than max_nal_units units.
[[nodiscard]] Result<std::vector<NalUnit>> split_annex_b(std::span<const std::byte> stream);

/// True if any unit is a coded slice of an IDR picture.
[[nodiscard]] bool contains_idr(std::span<const NalUnit> units) noexcept;

}  // namespace farland::codec::h264
