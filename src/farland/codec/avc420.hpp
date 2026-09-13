// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/writer.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/// MPEG-4 AVC/H.264 bitmap streams of RDPGFX, carried in the bitmapData of an
/// RDPGFX_WIRE_TO_SURFACE_PDU_1 ([MS-RDPEGFX] 2.2.2.1):
///
/// - RFX_AVC420_BITMAP_STREAM ([MS-RDPEGFX] 2.2.4.4), codecId
///   RDPGFX_CODECID_AVC420 (0x000B): an RFX_AVC420_METABLOCK (region
///   rectangles with a qp and quality for each) and one H.264 access unit in
///   Annex B format.
/// - RFX_AVC444_BITMAP_STREAM and RFX_AVC444V2_BITMAP_STREAM (2.2.4.5,
///   2.2.4.6), codecIds 0x000E and 0x000F: one or two AVC420 streams behind a
///   32-bit avc420EncodedBitstreamInfo. Both versions share this layout and
///   differ only in how the client combines the luma and chroma pictures.
///
/// Building an AVC420 WireToSurface1 (the channel code does this):
/// - The H.264 picture covers the surface rounded up to multiples of 16
///   (coded_size). The client decodes the whole picture and copies only the
///   region rectangles to the surface, so the padding is never shown.
/// - regionRects are in surface coordinates, with exclusive right and bottom
///   edges, and must lie inside the surface: FreeRDP rejects any rectangle
///   that extends past the surface (h264.c areRectsValid), and with it the
///   whole update.
/// - destRect is the bounding rectangle of the region rectangles (2.2.2.1).
/// - The metablock is informational ("SHOULD NOT be used by the client when
///   decoding"): give each rectangle the frame's average QP and
///   quality_from_qp of it.
/// - Every frame the encoder emits must reach the client, even one whose
///   region list is empty, because later frames predict from it.
/// - Keep one encoder per surface. Its first frame is an IDR. Request another
///   IDR after CreateSurface or ResetGraphics, and whenever the client may
///   have lost a frame.
namespace farland::codec::avc {

/// RDPGFX_RECT16 ([MS-RDPEGFX] 2.2.1.2): right and bottom are exclusive.
struct Rect16 {
    std::uint16_t left = 0;
    std::uint16_t top = 0;
    std::uint16_t right = 0;
    std::uint16_t bottom = 0;

    friend bool operator==(const Rect16&, const Rect16&) = default;
};

/// RDPGFX_AVC420_QUANT_QUALITY ([MS-RDPEGFX] 2.2.4.4.2). The reserved r bit
/// of qpVal is written as zero and ignored when parsing.
struct QuantQuality {
    std::uint8_t qp = 0;       ///< H.264 quantization parameter, 0..max_qp.
    bool progressive = false;  ///< p: the region is progressively encoded.
    std::uint8_t quality = 0;  ///< qualityVal, 0..max_quality.

    friend bool operator==(const QuantQuality&, const QuantQuality&) = default;
};

/// One entry of the metablock: a regionRects element and its quantQualityVals element.
struct Region {
    Rect16 rect;
    QuantQuality quant;

    friend bool operator==(const Region&, const Region&) = default;
};

/// A parsed RFX_AVC420_BITMAP_STREAM. `bitstream` points into the parsed input.
struct Avc420Stream {
    std::vector<Region> regions;
    std::span<const std::byte> bitstream;
};

/// qp range for 8-bit High profiles ([ITU-H.264-201201] 7.4.2.1.1, 7.4.3).
inline constexpr std::uint8_t max_qp = 51;
inline constexpr std::uint8_t max_quality = 100;
/// farland's limit on numRegionRects. FreeRDP's encoder emits one rectangle
/// per changed 64x64 tile, 8160 for a whole 7680x4320 surface.
inline constexpr std::uint32_t max_regions = 16384;
/// Largest cbAvc420EncodedBitstream1 (30 bits).
inline constexpr std::size_t max_avc444_first_size = (std::size_t{1} << 30U) - 1U;

/// The side length of the H.264 picture for a surface side: the next
/// multiple of 16 ([MS-RDPEGFX] 2.2.4.4).
[[nodiscard]] constexpr std::uint32_t coded_size(std::uint32_t side) noexcept
{
    return (side + 15U) & ~15U;
}

/// The qualityVal FreeRDP and the earlier servers send for a qp: 100 - qp
/// (FreeRDP h264.c allocate_h264_metablock).
[[nodiscard]] constexpr std::uint8_t quality_from_qp(std::uint8_t qp) noexcept
{
    return static_cast<std::uint8_t>(max_quality - (qp > max_qp ? max_qp : qp));
}

/// Size of an encoded RFX_AVC420_BITMAP_STREAM.
[[nodiscard]] constexpr std::size_t avc420_size(std::size_t regions, std::size_t bitstream) noexcept
{
    return 4U + (10U * regions) + bitstream;
}

/// Appends an RFX_AVC420_BITMAP_STREAM. Asserts at most max_regions
/// regions, non-empty rectangles, qp and quality in range, and a non-empty
/// bitstream.
void write_avc420(Writer& w, std::span<const Region> regions, std::span<const std::byte> bitstream);

[[nodiscard]] std::vector<std::byte> encode_avc420(std::span<const Region> regions,
                                                   std::span<const std::byte> bitstream);

/// Parses an RFX_AVC420_BITMAP_STREAM that fills `data`; the bitstream runs to
/// the end. Rejects truncated input, numRegionRects above max_regions or
/// above what the input holds, empty rectangles (left >= right or top >=
/// bottom), qp above max_qp, qualityVal above max_quality and an empty
/// bitstream. Whether the rectangles fit the surface is for the caller to
/// check. The bitstream itself is not parsed (see h264::split_annex_b).
[[nodiscard]] Result<Avc420Stream> decode_avc420(std::span<const std::byte> data);

/// LC of avc420EncodedBitstreamInfo ([MS-RDPEGFX] 2.2.4.5).
enum class Avc444Layout : std::uint8_t {
    luma_and_chroma = 0,  ///< A YUV420 picture, then a Chroma420 picture.
    luma = 1,             ///< Only a YUV420 picture; its chroma follows later.
    chroma = 2,           ///< Only a Chroma420 picture, for the last YUV420 picture.
};

/// Input to encode_avc444: one AVC420 stream.
struct Avc420Part {
    std::span<const Region> regions;
    std::span<const std::byte> bitstream;
};

/// A parsed RFX_AVC444_BITMAP_STREAM / RFX_AVC444V2_BITMAP_STREAM. `second`
/// is present exactly for Avc444Layout::luma_and_chroma.
struct Avc444Stream {
    Avc444Layout layout = Avc444Layout::luma;
    Avc420Stream first;
    std::optional<Avc420Stream> second;
};

/// Encodes an RFX_AVC444_BITMAP_STREAM or RFX_AVC444V2_BITMAP_STREAM.
/// cbAvc420EncodedBitstream1 is the size of the first AVC420 stream, except
/// for Avc444Layout::chroma, where it is 0 because the spec says "If no
/// YUV420 frame is present, then this field MUST be set to zero". (FreeRDP's
/// server writes the size there too; its client ignores the field unless LC
/// is 0.) Asserts that `second` is given exactly for luma_and_chroma and that
/// the first stream fits in 30 bits.
[[nodiscard]] std::vector<std::byte> encode_avc444(Avc444Layout layout, const Avc420Part& first,
                                                   const std::optional<Avc420Part>& second = std::nullopt);

/// Parses an RFX_AVC444(V2)_BITMAP_STREAM that fills `data`. Rejects LC 3.
/// For LC 0, cbAvc420EncodedBitstream1 delimits the first stream, and the
/// second runs to the end. For LC 1 it must equal the size of the single
/// stream; for LC 2 it may be 0 (the spec) or that size (FreeRDP).
[[nodiscard]] Result<Avc444Stream> decode_avc444(std::span<const std::byte> data);

}  // namespace farland::codec::avc
