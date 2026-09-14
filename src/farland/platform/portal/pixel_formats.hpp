// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/platform/backend.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/// Pixel formats and damage bookkeeping for the PipeWire capture. Nothing here
/// depends on PipeWire, so the conversions are unit-tested on their own.
namespace farland::platform::portal {

/// The packed 32-bit RGB layouts the capture accepts, named like PipeWire's
/// spa_video_format: the letters give the byte order in memory.
enum class PixelLayout : std::uint8_t { bgrx, bgra, rgbx, rgba, xrgb, argb, xbgr, abgr };

/// All layouts, in the order the capture prefers them (BGRx and BGRA need no
/// conversion).
inline constexpr std::array<PixelLayout, 8> all_pixel_layouts{
    PixelLayout::bgrx, PixelLayout::bgra, PixelLayout::rgbx, PixelLayout::rgba,
    PixelLayout::xrgb, PixelLayout::argb, PixelLayout::xbgr, PixelLayout::abgr,
};

/// The layout of a spa_video_format value, or nullopt for anything else.
[[nodiscard]] std::optional<PixelLayout> layout_from_spa(std::uint32_t spa_format) noexcept;
[[nodiscard]] std::uint32_t to_spa_format(PixelLayout layout) noexcept;
/// The DRM fourcc (drm_fourcc.h) with the same memory layout on little-endian machines.
[[nodiscard]] std::uint32_t drm_fourcc(PixelLayout layout) noexcept;
[[nodiscard]] bool has_alpha(PixelLayout layout) noexcept;

inline constexpr std::uint64_t drm_format_mod_linear = 0;
inline constexpr std::uint64_t drm_format_mod_invalid = 0x00ff'ffff'ffff'ffffULL;

/// Bytes a `width` x `height` image with rows `stride` bytes apart needs.
/// Precondition: stride >= width * 4, width and height > 0.
[[nodiscard]] constexpr std::size_t image_extent(std::size_t stride, std::uint32_t width, std::uint32_t height) noexcept
{
    return (stride * (height - 1U)) + (std::size_t{width} * 4U);
}

/// Converts `width` x `height` pixels of `src` (rows `src_stride` bytes apart,
/// starting at src[0]) to tightly packed BGRX in `dst` (stride width * 4).
/// Preconditions: src.size() >= image_extent(src_stride, width, height),
/// dst.size() >= width * height * 4.
void convert_to_bgrx(PixelLayout layout, std::span<const std::byte> src, std::size_t src_stride, std::uint32_t width,
                     std::uint32_t height, std::span<std::byte> dst);

/// Converts a cursor bitmap to straight-alpha BGRA (CursorImage::pixels).
/// `premultiplied`: the colour channels of `src` are premultiplied by alpha,
/// as mutter and KWin deliver them. Layouts without alpha become opaque.
/// Same preconditions on `src` as convert_to_bgrx.
[[nodiscard]] std::vector<std::byte> convert_cursor(PixelLayout layout, std::span<const std::byte> src,
                                                    std::size_t src_stride, std::uint32_t width, std::uint32_t height,
                                                    bool premultiplied);

/// The intersection of `rect` and `bounds`, or nullopt if it is empty.
[[nodiscard]] std::optional<Rect> intersect(const Rect& rect, const Rect& bounds) noexcept;

/// Collects the damage of frames the session has not taken yet.
class DamageAccumulator {
public:
    /// More rectangles than this collapse into their bounding box.
    static constexpr std::size_t max_rects = 32;

    /// Everything changed.
    void add_full() noexcept;
    /// Adds one changed rectangle; empty rectangles are ignored.
    void add(const Rect& rect);
    [[nodiscard]] bool full() const noexcept { return full_; }
    [[nodiscard]] bool empty() const noexcept { return !full_ && rects_.empty(); }
    /// The collected damage as Frame::damage wants it (empty means
    /// everything), and a reset.
    [[nodiscard]] std::vector<Rect> take();

private:
    std::vector<Rect> rects_;
    bool full_ = false;
};

}  // namespace farland::platform::portal
