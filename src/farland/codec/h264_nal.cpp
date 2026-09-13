// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// H.264 Annex B byte stream splitting, [ITU-H.264-201201] B.1 and 7.3.1.

#include <farland/codec/h264_nal.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace farland::codec::h264 {

namespace {

constexpr std::size_t start_code_size = 3;  // 00 00 01

/// Position of the next 00 00 01 at or after `from`.
[[nodiscard]] std::optional<std::size_t> find_start_code(std::span<const std::byte> stream, std::size_t from)
{
    for (std::size_t i = from; i + start_code_size <= stream.size(); ++i) {
        if (stream[i] == std::byte{0} && stream[i + 1] == std::byte{0} && stream[i + 2] == std::byte{1}) {
            return i;
        }
    }
    return std::nullopt;
}

}  // namespace

Result<std::vector<NalUnit>> split_annex_b(std::span<const std::byte> stream)
{
    // leading_zero_8bits, then the first start code.
    const auto first_nonzero = std::ranges::find_if(stream, [](std::byte b) { return b != std::byte{0}; });
    const auto leading = static_cast<std::size_t>(first_nonzero - stream.begin());
    if (first_nonzero == stream.end()) {
        return fail(Errc::truncated, "H.264 byte stream holds no NAL unit", 0);
    }
    if (leading < 2 || *first_nonzero != std::byte{1}) {
        return fail(Errc::invalid_value, "H.264 byte stream does not start with a start code", leading);
    }

    std::vector<NalUnit> units;
    std::size_t begin = leading + 1;
    while (true) {
        const auto next = find_start_code(stream, begin);
        std::size_t end = next.value_or(stream.size());
        // Zero bytes before a start code or at the end belong to no NAL unit
        // (the last byte of a NAL unit is never 0x00, 7.4.1).
        while (end > begin && stream[end - 1] == std::byte{0}) {
            --end;
        }
        if (end == begin) {
            return fail(Errc::invalid_length, "empty H.264 NAL unit", begin);
        }
        const auto header = std::to_integer<std::uint8_t>(stream[begin]);
        if ((header & 0x80U) != 0) {
            return fail(Errc::invalid_value, "H.264 forbidden_zero_bit is set", begin);
        }
        if (units.size() == max_nal_units) {
            return fail(Errc::limit_exceeded, "too many H.264 NAL units", begin);
        }
        units.push_back(NalUnit{
            .type = static_cast<std::uint8_t>(header & 0x1FU),
            .ref_idc = static_cast<std::uint8_t>((header >> 5U) & 0x03U),
            .data = stream.subspan(begin, end - begin),
        });
        if (!next.has_value()) {
            break;
        }
        begin = *next + start_code_size;
    }
    return units;
}

bool contains_idr(std::span<const NalUnit> units) noexcept
{
    return std::ranges::any_of(units, [](const NalUnit& unit) { return unit.type == nal_type::idr; });
}

}  // namespace farland::codec::h264
