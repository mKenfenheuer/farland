// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/hexdump.hpp>

#include <algorithm>
#include <format>

namespace farland {

namespace {

constexpr std::size_t bytes_per_row = 16;

int hex_value(char c) noexcept
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool is_space(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

}  // namespace

std::string to_hex(std::span<const std::byte> data)
{
    std::string out;
    out.reserve(data.size() * 3);
    for (const std::byte b : data) {
        if (!out.empty()) {
            out += ' ';
        }
        out += std::format("{:02x}", std::to_integer<unsigned>(b));
    }
    return out;
}

std::string hexdump(std::span<const std::byte> data, std::size_t base_offset)
{
    std::string out;
    for (std::size_t row = 0; row < data.size(); row += bytes_per_row) {
        const auto line = data.subspan(row, std::min(bytes_per_row, data.size() - row));
        out += std::format("{:08x}  ", base_offset + row);
        for (std::size_t i = 0; i < bytes_per_row; ++i) {
            out += i < line.size() ? std::format("{:02x} ", std::to_integer<unsigned>(line[i])) : "   ";
            if (i == 7) {
                out += ' ';
            }
        }
        out += " |";
        for (const std::byte b : line) {
            const auto c = std::to_integer<unsigned char>(b);
            out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
        }
        out += "|\n";
    }
    return out;
}

Result<std::vector<std::byte>> from_hex(std::string_view text)
{
    std::vector<std::byte> out;
    out.reserve(text.size() / 2);
    int high = -1;
    std::size_t high_pos = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (is_space(c)) {
            continue;
        }
        const int nibble = hex_value(c);
        if (nibble < 0) {
            return fail(Errc::invalid_value, "not a hexadecimal digit", i);
        }
        if (high < 0) {
            high = nibble;
            high_pos = i;
        } else {
            out.push_back(static_cast<std::byte>((high << 4) | nibble));
            high = -1;
        }
    }
    if (high >= 0) {
        return fail(Errc::invalid_length, "odd number of hexadecimal digits", high_pos);
    }
    return out;
}

}  // namespace farland
