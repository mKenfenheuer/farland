// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "support/bytes.hpp"

#include <farland/base/hexdump.hpp>

#include <stdexcept>
#include <utility>

namespace farland::test {

std::vector<std::byte> hex(std::string_view text)
{
    auto bytes = from_hex(text);
    if (!bytes.has_value()) {
        throw std::invalid_argument(bytes.error().message());
    }
    return std::move(*bytes);
}

std::span<const std::byte> ascii(std::string_view text)
{
    return std::as_bytes(std::span(text));
}

}  // namespace farland::test
