// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/server/compositor.hpp>

#include <algorithm>
#include <array>

namespace farland::server {

namespace {

constexpr std::size_t bytes_per_pixel = 4;

/// The source span [first, last) that destination index `i` of `count` covers.
std::pair<std::uint32_t, std::uint32_t> source_span(std::uint32_t i, std::uint32_t count, std::uint32_t source)
{
    const auto first = static_cast<std::uint32_t>((std::uint64_t{i} * source) / count);
    const auto last = static_cast<std::uint32_t>((std::uint64_t{i + 1} * source) / count);
    return {std::min(first, source - 1), std::max(last, first + 1)};
}

}  // namespace

void scale_image(const codec::ImageView& source, std::span<std::byte> destination, std::uint32_t width,
                 std::uint32_t height, std::size_t stride)
{
    FARLAND_ASSERT(source.width > 0 && source.height > 0 && width > 0 && height > 0);
    FARLAND_ASSERT(stride >= std::size_t{width} * bytes_per_pixel);
    FARLAND_ASSERT(destination.size() >= ((height - 1) * stride) + (std::size_t{width} * bytes_per_pixel));
    std::vector<std::pair<std::uint32_t, std::uint32_t>> columns(width);
    for (std::uint32_t x = 0; x < width; ++x) {
        columns[x] = source_span(x, width, source.width);
    }
    std::vector<std::array<std::uint32_t, 3>> sums(width);
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto [top, bottom] = source_span(y, height, source.height);
        std::ranges::fill(sums, std::array<std::uint32_t, 3>{});
        for (std::uint32_t row = top; row < bottom; ++row) {
            const auto line = source.data.subspan(row * source.stride, std::size_t{source.width} * bytes_per_pixel);
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto [left, right] = columns[x];
                auto& sum = sums[x];
                for (std::uint32_t col = left; col < right; ++col) {
                    const std::size_t at = std::size_t{col} * bytes_per_pixel;
                    sum[0] += std::to_integer<std::uint32_t>(line[at]);
                    sum[1] += std::to_integer<std::uint32_t>(line[at + 1]);
                    sum[2] += std::to_integer<std::uint32_t>(line[at + 2]);
                }
            }
        }
        const auto out = destination.subspan(y * stride, std::size_t{width} * bytes_per_pixel);
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto [left, right] = columns[x];
            const std::uint32_t count = (right - left) * (bottom - top);
            const std::size_t at = std::size_t{x} * bytes_per_pixel;
            for (std::size_t c = 0; c < 3; ++c) {
                out[at + c] = static_cast<std::byte>((sums[x].at(c) + (count / 2)) / count);
            }
            out[at + 3] = std::byte{0xFF};
        }
    }
}

codec::ImageView ScaledPicture::scale(const codec::ImageView& source, std::uint32_t width, std::uint32_t height)
{
    if (source.width == width && source.height == height) {
        return source;
    }
    const std::size_t stride = std::size_t{width} * bytes_per_pixel;
    pixels_.resize(stride * height);
    scale_image(source, pixels_, width, height, stride);
    return codec::ImageView{pixels_, width, height, stride};
}

codec::ImageView Compositor::compose(const OutputLayout& layout,
                                     std::span<const std::optional<codec::ImageView>> pictures)
{
    FARLAND_ASSERT(layout.width > 0 && layout.height > 0);
    if (layout.screens.size() == 1 && layout.fills.empty() && layout.screens.front().screen < pictures.size()) {
        const auto& only = layout.screens.front();
        const auto& picture = pictures[only.screen];
        if (!only.scaled() && only.target == PixelRect{0, 0, layout.width, layout.height} && picture &&
            picture->width == only.width && picture->height == only.height) {
            canvas_layout_.reset();
            return *picture;
        }
    }
    const std::size_t stride = std::size_t{layout.width} * bytes_per_pixel;
    if (!canvas_layout_ || *canvas_layout_ != layout) {
        canvas_.assign(stride * layout.height, std::byte{0});
        canvas_layout_ = layout;
    }
    const std::span canvas(canvas_);
    for (const auto& screen : layout.screens) {
        if (screen.screen >= pictures.size()) {
            continue;
        }
        const auto& given = pictures[screen.screen];
        if (!given) {
            continue;
        }
        const auto& picture = *given;
        const auto& t = screen.target;
        const std::size_t offset = (t.y * stride) + (std::size_t{t.x} * bytes_per_pixel);
        const auto area = canvas.subspan(offset, ((t.height - 1) * stride) + (std::size_t{t.width} * bytes_per_pixel));
        if (picture.width == t.width && picture.height == t.height) {
            for (std::uint32_t row = 0; row < t.height; ++row) {
                std::ranges::copy(picture.data.subspan(row * picture.stride, std::size_t{t.width} * bytes_per_pixel),
                                  area.subspan(row * stride).begin());
            }
        } else {
            scale_image(picture, area, t.width, t.height, stride);
        }
    }
    return codec::ImageView{canvas_, layout.width, layout.height, stride};
}

}  // namespace farland::server
