// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

/// Splits the transport byte stream into PDUs. RDP interleaves two framings on
/// the same connection: TPKT (first octet 0x03) and fast-path (low two bits of
/// the first octet are 0), [MS-RDPBCGR] 2.2.8.1.2 and 2.2.9.1.2.
namespace farland::proto {

enum class FrameKind : std::uint8_t { tpkt, fastpath };

struct FrameInfo {
    FrameKind kind;
    std::size_t length;  ///< Total PDU length, headers included.
};

/// Inspects the buffered bytes. Returns `std::nullopt` until enough bytes have
/// arrived to know the next PDU's length; the PDU itself may still be
/// incomplete, so callers wait for `length` bytes.
[[nodiscard]] Result<std::optional<FrameInfo>> peek_frame(std::span<const std::byte> buffered);

}  // namespace farland::proto
