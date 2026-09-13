// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace farland {

/// Space-separated lowercase hex, e.g. "03 00 00 13".
[[nodiscard]] std::string to_hex(std::span<const std::byte> data);

/// Classic 16-bytes-per-row dump with offsets and printable ASCII, for logs
/// and test failure messages. `base_offset` is added to the printed offsets.
[[nodiscard]] std::string hexdump(std::span<const std::byte> data, std::size_t base_offset = 0);

/// Parses hex digits, ignoring spaces, tabs and line breaks. For errors,
/// `Error::offset` is the character position in `text`.
[[nodiscard]] Result<std::vector<std::byte>> from_hex(std::string_view text);

}  // namespace farland
