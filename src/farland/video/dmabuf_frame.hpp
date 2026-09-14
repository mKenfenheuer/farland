// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>

/// A captured frame in GPU memory, for encoders that read it without a copy
/// (docs/PLAN.md §3.3: `Frame{dmabuf | shm, format, modifier, damage}`).
///
/// This is what PipeWire's spa_data describe for a SPA_DATA_DmaBuf buffer:
/// one entry per plane, each with its own descriptor (planes of one buffer
/// usually share it), offset and pitch. The descriptors stay owned by the
/// caller; an encoder that imports the buffer keeps its own reference to the
/// memory.
namespace farland::video {

/// DRM fourcc codes (drm_fourcc.h) of the packed 32-bit RGB formats. Names
/// give the bits of a little-endian 32-bit word from high to low, so
/// xrgb8888 is B, G, R, X in memory: PipeWire's BGRx.
namespace drm_fourcc {

[[nodiscard]] constexpr std::uint32_t code(char a, char b, char c, char d) noexcept
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24U);
}

inline constexpr std::uint32_t xrgb8888 = code('X', 'R', '2', '4');
inline constexpr std::uint32_t argb8888 = code('A', 'R', '2', '4');
inline constexpr std::uint32_t xbgr8888 = code('X', 'B', '2', '4');
inline constexpr std::uint32_t abgr8888 = code('A', 'B', '2', '4');
inline constexpr std::uint32_t rgbx8888 = code('R', 'X', '2', '4');
inline constexpr std::uint32_t rgba8888 = code('R', 'A', '2', '4');
inline constexpr std::uint32_t bgrx8888 = code('B', 'X', '2', '4');
inline constexpr std::uint32_t bgra8888 = code('B', 'A', '2', '4');

}  // namespace drm_fourcc

inline constexpr std::uint64_t drm_modifier_linear = 0;
/// DRM_FORMAT_MOD_INVALID: the layout is implied by the buffer (no modifier).
inline constexpr std::uint64_t drm_modifier_invalid = 0x00ff'ffff'ffff'ffffULL;

struct DmabufPlane {
    int fd = -1;
    std::uint32_t offset = 0;
    std::uint32_t pitch = 0;

    friend bool operator==(const DmabufPlane&, const DmabufPlane&) = default;
};

struct DmabufFrame {
    /// A drm_fourcc code.
    std::uint32_t fourcc = drm_fourcc::xrgb8888;
    /// The format modifier of all planes (PipeWire's SPA_FORMAT_VIDEO_modifier).
    std::uint64_t modifier = drm_modifier_linear;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// Planes in use, 1..4. Compressed layouts (AMD DCC, Intel CCS) add
    /// metadata planes to the one colour plane.
    std::uint32_t plane_count = 0;
    std::array<DmabufPlane, 4> planes{};

    friend bool operator==(const DmabufFrame&, const DmabufFrame&) = default;
};

}  // namespace farland::video
