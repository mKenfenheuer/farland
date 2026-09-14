// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// YUV444 and its AVC444 split into two YUV420 views, [MS-RDPEGFX] 3.3.8.3.
//
// Written independently from the specification; the layouts and the reverse
// filter were checked against FreeRDP's libfreerdp/primitives/prim_YUV.c
// (general_RGBToAVC444YUV_BGRX, general_RGBToAVC444YUVv2_BGRX,
// general_YUV420CombineToYUV444, general_YUV444ToRGB_8u_P3AC4R, Apache-2.0).
//
// The loops work on whole rows through spans sized before the loop, so the
// compilers can vectorise them.

#include <farland/base/assert.hpp>
#include <farland/codec/yuv444.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

namespace farland::codec {

namespace {

constexpr std::size_t bytes_per_pixel = 4;
/// The client recovers a (2x, 2y) chroma sample only if that changes it by
/// at least this much ([MS-RDPEGFX] 3.3.8.3.2, FreeRDP CONDITIONAL_CLIP).
constexpr int reverse_filter_threshold = 30;

[[nodiscard]] std::uint8_t at(std::span<const std::byte> bytes, std::size_t index)
{
    return std::to_integer<std::uint8_t>(bytes[index]);
}

/// One row of B, G, R, X pixels to Y, U and V samples; `y`, `u` and `v` have
/// one sample per pixel.
void convert_row(std::span<const std::byte> bgrx, std::span<std::byte> y, std::span<std::byte> u,
                 std::span<std::byte> v)
{
    const std::size_t n = y.size();
    FARLAND_ASSERT(bgrx.size() == n * bytes_per_pixel && u.size() == n && v.size() == n);
    for (std::size_t x = 0; x < n; ++x) {
        const std::uint8_t b = at(bgrx, (x * bytes_per_pixel) + 0U);
        const std::uint8_t g = at(bgrx, (x * bytes_per_pixel) + 1U);
        const std::uint8_t r = at(bgrx, (x * bytes_per_pixel) + 2U);
        y[x] = std::byte{rgb_to_y(r, g, b)};
        u[x] = std::byte{rgb_to_u(r, g, b)};
        v[x] = std::byte{rgb_to_v(r, g, b)};
    }
}

/// Fills the part of a plane outside the `used_width` x `used_height`
/// top-left area by repeating its last column and row.
void pad_plane(std::span<std::byte> plane, std::size_t stride, std::uint32_t used_width, std::uint32_t used_height,
               std::uint32_t width, std::uint32_t height)
{
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

/// dst[x] = the rounded average of the 2x2 block at column 2x of rows `a` and `b`.
void average_2x2(std::span<const std::byte> a, std::span<const std::byte> b, std::span<std::byte> dst)
{
    const std::size_t n = dst.size();
    FARLAND_ASSERT(a.size() == 2 * n && b.size() == 2 * n);
    for (std::size_t x = 0; x < n; ++x) {
        const unsigned sum = unsigned{at(a, 2 * x)} + unsigned{at(a, (2 * x) + 1U)} + unsigned{at(b, 2 * x)} +
                             unsigned{at(b, (2 * x) + 1U)};
        dst[x] = std::byte{static_cast<std::uint8_t>((sum + 2U) >> 2U)};
    }
}

/// dst[x] = src[first + step * x].
void gather(std::span<const std::byte> src, std::size_t first, std::size_t step, std::span<std::byte> dst)
{
    const std::size_t n = dst.size();
    FARLAND_ASSERT(n == 0 || first + (step * (n - 1U)) < src.size());
    for (std::size_t x = 0; x < n; ++x) {
        dst[x] = src[first + (step * x)];
    }
}

/// dst[first + step * x] = src[x], for every x that stays inside `dst`.
void scatter(std::span<const std::byte> src, std::span<std::byte> dst, std::size_t first, std::size_t step)
{
    for (std::size_t x = 0, i = first; x < src.size() && i < dst.size(); ++x, i += step) {
        dst[i] = src[x];
    }
}

void check_same_size(const Yuv420View& picture, const Yuv444Frame& out)
{
    FARLAND_ASSERT(picture.width == out.width() && picture.height == out.height());
    FARLAND_ASSERT(picture.y_stride >= picture.width && picture.uv_stride >= picture.width / 2U);
    FARLAND_ASSERT(picture.y.size() >= picture.y_stride * picture.height);
    FARLAND_ASSERT(picture.u.size() >= picture.uv_stride * (picture.height / 2U));
    FARLAND_ASSERT(picture.v.size() >= picture.uv_stride * (picture.height / 2U));
}

void check_rect(const avc::Rect16& rect, const Yuv444Frame& out)
{
    FARLAND_ASSERT(rect.left < rect.right && rect.top < rect.bottom);
    FARLAND_ASSERT(rect.right <= out.width() && rect.bottom <= out.height());
    FARLAND_ASSERT(rect.left % 2 == 0 && rect.top % 2 == 0);
}

/// The B4/B5 line of the v1 auxiliary view that carries line `row` (odd) of
/// U444; the V444 line follows 8 lines further down.
[[nodiscard]] std::size_t v1_aux_line(std::size_t row) noexcept
{
    const std::size_t pair = row / 2U;  // the 2-line pair: 8 of them per 16-line macroblock row
    return (16U * (pair / 8U)) + (pair % 8U);
}

[[nodiscard]] std::uint8_t recover(std::uint8_t average, std::uint8_t a, std::uint8_t b, std::uint8_t c,
                                   ChromaFilter filter)
{
    const int reversed = std::clamp((4 * int{average}) - a - b - c, 0, 255);
    if (filter == ChromaFilter::freerdp && std::abs(reversed - int{average}) < reverse_filter_threshold) {
        return average;
    }
    return static_cast<std::uint8_t>(reversed);
}

}  // namespace

Yuv444Frame::Yuv444Frame(std::uint32_t width, std::uint32_t height) : width_(width), height_(height)
{
    FARLAND_ASSERT(width >= 1 && height >= 1 && width <= max_yuv_dimension && height <= max_yuv_dimension);
    data_.resize(3 * plane_size());
}

std::span<std::byte> Yuv444Frame::y() noexcept
{
    return std::span(data_).first(plane_size());
}

std::span<std::byte> Yuv444Frame::u() noexcept
{
    return std::span(data_).subspan(plane_size(), plane_size());
}

std::span<std::byte> Yuv444Frame::v() noexcept
{
    return std::span(data_).subspan(2 * plane_size(), plane_size());
}

Yuv444View Yuv444Frame::view() const noexcept
{
    const std::span<const std::byte> all(data_);
    return Yuv444View{
        .y = all.first(plane_size()),
        .u = all.subspan(plane_size(), plane_size()),
        .v = all.subspan(2 * plane_size(), plane_size()),
        .width = width_,
        .height = height_,
        .stride = stride(),
    };
}

void bgrx_to_yuv444(const ImageView& image, Yuv444Frame& out)
{
    FARLAND_ASSERT(image.width > 0 && image.height > 0);
    FARLAND_ASSERT(image.width <= out.width() && image.height <= out.height());
    const std::size_t row_bytes = std::size_t{image.width} * bytes_per_pixel;
    FARLAND_ASSERT(image.stride >= row_bytes);
    FARLAND_ASSERT(image.data.size() >= ((image.height - 1U) * image.stride) + row_bytes);

    const std::size_t stride = out.stride();
    const auto y_plane = out.y();
    const auto u_plane = out.u();
    const auto v_plane = out.v();
    for (std::uint32_t row = 0; row < image.height; ++row) {
        convert_row(image.data.subspan(row * image.stride, row_bytes), y_plane.subspan(row * stride, image.width),
                    u_plane.subspan(row * stride, image.width), v_plane.subspan(row * stride, image.width));
    }
    for (const auto plane : {y_plane, u_plane, v_plane}) {
        pad_plane(plane, stride, image.width, image.height, out.width(), out.height());
    }
}

void split_avc444(const Yuv444View& yuv, Avc444Version version, Yuv420Frame& main, Yuv420Frame& aux)
{
    const std::uint32_t width = yuv.width;
    const std::uint32_t height = yuv.height;
    FARLAND_ASSERT(width % 16 == 0 && height % 16 == 0 && width > 0 && height > 0);
    FARLAND_ASSERT(main.width() == width && main.height() == height);
    FARLAND_ASSERT(aux.width() == width && aux.height() == height);
    FARLAND_ASSERT(yuv.stride >= width && yuv.y.size() >= yuv.stride * height);
    FARLAND_ASSERT(yuv.u.size() >= yuv.stride * height && yuv.v.size() >= yuv.stride * height);

    const std::size_t half = width / 2U;
    const std::size_t quarter = width / 4U;
    const auto row_of = [&](std::span<const std::byte> plane, std::size_t row) {
        return plane.subspan(row * yuv.stride, width);
    };
    const auto main_y = main.y();
    const auto main_u = main.u();
    const auto main_v = main.v();
    const auto aux_y = aux.y();
    const auto aux_u = aux.u();
    const auto aux_v = aux.v();

    // B1
    for (std::size_t row = 0; row < height; ++row) {
        std::ranges::copy(row_of(yuv.y, row), main_y.subspan(row * width, width).begin());
    }

    for (std::size_t pair = 0; pair < height / 2U; ++pair) {
        const auto u0 = row_of(yuv.u, 2 * pair);
        const auto u1 = row_of(yuv.u, (2 * pair) + 1U);
        const auto v0 = row_of(yuv.v, 2 * pair);
        const auto v1 = row_of(yuv.v, (2 * pair) + 1U);
        const auto main_u_row = main_u.subspan(pair * half, half);
        const auto main_v_row = main_v.subspan(pair * half, half);
        const auto aux_u_row = aux_u.subspan(pair * half, half);
        const auto aux_v_row = aux_v.subspan(pair * half, half);

        // B2, B3
        average_2x2(u0, u1, main_u_row);
        average_2x2(v0, v1, main_v_row);

        if (version == Avc444Version::v1) {
            // B4, B5: the odd line, U then V 8 lines further down.
            const std::size_t line = v1_aux_line((2 * pair) + 1U);
            std::ranges::copy(u1, aux_y.subspan(line * width, width).begin());
            std::ranges::copy(v1, aux_y.subspan((line + 8U) * width, width).begin());
            // B6, B7: odd columns of the even line.
            gather(u0, 1, 2, aux_u_row);
            gather(v0, 1, 2, aux_v_row);
        } else {
            // B6 to B9: columns 4x and 4x + 2 of the odd line, U in the left
            // quarter and V in the right one.
            gather(u1, 0, 4, aux_u_row.first(quarter));
            gather(v1, 0, 4, aux_u_row.subspan(quarter));
            gather(u1, 2, 4, aux_v_row.first(quarter));
            gather(v1, 2, 4, aux_v_row.subspan(quarter));
        }
    }

    if (version == Avc444Version::v2) {
        // B4, B5: odd columns of every line, U in the left half and V in the right one.
        for (std::size_t row = 0; row < height; ++row) {
            const auto line = aux_y.subspan(row * width, width);
            gather(row_of(yuv.u, row), 1, 2, line.first(half));
            gather(row_of(yuv.v, row), 1, 2, line.subspan(half));
        }
    }
}

void bgrx_to_avc444(const ImageView& image, Avc444Version version, Yuv444Frame& scratch, Yuv420Frame& main,
                    Yuv420Frame& aux)
{
    bgrx_to_yuv444(image, scratch);
    split_avc444(scratch.view(), version, main, aux);
}

void apply_main_view(const Yuv420View& main, const avc::Rect16& rect, Yuv444Frame& out)
{
    check_same_size(main, out);
    check_rect(rect, out);
    const std::size_t stride = out.stride();
    const std::size_t width = std::size_t{rect.right} - rect.left;
    const std::size_t chroma_width = (width + 1U) / 2U;
    const auto y_plane = out.y();
    const auto u_plane = out.u();
    const auto v_plane = out.v();
    for (std::size_t row = rect.top; row < rect.bottom; ++row) {
        const std::size_t offset = (row * stride) + rect.left;
        std::ranges::copy(main.y.subspan((row * main.y_stride) + rect.left, width),
                          y_plane.subspan(offset, width).begin());
        const std::size_t chroma = ((row / 2U) * main.uv_stride) + (rect.left / 2U);
        const auto u_src = main.u.subspan(chroma, chroma_width);
        const auto v_src = main.v.subspan(chroma, chroma_width);
        const auto u_dst = u_plane.subspan(offset, width);
        const auto v_dst = v_plane.subspan(offset, width);
        for (std::size_t x = 0; x < width; ++x) {
            u_dst[x] = u_src[x / 2U];
            v_dst[x] = v_src[x / 2U];
        }
    }
}

void apply_aux_view(const Yuv420View& aux, Avc444Version version, const avc::Rect16& rect, Yuv444Frame& out)
{
    check_same_size(aux, out);
    check_rect(rect, out);
    FARLAND_ASSERT(aux.width % 16 == 0 && aux.height % 16 == 0);
    const std::size_t stride = out.stride();
    const std::size_t left = rect.left;
    const std::size_t width = std::size_t{rect.right} - left;
    const std::size_t half = aux.width / 2U;
    const std::size_t quarter = aux.width / 4U;
    const auto u_plane = out.u();
    const auto v_plane = out.v();

    for (std::size_t row = rect.top; row < rect.bottom; ++row) {
        const auto u_dst = u_plane.subspan((row * stride) + left, width);
        const auto v_dst = v_plane.subspan((row * stride) + left, width);
        const bool odd_line = row % 2U != 0;
        if (version == Avc444Version::v1) {
            if (odd_line) {
                // B4, B5
                const std::size_t line = v1_aux_line(row);
                std::ranges::copy(aux.y.subspan((line * aux.y_stride) + left, width), u_dst.begin());
                std::ranges::copy(aux.y.subspan(((line + 8U) * aux.y_stride) + left, width), v_dst.begin());
            } else {
                // B6, B7 into the odd columns (left is even).
                const std::size_t chroma = ((row / 2U) * aux.uv_stride) + (left / 2U);
                scatter(aux.u.subspan(chroma, width / 2U), u_dst, 1, 2);
                scatter(aux.v.subspan(chroma, width / 2U), v_dst, 1, 2);
            }
            continue;
        }
        // v2, B4 and B5 into the odd columns of every line.
        const auto line = aux.y.subspan(row * aux.y_stride, aux.width);
        scatter(line.subspan(left / 2U, width / 2U), u_dst, 1, 2);
        scatter(line.subspan(half + (left / 2U), width / 2U), v_dst, 1, 2);
        if (odd_line) {
            // B6 to B9 into the even columns: 4x from the U plane, 4x + 2
            // from the V plane. `left` is even but need not be a multiple
            // of 4, so walk the picture columns.
            const auto u_line = aux.u.subspan((row / 2U) * aux.uv_stride, half);
            const auto v_line = aux.v.subspan((row / 2U) * aux.uv_stride, half);
            for (std::size_t x = left; x < rect.right; x += 2U) {
                const auto& source = x % 4U == 0 ? u_line : v_line;
                u_dst[x - left] = source[x / 4U];
                v_dst[x - left] = source[quarter + (x / 4U)];
            }
        }
    }
}

void yuv444_to_bgrx(const Yuv444View& yuv, std::uint32_t width, std::uint32_t height, ChromaFilter filter,
                    std::span<std::byte> out)
{
    FARLAND_ASSERT(width > 0 && height > 0 && width <= yuv.width && height <= yuv.height);
    FARLAND_ASSERT(yuv.stride >= yuv.width);
    const std::size_t planes = yuv.stride * yuv.height;
    FARLAND_ASSERT(yuv.y.size() >= planes && yuv.u.size() >= planes && yuv.v.size() >= planes);
    FARLAND_ASSERT(out.size() == std::size_t{width} * height * bytes_per_pixel);

    for (std::size_t row = 0; row < height; ++row) {
        const std::size_t line = row * yuv.stride;
        const std::size_t below = row + 1U < yuv.height ? line + yuv.stride : line;
        const auto dst = out.subspan(row * width * bytes_per_pixel, std::size_t{width} * bytes_per_pixel);
        for (std::size_t x = 0; x < width; ++x) {
            const std::uint8_t y = at(yuv.y, line + x);
            std::uint8_t u = at(yuv.u, line + x);
            std::uint8_t v = at(yuv.v, line + x);
            if (filter != ChromaFilter::none && row % 2U == 0 && x % 2U == 0 && row + 1U < yuv.height &&
                x + 1U < yuv.width) {
                u = recover(u, at(yuv.u, line + x + 1U), at(yuv.u, below + x), at(yuv.u, below + x + 1U), filter);
                v = recover(v, at(yuv.v, line + x + 1U), at(yuv.v, below + x), at(yuv.v, below + x + 1U), filter);
            }
            const std::size_t p = x * bytes_per_pixel;
            dst[p + 0U] = std::byte{yuv_to_b(y, u, v)};
            dst[p + 1U] = std::byte{yuv_to_g(y, u, v)};
            dst[p + 2U] = std::byte{yuv_to_r(y, u, v)};
            dst[p + 3U] = std::byte{0xFF};
        }
    }
}

}  // namespace farland::codec
