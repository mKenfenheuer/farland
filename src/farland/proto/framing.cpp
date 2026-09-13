// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/proto/framing.hpp>
#include <farland/proto/x224.hpp>

namespace farland::proto {

Result<std::optional<FrameInfo>> peek_frame(std::span<const std::byte> buffered)
{
    if (buffered.empty()) {
        return std::nullopt;
    }
    const auto first = std::to_integer<unsigned>(buffered[0]);
    if (first == 0x03) {
        if (buffered.size() < tpkt_header_size) {
            return std::nullopt;
        }
        const std::size_t length =
            (std::to_integer<std::size_t>(buffered[2]) << 8U) | std::to_integer<std::size_t>(buffered[3]);
        if (length < tpkt_header_size + 3) {
            return fail(Errc::invalid_length, "TPKT shorter than the smallest X.224 TPDU", 2);
        }
        return FrameInfo{FrameKind::tpkt, length};
    }
    if ((first & 0x03U) == 0) {
        // [MS-RDPBCGR] 2.2.8.1.2: length1, and length2 when the top bit of length1 is set.
        if (buffered.size() < 2) {
            return std::nullopt;
        }
        const auto length1 = std::to_integer<std::size_t>(buffered[1]);
        if ((length1 & 0x80U) == 0) {
            if (length1 < 2) {
                return fail(Errc::invalid_length, "fast-path PDU shorter than its header", 1);
            }
            return FrameInfo{FrameKind::fastpath, length1};
        }
        if (buffered.size() < 3) {
            return std::nullopt;
        }
        const std::size_t length = ((length1 & 0x7FU) << 8U) | std::to_integer<std::size_t>(buffered[2]);
        if (length < 3) {
            return fail(Errc::invalid_length, "fast-path PDU shorter than its header", 1);
        }
        return FrameInfo{FrameKind::fastpath, length};
    }
    return fail(Errc::invalid_value, "neither a TPKT nor a fast-path PDU", 0);
}

}  // namespace farland::proto
