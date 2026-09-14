// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/codec/image.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

/// Scroll detection for the graphics pipeline (docs/PLAN.md §3.4): finds a
/// region of the previous frame that reappears shifted vertically in the
/// current one, so the pipeline can send it as one SurfaceToSurface instead
/// of re-encoding every tile of a scrolled document. Horizontal scrolling is
/// rare on desktops and not detected.
///
/// Use: call detect_vertical_scroll() with the frame last sent and the new
/// one (when enough tiles changed to make it worth the cost, a few ms at
/// 1080p), send SurfaceToSurface from `source` to `destination()`, apply the
/// same move to the copy of the last frame with apply_scroll(), and diff the
/// tiles against that: only the rows the scroll exposed are left to encode.
/// For H.264 surfaces this is unnecessary (motion estimation does it). With
/// Progressive, the client's codec state of the moved tiles no longer matches
/// their pixels, so they must not get TILE_UPGRADE passes until re-sent.
namespace farland::server {

struct PixelRect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    friend bool operator==(const PixelRect&, const PixelRect&) = default;
};

/// The pixels of `source` in the previous frame are the pixels `dy` rows
/// lower (dy > 0: the content moved down) in the current frame.
struct ScrollMove {
    PixelRect source;
    std::int32_t dy = 0;

    [[nodiscard]] PixelRect destination() const noexcept
    {
        return {source.x, static_cast<std::uint32_t>(std::int64_t{source.y} + dy), source.width, source.height};
    }

    friend bool operator==(const ScrollMove&, const ScrollMove&) = default;
};

struct ScrollDetectorConfig {
    /// Width of the column strips compared independently, so a scrolled
    /// document next to a static sidebar or a moving scrollbar is found.
    std::uint32_t strip_width = 64;
    /// The smallest move worth a SurfaceToSurface.
    std::uint32_t min_rows = 32;
    std::uint32_t min_width = 128;
    /// Largest distance searched; 0: the height of the area.
    std::uint32_t max_distance = 0;
};

/// The largest vertical move inside `area` (clipped to the frames) between
/// `previous` and `current`, which have the same size. Every pixel of the
/// result is verified: its destination in `current` equals its source in
/// `previous`, which also lies inside `area`. nullopt when nothing moved.
[[nodiscard]] std::optional<ScrollMove> detect_vertical_scroll(const codec::ImageView& previous,
                                                               const codec::ImageView& current, const PixelRect& area,
                                                               const ScrollDetectorConfig& config = {});

/// Applies `move` to a BGRX image in place (what the client does for the
/// SurfaceToSurface): the source rows are copied to the destination.
void apply_scroll(std::span<std::byte> image, std::size_t stride, const ScrollMove& move);

}  // namespace farland::server
