// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// RDPGFX H.264 bitmap streams, [MS-RDPEGFX] 2.2.4.4 to 2.2.4.6.
//
// Written from the specification; checked against FreeRDP's
// channels/rdpgfx/client/rdpgfx_codec.c and server/rdpgfx_main.c (Apache-2.0).

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>
#include <farland/codec/avc420.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace farland::codec::avc {

namespace {

constexpr std::size_t region_size = 8 + 2;  // RDPGFX_RECT16 + RDPGFX_AVC420_QUANT_QUALITY

// qpVal ([MS-RDPEGFX] 2.2.4.4.2): qp in bits 0-5, r in bit 6, p in bit 7.
constexpr std::uint8_t qp_mask = 0x3F;
constexpr std::uint8_t progressive_bit = 0x80;

// avc420EncodedBitstreamInfo ([MS-RDPEGFX] 2.2.4.5): cbAvc420EncodedBitstream1
// in bits 0-29, LC in bits 30-31.
constexpr std::uint32_t size_mask = 0x3FFFFFFFU;
constexpr unsigned layout_shift = 30;

void check_region(const Region& region)
{
    FARLAND_ASSERT(region.rect.left < region.rect.right && region.rect.top < region.rect.bottom);
    FARLAND_ASSERT(region.quant.qp <= max_qp && region.quant.quality <= max_quality);
}

/// RFX_AVC420_BITMAP_STREAM filling `r`.
[[nodiscard]] Result<Avc420Stream> read_avc420(Reader r)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint32_t count, r.u32le());
    if (count > max_regions) {
        return fail(Errc::limit_exceeded, "too many AVC420 region rectangles", start);
    }
    if (std::size_t{count} * region_size > r.remaining()) {
        return fail(Errc::invalid_length, "AVC420 metablock is longer than its stream", start);
    }

    Avc420Stream stream;
    stream.regions.resize(count);
    for (Region& region : stream.regions) {
        const std::size_t at = r.offset();
        FARLAND_TRY(region.rect.left, r.u16le());
        FARLAND_TRY(region.rect.top, r.u16le());
        FARLAND_TRY(region.rect.right, r.u16le());
        FARLAND_TRY(region.rect.bottom, r.u16le());
        if (region.rect.left >= region.rect.right || region.rect.top >= region.rect.bottom) {
            return fail(Errc::invalid_value, "empty AVC420 region rectangle", at);
        }
    }
    for (Region& region : stream.regions) {
        const std::size_t at = r.offset();
        FARLAND_TRY(const std::uint8_t qp_val, r.u8());
        FARLAND_TRY(region.quant.quality, r.u8());
        region.quant.qp = static_cast<std::uint8_t>(qp_val & qp_mask);
        region.quant.progressive = (qp_val & progressive_bit) != 0;
        if (region.quant.qp > max_qp) {
            return fail(Errc::invalid_value, "AVC420 qp is above 51", at);
        }
        if (region.quant.quality > max_quality) {
            return fail(Errc::invalid_value, "AVC420 qualityVal is above 100", at + 1);
        }
    }
    stream.bitstream = r.rest();
    if (stream.bitstream.empty()) {
        return fail(Errc::invalid_length, "AVC420 stream has no H.264 bitstream", r.offset());
    }
    return stream;
}

}  // namespace

void write_avc420(Writer& w, std::span<const Region> regions, std::span<const std::byte> bitstream)
{
    FARLAND_ASSERT(regions.size() <= max_regions);
    FARLAND_ASSERT(!bitstream.empty());
    w.u32le(static_cast<std::uint32_t>(regions.size()));
    for (const Region& region : regions) {
        check_region(region);
        w.u16le(region.rect.left);
        w.u16le(region.rect.top);
        w.u16le(region.rect.right);
        w.u16le(region.rect.bottom);
    }
    for (const Region& region : regions) {
        w.u8(static_cast<std::uint8_t>(region.quant.qp | (region.quant.progressive ? progressive_bit : 0U)));
        w.u8(region.quant.quality);
    }
    w.bytes(bitstream);
}

std::vector<std::byte> encode_avc420(std::span<const Region> regions, std::span<const std::byte> bitstream)
{
    Writer w(avc420_size(regions.size(), bitstream.size()));
    write_avc420(w, regions, bitstream);
    return std::move(w).take();
}

Result<Avc420Stream> decode_avc420(std::span<const std::byte> data)
{
    return read_avc420(Reader(data));
}

std::vector<std::byte> encode_avc444(Avc444Layout layout, const Avc420Part& first,
                                     const std::optional<Avc420Part>& second)
{
    FARLAND_ASSERT((layout == Avc444Layout::luma_and_chroma) == second.has_value());
    const std::size_t first_size = avc420_size(first.regions.size(), first.bitstream.size());
    FARLAND_ASSERT(first_size <= max_avc444_first_size);
    const std::size_t second_size =
        second.has_value() ? avc420_size(second->regions.size(), second->bitstream.size()) : 0;

    const std::uint32_t size_field = layout == Avc444Layout::chroma ? 0U : static_cast<std::uint32_t>(first_size);
    Writer w(4 + first_size + second_size);
    w.u32le(size_field | (static_cast<std::uint32_t>(layout) << layout_shift));
    write_avc420(w, first.regions, first.bitstream);
    if (second.has_value()) {
        write_avc420(w, second->regions, second->bitstream);
    }
    return std::move(w).take();
}

Result<Avc444Stream> decode_avc444(std::span<const std::byte> data)
{
    Reader r(data);
    FARLAND_TRY(const std::uint32_t info, r.u32le());
    const std::uint32_t first_size = info & size_mask;
    const std::uint32_t layout = info >> layout_shift;

    Avc444Stream stream;
    switch (layout) {
    case 0: {
        stream.layout = Avc444Layout::luma_and_chroma;
        if (first_size > r.remaining()) {
            return fail(Errc::invalid_length, "cbAvc420EncodedBitstream1 is longer than the AVC444 stream", 0);
        }
        FARLAND_TRY(const Reader first, r.sub(first_size));
        FARLAND_TRY(stream.first, read_avc420(first));
        FARLAND_TRY(stream.second, read_avc420(Reader(r.rest(), r.offset())));
        return stream;
    }
    case 1:
        stream.layout = Avc444Layout::luma;
        if (first_size != r.remaining()) {
            return fail(Errc::invalid_length, "cbAvc420EncodedBitstream1 disagrees with the AVC444 stream", 0);
        }
        break;
    case 2:
        stream.layout = Avc444Layout::chroma;
        if (first_size != 0 && first_size != r.remaining()) {
            return fail(Errc::invalid_length, "cbAvc420EncodedBitstream1 disagrees with the AVC444 stream", 0);
        }
        break;
    default:
        return fail(Errc::invalid_value, "AVC444 LC is 3", 3);
    }
    FARLAND_TRY(stream.first, read_avc420(Reader(r.rest(), r.offset())));
    return stream;
}

}  // namespace farland::codec::avc
