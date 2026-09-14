// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/// Device-independent bitmaps, the images of the Windows clipboard (CF_DIB,
/// CF_DIBV5; [MS-WMF] 2.2.2.9 DeviceIndependentBitmap, [MS-WMF] 2.2.2.3
/// BitmapInfoHeader, 2.2.2.5 BitmapV4Header, 2.2.2.6 BitmapV5Header), and
/// BMP files (a BITMAPFILEHEADER in front of one).
namespace farland::codec {

/// farland limits on decoded images (DIB and PNG).
inline constexpr std::uint32_t max_image_dimension = 32768;
inline constexpr std::uint64_t max_image_pixels = std::uint64_t{1} << 26U;

/// 8-bit RGBA with straight alpha, top-down rows of width * 4 bytes.
struct RgbaImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;

    /// True when every pixel's alpha is 255.
    [[nodiscard]] bool opaque() const noexcept;
    friend bool operator==(const RgbaImage&, const RgbaImage&) = default;
};

/// Decodes a packed DIB: BITMAPINFOHEADER, a V2-V5 header or BITMAPV5HEADER,
/// then optional masks and colour table, then the pixels. Takes 1, 4 and 8
/// bits per pixel with a palette, 16 and 32 with BI_RGB or bit fields, and
/// 24; compressed bitmaps (RLE, JPEG, PNG) are Errc::unsupported. 32-bit
/// BI_RGB pixels are opaque; bit fields with an alpha mask carry alpha,
/// unless every alpha value is 0 (then opaque).
[[nodiscard]] Result<RgbaImage> decode_dib(std::span<const std::byte> dib);

enum class DibHeader : std::uint8_t {
    info,  ///< BITMAPINFOHEADER, 32-bit BI_RGB (CF_DIB)
    v5,    ///< BITMAPV5HEADER, 32-bit bit fields with alpha, sRGB (CF_DIBV5)
};

/// Encodes a bottom-up 32-bit packed DIB.
[[nodiscard]] std::vector<std::byte> encode_dib(const RgbaImage& image, DibHeader header);

/// A packed DIB as a BMP file: the 14-byte BITMAPFILEHEADER in front.
[[nodiscard]] Result<std::vector<std::byte>> dib_to_bmp(std::span<const std::byte> dib);
/// A BMP file as a packed DIB, honouring a gap before the pixels (bfOffBits).
[[nodiscard]] Result<std::vector<std::byte>> bmp_to_dib(std::span<const std::byte> bmp);

}  // namespace farland::codec
