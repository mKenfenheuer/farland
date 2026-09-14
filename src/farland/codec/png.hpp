// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/codec/dib.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/// A small PNG codec (ISO/IEC 15948, W3C PNG 3rd edition) for the clipboard:
/// images copied on the desktop are PNG, the Windows clipboard wants DIBs.
/// The chunk layer, filters and pixel formats are here; deflate is zlib's.
/// Without zlib at build time (the meson dependency is optional) every
/// function fails with Errc::unsupported.
namespace farland::codec {

/// True when this build can encode and decode PNG.
[[nodiscard]] bool png_supported() noexcept;

/// 8-bit RGBA (colour type 6), or RGB (type 2) when the image is opaque;
/// adaptive filtering, default compression.
[[nodiscard]] Result<std::vector<std::byte>> encode_png(const RgbaImage& image);

/// Decodes every standard PNG: colour types 0, 2, 3, 4 and 6 at all their
/// bit depths, tRNS transparency, Adam7 interlacing. 16-bit samples are
/// reduced to 8 bits; ancillary chunks other than tRNS are ignored. Checks
/// every CRC. Images with more than `max_pixels` pixels (or a side above
/// max_image_dimension) are Errc::limit_exceeded.
[[nodiscard]] Result<RgbaImage> decode_png(std::span<const std::byte> png, std::uint64_t max_pixels = max_image_pixels);

}  // namespace farland::codec
