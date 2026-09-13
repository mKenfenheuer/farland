// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include <cstdint>
#include <span>
#include <vector>

/// Bitmap updates (TS_UPDATE_BITMAP_DATA), [MS-RDPBCGR] 2.2.9.1.1.3.1.2. The
/// same structure is the payload of a slow-path Update PDU and of a
/// fast-path bitmap update.
namespace farland::proto {

/// TS_BITMAP_DATA flags.
namespace bitmap_flags {
inline constexpr std::uint16_t compression = 0x0001;
inline constexpr std::uint16_t no_bitmap_compression_hdr = 0x0400;
}  // namespace bitmap_flags

inline constexpr std::uint16_t updatetype_bitmap = 0x0001;

/// TS_BITMAP_DATA. Destination bounds are inclusive; `data` refers to memory
/// owned by the caller. Uncompressed data is bottom-up with rows padded to
/// four bytes; 32 bpp compressed data is a planar stream (bottom-up).
struct BitmapData {
    std::uint16_t dest_left = 0;
    std::uint16_t dest_top = 0;
    std::uint16_t dest_right = 0;
    std::uint16_t dest_bottom = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t bits_per_pixel = 32;
    std::uint16_t flags = 0;
    std::span<const std::byte> data;
};

/// Bytes `encode_bitmap_update` needs for one rectangle.
[[nodiscard]] std::size_t encoded_size(const BitmapData& rect);

/// Encodes TS_UPDATE_BITMAP_DATA. Compressed rectangles without
/// NO_BITMAP_COMPRESSION_HDR get a TS_CD_HEADER; bitmapLength then counts it,
/// as the specification says.
void encode_bitmap_update(Writer& w, std::span<const BitmapData> rects);

/// Decodes TS_UPDATE_BITMAP_DATA. Accepts both bitmapLength conventions for
/// rectangles with a TS_CD_HEADER (with and without the header counted;
/// FreeRDP writes the latter).
[[nodiscard]] Result<std::vector<BitmapData>> decode_bitmap_update(Reader& r);

}  // namespace farland::proto
