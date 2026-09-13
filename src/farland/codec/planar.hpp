// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/codec/image.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/// RDP 6.0 planar bitmap codec: the RDP6_BITMAP_STREAM of [MS-RDPEGDI]
/// 2.2.2.5.1, compressed and decompressed as in [MS-RDPEGDI] 3.1.9.
///
/// The stream carries one byte per pixel per plane. Planes appear in the order
/// Alpha (omitted when the NA flag is set), Red, Green, Blue. Each plane is
/// either raw or RLE; an RLE plane codes the first scanline as values and
/// every later scanline as deltas to the one before it.
///
/// Row order inside the stream depends on the transport. FreeRDP decodes
/// planar data in legacy bitmap updates ([MS-RDPBCGR] 2.2.9.1.1.3.1.2.2,
/// codec selected by 32 bpp with BITMAP_COMPRESSION) as bottom-up (vFlip TRUE
/// in gdi_Bitmap_Decompress), and RDPGFX planar ([MS-RDPEGFX] 2.2.2.1,
/// RDPGFX_CODECID_PLANAR) as top-down.
namespace farland::codec::planar {

/// Row order inside the stream. The images on both sides are always top-down.
enum class Orientation : std::uint8_t { top_down, bottom_up };

/// `automatic` picks RLE unless the raw planes are smaller.
enum class Mode : std::uint8_t { raw, rle, automatic };

struct EncodeOptions {
    Mode mode = Mode::automatic;
    Orientation orientation = Orientation::bottom_up;
};

/// Largest width or height either direction accepts.
inline constexpr std::uint32_t max_dimension = 8192;

/// Encodes without an alpha plane (NA set), without color loss and without
/// chroma subsampling. Raw streams end with the optional pad byte.
/// Asserts width and height in 1..max_dimension and a view that holds them.
[[nodiscard]] std::vector<std::byte> encode(const ImageView& image, const EncodeOptions& options = {});

/// Decodes into `out` as top-down B, G, R, A with stride width * 4 (alpha
/// 0xFF when the stream has no alpha plane). `out` must hold exactly
/// width * height * 4 bytes (asserted once the dimensions are valid).
///
/// Supports raw and RLE planes, with or without an alpha plane. Color loss
/// (CLL != 0) or chroma subsampling is Errc::unsupported. Rejects truncated
/// input, segments that overrun a scanline and trailing bytes; only a raw
/// stream may end with one pad byte. On error, `out` holds partial output.
[[nodiscard]] Result<void> decode(std::span<const std::byte> stream, std::uint32_t width, std::uint32_t height,
                                  Orientation orientation, std::span<std::byte> out);

}  // namespace farland::codec::planar
