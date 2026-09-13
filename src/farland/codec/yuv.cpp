// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// BGRX <-> I420 for the H.264 codecs, [MS-RDPEGFX] 3.3.8.3.1.
//
// Written independently; the coefficients and the 2x2 chroma averaging match
// FreeRDP's libfreerdp/primitives/prim_YUV.c (general_RGBToYUV420_BGRX) and
// prim_internal.h (Apache-2.0), which the unit tests use as the reference.

#include <farland/base/assert.hpp>
#include <farland/codec/yuv.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace farland::codec {

namespace {

constexpr std::size_t bytes_per_pixel = 4;
constexpr std::size_t offset_b = 0;
constexpr std::size_t offset_g = 1;
constexpr std::size_t offset_r = 2;

[[nodiscard]] std::uint8_t at(std::span<const std::byte> bytes, std::size_t index)
{
    return std::to_integer<std::uint8_t>(bytes[index]);
}

/// Fills the part of a plane outside the `used_width` x `used_height`
/// top-left area by repeating its last column and row.
void pad_plane(std::span<std::byte> plane, std::size_t stride, std::uint32_t used_width, std::uint32_t used_height,
               std::uint32_t width, std::uint32_t height)
{
    FARLAND_ASSERT(used_width > 0 && used_height > 0 && used_width <= width && used_height <= height);
    if (used_width < width) {
        for (std::uint32_t row = 0; row < used_height; ++row) {
            const auto line = plane.subspan(row * stride, width);
            std::ranges::fill(line.subspan(used_width), line[used_width - 1U]);
        }
    }
    const auto last = plane.subspan((used_height - 1U) * stride, width);
    for (std::uint32_t row = used_height; row < height; ++row) {
        std::ranges::copy(last, plane.subspan(row * stride, width).begin());
    }
}

}  // namespace

Yuv420Frame::Yuv420Frame(std::uint32_t width, std::uint32_t height) : width_(width), height_(height)
{
    FARLAND_ASSERT(width >= 2 && height >= 2 && width <= max_yuv_dimension && height <= max_yuv_dimension);
    FARLAND_ASSERT(width % 2 == 0 && height % 2 == 0);
    data_.resize(luma_size() + (2 * chroma_size()));
}

std::span<std::byte> Yuv420Frame::y() noexcept
{
    return std::span(data_).first(luma_size());
}

std::span<std::byte> Yuv420Frame::u() noexcept
{
    return std::span(data_).subspan(luma_size(), chroma_size());
}

std::span<std::byte> Yuv420Frame::v() noexcept
{
    return std::span(data_).subspan(luma_size() + chroma_size(), chroma_size());
}

Yuv420View Yuv420Frame::view() const noexcept
{
    const std::span<const std::byte> all(data_);
    return Yuv420View{
        .y = all.first(luma_size()),
        .u = all.subspan(luma_size(), chroma_size()),
        .v = all.subspan(luma_size() + chroma_size(), chroma_size()),
        .width = width_,
        .height = height_,
        .y_stride = y_stride(),
        .uv_stride = uv_stride(),
    };
}

void bgrx_to_yuv420(const ImageView& image, Yuv420Frame& out)
{
    FARLAND_ASSERT(image.width > 0 && image.height > 0);
    FARLAND_ASSERT(image.width <= out.width() && image.height <= out.height());
    const std::size_t row_bytes = std::size_t{image.width} * bytes_per_pixel;
    FARLAND_ASSERT(image.stride >= row_bytes);
    FARLAND_ASSERT(image.data.size() >= ((image.height - 1U) * image.stride) + row_bytes);

    const auto y_plane = out.y();
    const auto u_plane = out.u();
    const auto v_plane = out.v();
    const std::size_t y_stride = out.y_stride();
    const std::size_t uv_stride = out.uv_stride();
    // Chroma blocks that cover the image; the last may hang over an odd edge.
    const std::uint32_t blocks_x = (image.width + 1U) / 2U;
    const std::uint32_t blocks_y = (image.height + 1U) / 2U;

    for (std::uint32_t by = 0; by < blocks_y; ++by) {
        const std::uint32_t y0 = 2U * by;
        const std::uint32_t y1 = std::min(y0 + 1U, image.height - 1U);
        const auto src0 = image.data.subspan(y0 * image.stride, row_bytes);
        const auto src1 = image.data.subspan(y1 * image.stride, row_bytes);
        // The output always has the second row: its height is even and >= 2 * blocks_y.
        const std::size_t luma_width = 2U * std::size_t{blocks_x};
        const auto luma0 = y_plane.subspan(y0 * y_stride, luma_width);
        const auto luma1 = y_plane.subspan((y0 + 1U) * y_stride, luma_width);
        const auto u_row = u_plane.subspan(by * uv_stride, blocks_x);
        const auto v_row = v_plane.subspan(by * uv_stride, blocks_x);

        for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
            const std::size_t x0 = 2U * std::size_t{bx};
            const std::size_t x1 = std::min<std::size_t>(x0 + 1U, image.width - 1U);
            const std::size_t p0 = x0 * bytes_per_pixel;
            const std::size_t p1 = x1 * bytes_per_pixel;

            const std::uint8_t b00 = at(src0, p0 + offset_b);
            const std::uint8_t g00 = at(src0, p0 + offset_g);
            const std::uint8_t r00 = at(src0, p0 + offset_r);
            const std::uint8_t b01 = at(src0, p1 + offset_b);
            const std::uint8_t g01 = at(src0, p1 + offset_g);
            const std::uint8_t r01 = at(src0, p1 + offset_r);
            const std::uint8_t b10 = at(src1, p0 + offset_b);
            const std::uint8_t g10 = at(src1, p0 + offset_g);
            const std::uint8_t r10 = at(src1, p0 + offset_r);
            const std::uint8_t b11 = at(src1, p1 + offset_b);
            const std::uint8_t g11 = at(src1, p1 + offset_g);
            const std::uint8_t r11 = at(src1, p1 + offset_r);

            luma0[x0] = std::byte{rgb_to_y(r00, g00, b00)};
            luma0[x0 + 1U] = std::byte{rgb_to_y(r01, g01, b01)};
            luma1[x0] = std::byte{rgb_to_y(r10, g10, b10)};
            luma1[x0 + 1U] = std::byte{rgb_to_y(r11, g11, b11)};

            const auto r = static_cast<std::uint8_t>((r00 + r01 + r10 + r11) >> 2U);
            const auto g = static_cast<std::uint8_t>((g00 + g01 + g10 + g11) >> 2U);
            const auto b = static_cast<std::uint8_t>((b00 + b01 + b10 + b11) >> 2U);
            u_row[bx] = std::byte{rgb_to_u(r, g, b)};
            v_row[bx] = std::byte{rgb_to_v(r, g, b)};
        }
    }

    pad_plane(y_plane, y_stride, 2U * blocks_x, 2U * blocks_y, out.width(), out.height());
    pad_plane(u_plane, uv_stride, blocks_x, blocks_y, out.width() / 2U, out.height() / 2U);
    pad_plane(v_plane, uv_stride, blocks_x, blocks_y, out.width() / 2U, out.height() / 2U);
}

void yuv420_to_bgrx(const Yuv420View& yuv, std::uint32_t width, std::uint32_t height, std::span<std::byte> out)
{
    FARLAND_ASSERT(width > 0 && height > 0 && width <= yuv.width && height <= yuv.height);
    FARLAND_ASSERT(yuv.y_stride >= yuv.width && yuv.uv_stride >= yuv.width / 2U);
    FARLAND_ASSERT(yuv.y.size() >= ((height - 1U) * yuv.y_stride) + width);
    const std::size_t chroma_rows = (height + 1U) / 2U;
    const std::size_t chroma_width = (width + 1U) / 2U;
    FARLAND_ASSERT(yuv.u.size() >= ((chroma_rows - 1U) * yuv.uv_stride) + chroma_width);
    FARLAND_ASSERT(yuv.v.size() >= ((chroma_rows - 1U) * yuv.uv_stride) + chroma_width);
    FARLAND_ASSERT(out.size() == std::size_t{width} * height * bytes_per_pixel);

    for (std::uint32_t row = 0; row < height; ++row) {
        const auto luma = yuv.y.subspan(row * yuv.y_stride, width);
        const auto u_row = yuv.u.subspan((row / 2U) * yuv.uv_stride, chroma_width);
        const auto v_row = yuv.v.subspan((row / 2U) * yuv.uv_stride, chroma_width);
        const auto dst = out.subspan(std::size_t{row} * width * bytes_per_pixel, std::size_t{width} * bytes_per_pixel);
        for (std::size_t x = 0; x < width; ++x) {
            const std::uint8_t y = at(luma, x);
            const std::uint8_t u = at(u_row, x / 2U);
            const std::uint8_t v = at(v_row, x / 2U);
            const std::size_t p = x * bytes_per_pixel;
            dst[p + offset_b] = std::byte{yuv_to_b(y, u, v)};
            dst[p + offset_g] = std::byte{yuv_to_g(y, u, v)};
            dst[p + offset_r] = std::byte{yuv_to_r(y, u, v)};
            dst[p + 3U] = std::byte{0xFF};
        }
    }
}

}  // namespace farland::codec
