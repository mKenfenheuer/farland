// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/proto/bitmap.hpp>

namespace farland::proto {

namespace {

constexpr std::size_t rect_header_size = 18;
constexpr std::size_t compression_header_size = 8;
constexpr std::size_t max_rectangles = 0xFFFF;

bool has_compression_header(const BitmapData& rect)
{
    return (rect.flags & bitmap_flags::compression) != 0 && (rect.flags & bitmap_flags::no_bitmap_compression_hdr) == 0;
}

}  // namespace

std::size_t encoded_size(const BitmapData& rect)
{
    return rect_header_size + (has_compression_header(rect) ? compression_header_size : 0) + rect.data.size();
}

void encode_bitmap_update(Writer& w, std::span<const BitmapData> rects)
{
    FARLAND_ASSERT(rects.size() <= max_rectangles);
    w.u16le(updatetype_bitmap);
    w.u16le(static_cast<std::uint16_t>(rects.size()));
    for (const auto& rect : rects) {
        const bool header = has_compression_header(rect);
        const std::size_t length = rect.data.size() + (header ? compression_header_size : 0);
        FARLAND_ASSERT(length <= 0xFFFF);
        w.u16le(rect.dest_left);
        w.u16le(rect.dest_top);
        w.u16le(rect.dest_right);
        w.u16le(rect.dest_bottom);
        w.u16le(rect.width);
        w.u16le(rect.height);
        w.u16le(rect.bits_per_pixel);
        w.u16le(rect.flags);
        w.u16le(static_cast<std::uint16_t>(length));
        if (header) {
            // TS_CD_HEADER, [MS-RDPBCGR] 2.2.9.1.1.3.1.2.3.
            const std::size_t scan_width = static_cast<std::size_t>(rect.width) * rect.bits_per_pixel / 8;
            const std::size_t uncompressed = scan_width * rect.height;
            FARLAND_ASSERT(scan_width <= 0xFFFF && uncompressed <= 0xFFFF && rect.data.size() <= 0xFFFF);
            w.u16le(0);  // cbCompFirstRowSize
            w.u16le(static_cast<std::uint16_t>(rect.data.size()));
            w.u16le(static_cast<std::uint16_t>(scan_width));
            w.u16le(static_cast<std::uint16_t>(uncompressed));
        }
        w.bytes(rect.data);
    }
}

Result<std::vector<BitmapData>> decode_bitmap_update(Reader& r)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint16_t update_type, r.u16le());
    if (update_type != updatetype_bitmap) {
        return fail(Errc::invalid_value, "not a bitmap update", start);
    }
    FARLAND_TRY(const std::uint16_t count, r.u16le());
    std::vector<BitmapData> rects;
    rects.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        BitmapData rect;
        FARLAND_TRY(rect.dest_left, r.u16le());
        FARLAND_TRY(rect.dest_top, r.u16le());
        FARLAND_TRY(rect.dest_right, r.u16le());
        FARLAND_TRY(rect.dest_bottom, r.u16le());
        FARLAND_TRY(rect.width, r.u16le());
        FARLAND_TRY(rect.height, r.u16le());
        FARLAND_TRY(rect.bits_per_pixel, r.u16le());
        FARLAND_TRY(rect.flags, r.u16le());
        const std::size_t length_offset = r.offset();
        FARLAND_TRY(const std::uint16_t length, r.u16le());
        if (has_compression_header(rect)) {
            FARLAND_TRY_VOID(r.skip(2));  // cbCompFirstRowSize
            FARLAND_TRY(const std::uint16_t body, r.u16le());
            FARLAND_TRY_VOID(r.skip(4));  // cbScanWidth, cbUncompressedSize
            if (length != body + compression_header_size && length != body) {
                return fail(Errc::invalid_length, "bitmapLength disagrees with cbCompMainBodySize", length_offset);
            }
            FARLAND_TRY(rect.data, r.bytes(body));
        } else {
            FARLAND_TRY(rect.data, r.bytes(length));
        }
        rects.push_back(rect);
    }
    FARLAND_TRY_VOID(r.expect_end("bitmap update"));
    return rects;
}

}  // namespace farland::proto
