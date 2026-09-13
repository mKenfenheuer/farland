// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace farland::test {

/// Parses hex (whitespace ignored). Throws std::invalid_argument on bad input,
/// which Catch2 reports as a test failure.
[[nodiscard]] std::vector<std::byte> hex(std::string_view text);

/// The bytes of an ASCII string literal, without a terminator.
[[nodiscard]] std::span<const std::byte> ascii(std::string_view text);

namespace literals {

/// "03 00 00 13"_hex
[[nodiscard]] inline std::vector<std::byte> operator""_hex(const char* text, std::size_t size)
{
    return hex(std::string_view(text, size));
}

}  // namespace literals

}  // namespace farland::test
