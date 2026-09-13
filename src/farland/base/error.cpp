// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/base/error.hpp>

#include <cstdlib>
#include <format>
#include <iostream>

namespace farland {

std::string_view to_string(Errc code) noexcept
{
    switch (code) {
    case Errc::truncated:
        return "truncated";
    case Errc::invalid_value:
        return "invalid value";
    case Errc::invalid_length:
        return "invalid length";
    case Errc::unsupported:
        return "unsupported";
    case Errc::limit_exceeded:
        return "limit exceeded";
    case Errc::trailing_data:
        return "trailing data";
    }
    return "unknown error";
}

std::string Error::message() const
{
    return std::format("{} at offset {}: {}", to_string(code), offset, what);
}

namespace detail {

void assert_failed(const char* expression, const char* file, int line) noexcept
{
    std::cerr << file << ':' << line << ": assertion failed: " << expression << '\n';
    std::abort();
}

}  // namespace detail

}  // namespace farland
