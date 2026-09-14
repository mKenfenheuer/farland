// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/platform/portal/pixel_formats.hpp>

#include <spa/param/video/raw.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace farland::platform::portal {

// The layouts are defined by byte order, the conversions below work on
// little-endian 32-bit loads.
static_assert(std::endian::native == std::endian::little, "farland's PipeWire capture assumes little-endian");

namespace {

constexpr std::uint32_t fourcc(char a, char b, char c, char d) noexcept
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24U);
}

/// Moves one pixel, loaded as a little-endian word, to B, G, R, X/A order.
std::uint32_t to_bgra_order(PixelLayout layout, std::uint32_t v) noexcept
{
    switch (layout) {
    case PixelLayout::bgrx:
    case PixelLayout::bgra:
        return v;
    case PixelLayout::rgbx:
    case PixelLayout::rgba:
        return (v & 0xff00ff00U) | ((v >> 16U) & 0xffU) | ((v & 0xffU) << 16U);
    case PixelLayout::xrgb:
    case PixelLayout::argb:
        return std::byteswap(v);
    case PixelLayout::xbgr:
    case PixelLayout::abgr:
        return std::rotr(v, 8);
    }
    return v;
}

template <PixelLayout L>
void convert_row(std::span<const std::byte> src, std::span<std::byte> dst) noexcept
{
    const std::size_t pixels = dst.size() / 4U;
    for (std::size_t i = 0; i < pixels; ++i) {
        std::uint32_t v = 0;
        std::memcpy(&v, src.subspan(i * 4U, 4U).data(), 4U);
        v = to_bgra_order(L, v);
        std::memcpy(dst.subspan(i * 4U, 4U).data(), &v, 4U);
    }
}

template <PixelLayout L>
void convert_rows(std::span<const std::byte> src, std::size_t src_stride, std::uint32_t width, std::uint32_t height,
                  std::span<std::byte> dst) noexcept
{
    const std::size_t row_bytes = std::size_t{width} * 4U;
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto src_row = src.subspan(y * src_stride, row_bytes);
        const auto dst_row = dst.subspan(y * row_bytes, row_bytes);
        if constexpr (L == PixelLayout::bgrx || L == PixelLayout::bgra) {
            std::memcpy(dst_row.data(), src_row.data(), row_bytes);
        } else {
            convert_row<L>(src_row, dst_row);
        }
    }
}

std::byte unpremultiply(std::byte channel, unsigned alpha) noexcept
{
    const auto c = std::to_integer<unsigned>(channel);
    return static_cast<std::byte>(std::min(255U, ((c * 255U) + (alpha / 2U)) / alpha));
}

}  // namespace

std::optional<PixelLayout> layout_from_spa(std::uint32_t spa_format) noexcept
{
    switch (spa_format) {
    case SPA_VIDEO_FORMAT_BGRx:
        return PixelLayout::bgrx;
    case SPA_VIDEO_FORMAT_BGRA:
        return PixelLayout::bgra;
    case SPA_VIDEO_FORMAT_RGBx:
        return PixelLayout::rgbx;
    case SPA_VIDEO_FORMAT_RGBA:
        return PixelLayout::rgba;
    case SPA_VIDEO_FORMAT_xRGB:
        return PixelLayout::xrgb;
    case SPA_VIDEO_FORMAT_ARGB:
        return PixelLayout::argb;
    case SPA_VIDEO_FORMAT_xBGR:
        return PixelLayout::xbgr;
    case SPA_VIDEO_FORMAT_ABGR:
        return PixelLayout::abgr;
    default:
        return std::nullopt;
    }
}

std::uint32_t to_spa_format(PixelLayout layout) noexcept
{
    switch (layout) {
    case PixelLayout::bgrx:
        return SPA_VIDEO_FORMAT_BGRx;
    case PixelLayout::bgra:
        return SPA_VIDEO_FORMAT_BGRA;
    case PixelLayout::rgbx:
        return SPA_VIDEO_FORMAT_RGBx;
    case PixelLayout::rgba:
        return SPA_VIDEO_FORMAT_RGBA;
    case PixelLayout::xrgb:
        return SPA_VIDEO_FORMAT_xRGB;
    case PixelLayout::argb:
        return SPA_VIDEO_FORMAT_ARGB;
    case PixelLayout::xbgr:
        return SPA_VIDEO_FORMAT_xBGR;
    case PixelLayout::abgr:
        return SPA_VIDEO_FORMAT_ABGR;
    }
    return SPA_VIDEO_FORMAT_UNKNOWN;
}

std::uint32_t drm_fourcc(PixelLayout layout) noexcept
{
    // DRM names the bits of a little-endian word, PipeWire the bytes in memory.
    switch (layout) {
    case PixelLayout::bgrx:
        return fourcc('X', 'R', '2', '4');  // DRM_FORMAT_XRGB8888
    case PixelLayout::bgra:
        return fourcc('A', 'R', '2', '4');  // DRM_FORMAT_ARGB8888
    case PixelLayout::rgbx:
        return fourcc('X', 'B', '2', '4');  // DRM_FORMAT_XBGR8888
    case PixelLayout::rgba:
        return fourcc('A', 'B', '2', '4');  // DRM_FORMAT_ABGR8888
    case PixelLayout::xrgb:
        return fourcc('B', 'X', '2', '4');  // DRM_FORMAT_BGRX8888
    case PixelLayout::argb:
        return fourcc('B', 'A', '2', '4');  // DRM_FORMAT_BGRA8888
    case PixelLayout::xbgr:
        return fourcc('R', 'X', '2', '4');  // DRM_FORMAT_RGBX8888
    case PixelLayout::abgr:
        return fourcc('R', 'A', '2', '4');  // DRM_FORMAT_RGBA8888
    }
    return 0;
}

bool has_alpha(PixelLayout layout) noexcept
{
    return layout == PixelLayout::bgra || layout == PixelLayout::rgba || layout == PixelLayout::argb ||
           layout == PixelLayout::abgr;
}

void convert_to_bgrx(PixelLayout layout, std::span<const std::byte> src, std::size_t src_stride, std::uint32_t width,
                     std::uint32_t height, std::span<std::byte> dst)
{
    if (width == 0 || height == 0) {
        return;
    }
    FARLAND_ASSERT(src_stride >= std::size_t{width} * 4U);
    FARLAND_ASSERT(src.size() >= image_extent(src_stride, width, height));
    FARLAND_ASSERT(dst.size() >= std::size_t{width} * height * 4U);
    switch (layout) {
    case PixelLayout::bgrx:
        convert_rows<PixelLayout::bgrx>(src, src_stride, width, height, dst);
        break;
    case PixelLayout::bgra:
        convert_rows<PixelLayout::bgra>(src, src_stride, width, height, dst);
        break;
    case PixelLayout::rgbx:
        convert_rows<PixelLayout::rgbx>(src, src_stride, width, height, dst);
        break;
    case PixelLayout::rgba:
        convert_rows<PixelLayout::rgba>(src, src_stride, width, height, dst);
        break;
    case PixelLayout::xrgb:
        convert_rows<PixelLayout::xrgb>(src, src_stride, width, height, dst);
        break;
    case PixelLayout::argb:
        convert_rows<PixelLayout::argb>(src, src_stride, width, height, dst);
        break;
    case PixelLayout::xbgr:
        convert_rows<PixelLayout::xbgr>(src, src_stride, width, height, dst);
        break;
    case PixelLayout::abgr:
        convert_rows<PixelLayout::abgr>(src, src_stride, width, height, dst);
        break;
    }
}

std::vector<std::byte> convert_cursor(PixelLayout layout, std::span<const std::byte> src, std::size_t src_stride,
                                      std::uint32_t width, std::uint32_t height, bool premultiplied)
{
    std::vector<std::byte> pixels(std::size_t{width} * height * 4U);
    convert_to_bgrx(layout, src, src_stride, width, height, pixels);
    const bool alpha = has_alpha(layout);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        const auto px = std::span(pixels).subspan(i, 4);
        if (!alpha) {
            px[3] = std::byte{0xff};
            continue;
        }
        const auto a = std::to_integer<unsigned>(px[3]);
        if (!premultiplied || a == 255U) {
            continue;
        }
        if (a == 0U) {
            std::ranges::fill(px, std::byte{0});
            continue;
        }
        px[0] = unpremultiply(px[0], a);
        px[1] = unpremultiply(px[1], a);
        px[2] = unpremultiply(px[2], a);
    }
    return pixels;
}

std::optional<Rect> intersect(const Rect& rect, const Rect& bounds) noexcept
{
    const auto right = [](const Rect& r) { return std::int64_t{r.x} + r.width; };
    const auto bottom = [](const Rect& r) { return std::int64_t{r.y} + r.height; };
    const std::int64_t x0 = std::max(rect.x, bounds.x);
    const std::int64_t y0 = std::max(rect.y, bounds.y);
    const std::int64_t x1 = std::min(right(rect), right(bounds));
    const std::int64_t y1 = std::min(bottom(rect), bottom(bounds));
    if (x1 <= x0 || y1 <= y0) {
        return std::nullopt;
    }
    return Rect{static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0), static_cast<std::int32_t>(x1 - x0),
                static_cast<std::int32_t>(y1 - y0)};
}

void DamageAccumulator::add_full() noexcept
{
    full_ = true;
    rects_.clear();
}

void DamageAccumulator::add(const Rect& rect)
{
    if (full_ || rect.width <= 0 || rect.height <= 0) {
        return;
    }
    const auto contains = [](const Rect& outer, const Rect& inner) {
        return inner.x >= outer.x && inner.y >= outer.y &&
               std::int64_t{inner.x} + inner.width <= std::int64_t{outer.x} + outer.width &&
               std::int64_t{inner.y} + inner.height <= std::int64_t{outer.y} + outer.height;
    };
    if (std::ranges::any_of(rects_, [&](const Rect& r) { return contains(r, rect); })) {
        return;
    }
    std::erase_if(rects_, [&](const Rect& r) { return contains(rect, r); });
    rects_.push_back(rect);
    if (rects_.size() > max_rects) {
        std::int64_t x0 = std::numeric_limits<std::int64_t>::max();
        std::int64_t y0 = x0;
        std::int64_t x1 = std::numeric_limits<std::int64_t>::min();
        std::int64_t y1 = x1;
        for (const Rect& r : rects_) {
            x0 = std::min<std::int64_t>(x0, r.x);
            y0 = std::min<std::int64_t>(y0, r.y);
            x1 = std::max(x1, std::int64_t{r.x} + r.width);
            y1 = std::max(y1, std::int64_t{r.y} + r.height);
        }
        rects_.assign(1, Rect{static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0),
                              static_cast<std::int32_t>(x1 - x0), static_cast<std::int32_t>(y1 - y0)});
    }
}

std::vector<Rect> DamageAccumulator::take()
{
    std::vector<Rect> rects;
    if (!full_) {
        rects = std::move(rects_);
    }
    rects_.clear();
    full_ = false;
    return rects;
}

}  // namespace farland::platform::portal
