// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/text.hpp>

#include <atomic>
#include <cstdint>

namespace farland {

namespace {

constexpr char32_t replacement = 0xFFFD;

bool is_high_surrogate(std::uint32_t unit) noexcept
{
    return unit >= 0xD800 && unit <= 0xDBFF;
}

bool is_low_surrogate(std::uint32_t unit) noexcept
{
    return unit >= 0xDC00 && unit <= 0xDFFF;
}

void append_utf8(std::string& out, char32_t cp)
{
    const auto put = [&out](std::uint32_t value) {
        out.push_back(static_cast<char>(static_cast<std::uint8_t>(value)));
    };
    const auto v = static_cast<std::uint32_t>(cp);
    if (v < 0x80) {
        put(v);
    } else if (v < 0x800) {
        put(0xC0U | (v >> 6U));
        put(0x80U | (v & 0x3FU));
    } else if (v < 0x10000) {
        put(0xE0U | (v >> 12U));
        put(0x80U | ((v >> 6U) & 0x3FU));
        put(0x80U | (v & 0x3FU));
    } else {
        put(0xF0U | (v >> 18U));
        put(0x80U | ((v >> 12U) & 0x3FU));
        put(0x80U | ((v >> 6U) & 0x3FU));
        put(0x80U | (v & 0x3FU));
    }
}

void append_utf16le(std::vector<std::byte>& out, std::uint32_t unit)
{
    out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(unit)));
    out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(unit >> 8U)));
}

/// Decodes one UTF-8 sequence starting at `pos`, advancing it. Returns U+FFFD
/// (consuming one byte) for anything malformed, overlong or out of range.
char32_t next_utf8(std::string_view text, std::size_t& pos) noexcept
{
    const auto lead = static_cast<std::uint8_t>(text[pos]);
    ++pos;
    if (lead < 0x80) {
        return lead;
    }
    std::size_t extra = 0;
    std::uint32_t cp = 0;
    std::uint32_t min = 0;
    if ((lead & 0xE0U) == 0xC0U) {
        extra = 1;
        cp = lead & 0x1FU;
        min = 0x80;
    } else if ((lead & 0xF0U) == 0xE0U) {
        extra = 2;
        cp = lead & 0x0FU;
        min = 0x800;
    } else if ((lead & 0xF8U) == 0xF0U) {
        extra = 3;
        cp = lead & 0x07U;
        min = 0x10000;
    } else {
        return replacement;
    }
    if (extra > text.size() - pos) {
        return replacement;
    }
    for (std::size_t i = 0; i < extra; ++i) {
        const auto cont = static_cast<std::uint8_t>(text[pos + i]);
        if ((cont & 0xC0U) != 0x80U) {
            return replacement;
        }
        cp = (cp << 6U) | (cont & 0x3FU);
    }
    pos += extra;
    if (cp < min || cp > 0x10FFFF || is_high_surrogate(cp) || is_low_surrogate(cp)) {
        return replacement;
    }
    return cp;
}

}  // namespace

std::string utf16le_to_utf8(std::span<const std::byte> bytes)
{
    std::string out;
    const std::size_t units = bytes.size() / 2;
    const auto unit_at = [&bytes](std::size_t i) {
        return std::to_integer<std::uint32_t>(bytes[2 * i]) |
               (std::to_integer<std::uint32_t>(bytes[(2 * i) + 1]) << 8U);
    };
    for (std::size_t i = 0; i < units; ++i) {
        const std::uint32_t unit = unit_at(i);
        if (unit == 0) {
            return out;
        }
        if (is_high_surrogate(unit) && i + 1 < units && is_low_surrogate(unit_at(i + 1))) {
            const std::uint32_t low = unit_at(i + 1);
            append_utf8(out, static_cast<char32_t>(0x10000U + ((unit - 0xD800U) << 10U) + (low - 0xDC00U)));
            ++i;
        } else if (is_high_surrogate(unit) || is_low_surrogate(unit)) {
            append_utf8(out, replacement);
        } else {
            append_utf8(out, static_cast<char32_t>(unit));
        }
    }
    if (bytes.size() % 2 != 0) {
        append_utf8(out, replacement);
    }
    return out;
}

std::vector<std::byte> utf8_to_utf16le(std::string_view text)
{
    std::vector<std::byte> out;
    out.reserve(text.size() * 2);
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto cp = static_cast<std::uint32_t>(next_utf8(text, pos));
        if (cp >= 0x10000) {
            const std::uint32_t v = cp - 0x10000U;
            append_utf16le(out, 0xD800U + (v >> 10U));
            append_utf16le(out, 0xDC00U + (v & 0x3FFU));
        } else {
            append_utf16le(out, cp);
        }
    }
    return out;
}

void secure_zero(std::span<std::byte> data) noexcept
{
    for (std::byte& b : data) {  // NOLINT(misc-const-correctness): written through a volatile pointer
        *static_cast<volatile std::byte*>(&b) = std::byte{0};
    }
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

}  // namespace farland
