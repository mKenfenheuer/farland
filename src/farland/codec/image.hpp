// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace farland::codec {

/// 32-bit pixels in memory order B, G, R, X (DRM XRGB8888 / PipeWire BGRx on
/// little-endian). Rows are top-down; `stride` is in bytes and at least
/// `width * 4`. The X byte is ignored by the encoders.
struct ImageView {
    std::span<const std::byte> data;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t stride = 0;
};

}  // namespace farland::codec
