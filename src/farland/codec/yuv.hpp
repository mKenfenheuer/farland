// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/codec/image.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/// YUV 4:2:0 pictures for the MPEG-4 AVC/H.264 codecs of RDPGFX ([MS-RDPEGFX]
/// 2.2.4.4 and 3.3.8.3).
///
/// Colour conversion is full-range BT.709 as [MS-RDPEGFX] 3.3.8.3.1 requires,
/// with the 8-bit integer coefficients of FreeRDP (libfreerdp/primitives/
/// prim_internal.h: RGB2Y/RGB2U/RGB2V for the encoder, YUV2R/YUV2G/YUV2B for
/// the decoder), so the output is bit-identical to FreeRDP's AVC420 encoder
/// and is what its decoder and mstsc invert:
///
///     Y = ( 54 R + 183 G +  18 B) >> 8            (0.2126, 0.7152, 0.0722)
///     U = (-29 R -  99 G + 128 B) >> 8 + 128      (-0.1146, -0.3854, 0.5)
///     V = (128 R - 116 G -  12 B) >> 8 + 128      (0.5, -0.4542, -0.0458)
///
///     R = Y + (403 (V - 128)) >> 8                (1.5748)
///     G = Y - (48 (U - 128) + 120 (V - 128)) >> 8 (0.1873, 0.4681)
///     B = Y + (475 (U - 128)) >> 8                (1.8556)
///
/// There is no 16..235 offset (full range), and the shifts round towards
/// negative infinity. The earlier ZeroVDI bridge used BT.601 limited range,
/// which shifts every colour on these clients.
namespace farland::codec {

/// Largest width or height of a YUV picture (H.264 level 6.2 allows 8192x4320).
inline constexpr std::uint32_t max_yuv_dimension = 8192;

/// Read-only view of an I420 picture: a full-resolution Y plane, then U and V
/// planes subsampled by 2 in both directions. Width and height are even;
/// strides are in bytes.
struct Yuv420View {
    std::span<const std::byte> y;
    std::span<const std::byte> u;
    std::span<const std::byte> v;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t y_stride = 0;
    std::size_t uv_stride = 0;
};

/// An I420 picture with tightly packed planes (strides `width` and
/// `width / 2`), all three in one allocation.
class Yuv420Frame {
public:
    Yuv420Frame() = default;
    /// Asserts even width and height in 2..max_yuv_dimension. Planes start zeroed.
    Yuv420Frame(std::uint32_t width, std::uint32_t height);

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::size_t y_stride() const noexcept { return width_; }
    [[nodiscard]] std::size_t uv_stride() const noexcept { return width_ / 2U; }

    [[nodiscard]] std::span<std::byte> y() noexcept;
    [[nodiscard]] std::span<std::byte> u() noexcept;
    [[nodiscard]] std::span<std::byte> v() noexcept;
    [[nodiscard]] Yuv420View view() const noexcept;

private:
    [[nodiscard]] std::size_t luma_size() const noexcept { return std::size_t{width_} * height_; }
    [[nodiscard]] std::size_t chroma_size() const noexcept { return luma_size() / 4U; }

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<std::byte> data_;
};

// Per-pixel conversion, [MS-RDPEGFX] 3.3.8.3.1 with FreeRDP's coefficients.
// C++20 defines >> on negative values as an arithmetic shift.

[[nodiscard]] constexpr std::uint8_t rgb_to_y(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    return static_cast<std::uint8_t>(((54 * r) + (183 * g) + (18 * b)) >> 8);
}

[[nodiscard]] constexpr std::uint8_t rgb_to_u(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    return static_cast<std::uint8_t>((((-29 * r) - (99 * g) + (128 * b)) >> 8) + 128);
}

[[nodiscard]] constexpr std::uint8_t rgb_to_v(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
{
    return static_cast<std::uint8_t>((((128 * r) - (116 * g) - (12 * b)) >> 8) + 128);
}

namespace detail {
[[nodiscard]] constexpr std::uint8_t clamp_u8(int value) noexcept
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return static_cast<std::uint8_t>(value);
}
}  // namespace detail

[[nodiscard]] constexpr std::uint8_t yuv_to_r(std::uint8_t y, std::uint8_t /*u*/, std::uint8_t v) noexcept
{
    return detail::clamp_u8(((256 * y) + (403 * (v - 128))) >> 8);
}

[[nodiscard]] constexpr std::uint8_t yuv_to_g(std::uint8_t y, std::uint8_t u, std::uint8_t v) noexcept
{
    return detail::clamp_u8(((256 * y) - (48 * (u - 128)) - (120 * (v - 128))) >> 8);
}

[[nodiscard]] constexpr std::uint8_t yuv_to_b(std::uint8_t y, std::uint8_t u, std::uint8_t /*v*/) noexcept
{
    return detail::clamp_u8(((256 * y) + (475 * (u - 128))) >> 8);
}

/// Converts B, G, R, X pixels to I420 into the top-left `image.width` x
/// `image.height` of `out`, and fills the rest of `out` by repeating the last
/// column and row. H.264 surfaces are coded at multiples of 16 and the client
/// crops them with the region rectangles ([MS-RDPEGFX] 2.2.4.4); repeating
/// the edge keeps the padding cheap to code.
///
/// Each chroma sample is computed from the 2x2 average (rounded down) of B, G
/// and R, as FreeRDP does. Odd image sides repeat the last pixel inside the
/// 2x2 block (FreeRDP instead divides a 2-pixel sum by 4 there).
///
/// Asserts a non-empty image that `image.data` holds and that fits in `out`.
void bgrx_to_yuv420(const ImageView& image, Yuv420Frame& out);

/// The reverse transformation ([MS-RDPEGFX] 3.3.8.3.1, FreeRDP's decoder):
/// writes the top-left `width` x `height` of `yuv` as B, G, R, A (alpha 0xFF,
/// stride width * 4) into `out`, with each chroma sample covering its 2x2
/// block. Asserts that `yuv` covers the area and that `out` holds exactly
/// width * height * 4 bytes.
void yuv420_to_bgrx(const Yuv420View& yuv, std::uint32_t width, std::uint32_t height, std::span<std::byte> out);

}  // namespace farland::codec
