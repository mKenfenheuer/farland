// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>
#include <farland/codec/png.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#ifdef FARLAND_HAVE_ZLIB
#include <zlib.h>
#endif

namespace farland::codec {

#ifdef FARLAND_HAVE_ZLIB

namespace {

constexpr std::array<std::uint8_t, 8> signature = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
/// IDAT chunks farland writes; readers take any size.
constexpr std::size_t idat_chunk_size = std::size_t{1} << 20U;
constexpr std::uint32_t max_chunk_length = 0x7FFFFFFF;  // PNG 5.3

// zlib takes unsigned char pointers; std::byte has the same representation.
const Bytef* zbytes(std::span<const std::byte> data)
{
    return reinterpret_cast<const Bytef*>(data.data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}
Bytef* zbytes(std::span<std::byte> data)
{
    return reinterpret_cast<Bytef*>(data.data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

std::uint32_t crc_of(std::span<const std::byte> type, std::span<const std::byte> data)
{
    uLong crc = crc32(0L, nullptr, 0);
    crc = crc32(crc, zbytes(type), static_cast<uInt>(type.size()));
    // zlib restarts the CRC for a null buffer, which an empty span may have.
    if (!data.empty()) {
        // Chunk data is at most 2^31 - 1 bytes (max_chunk_length), which fits uInt.
        crc = crc32(crc, zbytes(data), static_cast<uInt>(data.size()));
    }
    return static_cast<std::uint32_t>(crc);
}

void write_chunk(Writer& w, std::string_view type, std::span<const std::byte> data)
{
    const auto type_bytes = std::as_bytes(std::span(type));
    w.u32be(static_cast<std::uint32_t>(data.size()));
    w.bytes(type_bytes);
    w.bytes(data);
    w.u32be(crc_of(type_bytes, data));
}

std::uint8_t paeth(std::uint8_t a, std::uint8_t b, std::uint8_t c)
{
    const int p = a + b - c;
    const int pa = std::abs(p - a);
    const int pb = std::abs(p - b);
    const int pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return pb <= pc ? b : c;
}

/// Filters one row with filter type `type`; `prior` is the row above (zeros
/// for the first).
void filter_row(std::span<const std::uint8_t> row, std::span<const std::uint8_t> prior, std::size_t bpp,
                std::uint8_t type, std::span<std::uint8_t> out)
{
    for (std::size_t i = 0; i < row.size(); ++i) {
        const std::uint8_t a = i >= bpp ? row[i - bpp] : 0;
        const std::uint8_t b = prior[i];
        const std::uint8_t c = i >= bpp ? prior[i - bpp] : 0;
        std::uint8_t predictor = 0;
        switch (type) {
        case 1:
            predictor = a;
            break;
        case 2:
            predictor = b;
            break;
        case 3:
            predictor = static_cast<std::uint8_t>((a + b) / 2);
            break;
        case 4:
            predictor = paeth(a, b, c);
            break;
        default:
            break;
        }
        out[i] = static_cast<std::uint8_t>(row[i] - predictor);
    }
}

struct Header {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t depth = 0;
    std::uint8_t color_type = 0;
    bool interlaced = false;

    [[nodiscard]] unsigned channels() const
    {
        switch (color_type) {
        case 2:
            return 3;
        case 4:
            return 2;
        case 6:
            return 4;
        default:
            return 1;
        }
    }
    [[nodiscard]] unsigned bits_per_pixel() const { return channels() * depth; }
    [[nodiscard]] std::size_t row_bytes(std::uint32_t pixels) const
    {
        return ((std::size_t{pixels} * bits_per_pixel()) + 7) / 8;
    }
};

Result<Header> parse_header(Reader r)
{
    Header header;
    FARLAND_TRY(header.width, r.u32be());
    FARLAND_TRY(header.height, r.u32be());
    FARLAND_TRY(header.depth, r.u8());
    FARLAND_TRY(header.color_type, r.u8());
    FARLAND_TRY(const auto compression, r.u8());
    FARLAND_TRY(const auto filter, r.u8());
    FARLAND_TRY(const auto interlace, r.u8());
    bool depth_ok = false;
    switch (header.color_type) {
    case 0:
        depth_ok =
            header.depth == 1 || header.depth == 2 || header.depth == 4 || header.depth == 8 || header.depth == 16;
        break;
    case 3:
        depth_ok = header.depth == 1 || header.depth == 2 || header.depth == 4 || header.depth == 8;
        break;
    case 2:
    case 4:
    case 6:
        depth_ok = header.depth == 8 || header.depth == 16;
        break;
    default:
        return fail(Errc::invalid_value, "PNG colour type", r.offset() - 4);
    }
    if (!depth_ok) {
        return fail(Errc::invalid_value, "PNG bit depth not allowed for its colour type", r.offset() - 5);
    }
    if (compression != 0 || filter != 0 || interlace > 1) {
        return fail(Errc::invalid_value, "PNG compression, filter or interlace method", r.offset() - 3);
    }
    header.interlaced = interlace == 1;
    return header;
}

/// One Adam7 pass (or the whole image): origin and spacing.
struct Pass {
    std::uint32_t x0, y0, dx, dy;
};
constexpr std::array<Pass, 7> adam7 = {{
    {0, 0, 8, 8},
    {4, 0, 8, 8},
    {0, 4, 4, 8},
    {2, 0, 4, 4},
    {0, 2, 2, 4},
    {1, 0, 2, 2},
    {0, 1, 1, 2},
}};
constexpr std::array<Pass, 1> progressive = {{{0, 0, 1, 1}}};

struct Chunk {
    std::string type;
    std::span<const std::byte> data;
    std::size_t offset = 0;
};

Result<Chunk> read_chunk(Reader& r)
{
    Chunk chunk;
    chunk.offset = r.offset();
    FARLAND_TRY(const auto length, r.u32be());
    if (length > max_chunk_length) {
        return fail(Errc::invalid_length, "PNG chunk length above 2^31 - 1", chunk.offset);
    }
    FARLAND_TRY(const auto type, r.bytes(4));
    FARLAND_TRY(chunk.data, r.bytes(length));
    FARLAND_TRY(const auto crc, r.u32be());
    if (crc != crc_of(type, chunk.data)) {
        return fail(Errc::invalid_value, "PNG chunk CRC mismatch", chunk.offset);
    }
    chunk.type.resize(4);
    std::ranges::transform(type, chunk.type.begin(), [](std::byte b) { return static_cast<char>(b); });
    return chunk;
}

std::uint32_t pass_extent(std::uint32_t size, std::uint32_t origin, std::uint32_t step)
{
    return size > origin ? (size - origin + step - 1) / step : 0;
}

Result<void> inflate_all(std::span<const std::byte> compressed, std::span<std::byte> out)
{
    z_stream stream{};
    // inflateInit() is a macro with an old-style cast.
    if (inflateInit_(&stream, ZLIB_VERSION, static_cast<int>(sizeof(z_stream))) != Z_OK) {
        return fail(Errc::io, "zlib inflateInit failed", 0);
    }
    stream.next_in = const_cast<Bytef*>(
        zbytes(compressed));  // NOLINT(cppcoreguidelines-pro-type-const-cast): zlib does not write the input
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = zbytes(out);
    stream.avail_out = static_cast<uInt>(out.size());
    const int result = inflate(&stream, Z_FINISH);
    const auto produced = stream.total_out;
    inflateEnd(&stream);
    if (result == Z_STREAM_END && produced == out.size()) {
        return {};
    }
    if (result == Z_BUF_ERROR && stream.avail_out == 0) {
        return fail(Errc::invalid_length, "PNG image data longer than the image", 0);
    }
    if (result == Z_BUF_ERROR || result == Z_STREAM_END) {
        return fail(Errc::truncated, "PNG image data shorter than the image", 0);
    }
    return fail(Errc::invalid_value, "PNG image data is not a valid zlib stream", 0);
}

/// A sample of `depth` bits at sample index `i` of an unfiltered row.
std::uint32_t sample(std::span<const std::uint8_t> row, std::size_t i, unsigned depth)
{
    if (depth == 16) {
        return (std::uint32_t{row[2 * i]} << 8U) | row[(2 * i) + 1];
    }
    if (depth == 8) {
        return row[i];
    }
    const std::size_t bit = i * depth;
    return (std::uint32_t{row[bit / 8]} >> (8U - depth - (bit % 8))) & ((1U << depth) - 1U);
}

std::uint8_t to_8bit(std::uint32_t value, unsigned depth)
{
    switch (depth) {
    case 1:
        return value != 0 ? 255 : 0;
    case 2:
        return static_cast<std::uint8_t>(value * 85);
    case 4:
        return static_cast<std::uint8_t>(value * 17);
    case 16:
        return static_cast<std::uint8_t>(value >> 8U);
    default:
        return static_cast<std::uint8_t>(value);
    }
}

}  // namespace

bool png_supported() noexcept
{
    return true;
}

Result<std::vector<std::byte>> encode_png(const RgbaImage& image)
{
    if (image.width == 0 || image.height == 0 || image.pixels.size() != std::size_t{image.width} * image.height * 4) {
        return fail(Errc::invalid_value, "empty or inconsistent image", 0);
    }
    const bool alpha = !image.opaque();
    const std::size_t bpp = alpha ? 4 : 3;
    const std::size_t row_bytes = std::size_t{image.width} * bpp;

    std::vector<std::uint8_t> raw((row_bytes + 1) * image.height);
    std::vector<std::uint8_t> row(row_bytes);
    std::vector<std::uint8_t> prior(row_bytes);
    std::vector<std::uint8_t> candidate(row_bytes);
    std::vector<std::uint8_t> best(row_bytes);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        const auto source =
            std::span(image.pixels).subspan(std::size_t{y} * image.width * 4, std::size_t{image.width} * 4);
        for (std::uint32_t x = 0; x < image.width; ++x) {
            for (std::size_t c = 0; c < bpp; ++c) {
                row[(x * bpp) + c] = source[(std::size_t{x} * 4) + c];
            }
        }
        // The usual heuristic: the filter with the smallest sum of absolute
        // (signed) differences.
        std::uint64_t best_score = std::numeric_limits<std::uint64_t>::max();
        std::uint8_t best_type = 0;
        for (std::uint8_t type = 0; type <= 4; ++type) {
            filter_row(row, prior, bpp, type, candidate);
            std::uint64_t score = 0;
            for (const std::uint8_t v : candidate) {
                score += v < 128 ? v : 256U - v;
            }
            if (score < best_score) {
                best_score = score;
                best_type = type;
                best.swap(candidate);
            }
        }
        const std::size_t at = std::size_t{y} * (row_bytes + 1);
        raw[at] = best_type;
        std::ranges::copy(best, raw.begin() + static_cast<std::ptrdiff_t>(at + 1));
        prior.swap(row);
    }

    uLongf compressed_size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::byte> compressed(compressed_size);
    if (compress2(zbytes(std::span(compressed)), &compressed_size, zbytes(std::as_bytes(std::span(raw))),
                  static_cast<uLong>(raw.size()), Z_DEFAULT_COMPRESSION) != Z_OK) {
        return fail(Errc::io, "zlib compress2 failed", 0);
    }
    compressed.resize(compressed_size);

    Writer w(compressed.size() + 128);
    w.bytes(std::as_bytes(std::span(signature)));
    Writer ihdr;
    ihdr.u32be(image.width);
    ihdr.u32be(image.height);
    ihdr.u8(8);
    ihdr.u8(alpha ? 6 : 2);
    ihdr.u8(0);
    ihdr.u8(0);
    ihdr.u8(0);
    write_chunk(w, "IHDR", ihdr.view());
    for (std::size_t offset = 0; offset < compressed.size(); offset += idat_chunk_size) {
        write_chunk(w, "IDAT",
                    std::span(compressed).subspan(offset, std::min(idat_chunk_size, compressed.size() - offset)));
    }
    write_chunk(w, "IEND", {});
    return std::move(w).take();
}

Result<RgbaImage> decode_png(std::span<const std::byte> png, std::uint64_t max_pixels)
{
    Reader r(png);
    FARLAND_TRY(const auto magic, r.bytes(signature.size()));
    if (!std::ranges::equal(magic, std::as_bytes(std::span(signature)))) {
        return fail(Errc::invalid_value, "not a PNG file", 0);
    }
    FARLAND_TRY(const auto ihdr, read_chunk(r));
    if (ihdr.type != "IHDR" || ihdr.data.size() != 13) {
        return fail(Errc::invalid_value, "PNG does not start with IHDR", ihdr.offset);
    }
    FARLAND_TRY(const Header header, parse_header(Reader(ihdr.data, ihdr.offset + 8)));
    if (header.width == 0 || header.height == 0 || header.width > max_image_dimension ||
        header.height > max_image_dimension ||
        std::uint64_t{header.width} * header.height > std::min(max_pixels, max_image_pixels)) {
        return fail(Errc::limit_exceeded, "PNG image empty or too large", ihdr.offset + 8);
    }
    std::vector<std::array<std::uint8_t, 4>> palette;
    std::optional<std::array<std::uint32_t, 3>> transparent;  // colour types 0 and 2
    std::vector<std::byte> idat;
    bool idat_done = false;
    for (;;) {
        FARLAND_TRY(const auto chunk, read_chunk(r));
        const auto& data = chunk.data;
        if (chunk.type != "IDAT" && !idat.empty()) {
            idat_done = true;
        }
        if (chunk.type == "IEND") {
            break;
        }
        if (chunk.type == "IHDR") {
            return fail(Errc::invalid_value, "second PNG IHDR", chunk.offset);
        }
        if (chunk.type == "PLTE") {
            const std::size_t max_entries = header.color_type == 3 ? (std::size_t{1} << header.depth) : 256;
            if (data.empty() || data.size() % 3 != 0 || data.size() / 3 > max_entries || !idat.empty() ||
                !palette.empty()) {
                return fail(Errc::invalid_value, "PNG palette of the wrong size or place", chunk.offset);
            }
            for (std::size_t i = 0; i < data.size(); i += 3) {
                palette.push_back({std::to_integer<std::uint8_t>(data[i]), std::to_integer<std::uint8_t>(data[i + 1]),
                                   std::to_integer<std::uint8_t>(data[i + 2]), 255});
            }
        } else if (chunk.type == "tRNS") {
            if (header.color_type == 3) {
                if (data.size() > palette.size()) {
                    return fail(Errc::invalid_value, "PNG tRNS longer than the palette", chunk.offset);
                }
                for (std::size_t i = 0; i < data.size(); ++i) {
                    palette[i][3] = std::to_integer<std::uint8_t>(data[i]);
                }
            } else if ((header.color_type == 0 && data.size() == 2) || (header.color_type == 2 && data.size() == 6)) {
                Reader t(data);
                std::array<std::uint32_t, 3> key{};
                for (std::size_t i = 0; i < data.size() / 2; ++i) {
                    FARLAND_TRY(key.at(i), t.u16be());
                }
                transparent = key;
            }
        } else if (chunk.type == "IDAT") {
            if (idat_done) {
                return fail(Errc::invalid_value, "PNG IDAT chunks are not consecutive", chunk.offset);
            }
            idat.insert(idat.end(), data.begin(), data.end());
        } else if ((static_cast<unsigned char>(chunk.type[0]) & 0x20U) == 0) {
            return fail(Errc::unsupported, "unknown critical PNG chunk", chunk.offset);
        }
    }
    if (header.color_type == 3 && palette.empty()) {
        return fail(Errc::invalid_value, "PNG with colour type 3 but no palette", 0);
    }
    if (idat.size() > std::numeric_limits<uInt>::max()) {
        return fail(Errc::limit_exceeded, "PNG image data too large", 0);
    }

    const auto passes = header.interlaced ? std::span<const Pass>(adam7) : std::span<const Pass>(progressive);
    std::size_t raw_size = 0;
    for (const auto& pass : passes) {
        const auto w = pass_extent(header.width, pass.x0, pass.dx);
        const auto h = pass_extent(header.height, pass.y0, pass.dy);
        if (w != 0 && h != 0) {
            raw_size += (header.row_bytes(w) + 1) * h;
        }
    }
    std::vector<std::byte> raw(raw_size);
    FARLAND_TRY_VOID(inflate_all(idat, raw));

    RgbaImage image;
    image.width = header.width;
    image.height = header.height;
    image.pixels.resize(std::size_t{image.width} * image.height * 4);
    const unsigned depth = header.depth;
    const std::size_t filter_bpp = std::max<std::size_t>(1, header.bits_per_pixel() / 8);
    std::size_t offset = 0;
    for (const auto& pass : passes) {
        const auto w = pass_extent(header.width, pass.x0, pass.dx);
        const auto h = pass_extent(header.height, pass.y0, pass.dy);
        if (w == 0 || h == 0) {
            continue;
        }
        const std::size_t row_bytes = header.row_bytes(w);
        std::vector<std::uint8_t> prior(row_bytes);
        std::vector<std::uint8_t> row(row_bytes);
        for (std::uint32_t y = 0; y < h; ++y) {
            const auto filter = std::to_integer<std::uint8_t>(raw[offset]);
            if (filter > 4) {
                return fail(Errc::invalid_value, "PNG filter type above 4", 0);
            }
            for (std::size_t i = 0; i < row_bytes; ++i) {
                const auto x = std::to_integer<std::uint8_t>(raw[offset + 1 + i]);
                const std::uint8_t a = i >= filter_bpp ? row[i - filter_bpp] : 0;
                const std::uint8_t b = prior[i];
                const std::uint8_t c = i >= filter_bpp ? prior[i - filter_bpp] : 0;
                std::uint8_t predictor = 0;
                switch (filter) {
                case 1:
                    predictor = a;
                    break;
                case 2:
                    predictor = b;
                    break;
                case 3:
                    predictor = static_cast<std::uint8_t>((a + b) / 2);
                    break;
                case 4:
                    predictor = paeth(a, b, c);
                    break;
                default:
                    break;
                }
                row[i] = static_cast<std::uint8_t>(x + predictor);
            }
            offset += row_bytes + 1;
            for (std::uint32_t x = 0; x < w; ++x) {
                const std::size_t at =
                    ((std::size_t{pass.y0 + (y * pass.dy)} * image.width) + pass.x0 + (std::size_t{x} * pass.dx)) * 4;
                std::array<std::uint8_t, 4> rgba{0, 0, 0, 255};
                const std::size_t first = std::size_t{x} * header.channels();
                switch (header.color_type) {
                case 0: {
                    const auto gray = sample(row, first, depth);
                    rgba = {to_8bit(gray, depth), to_8bit(gray, depth), to_8bit(gray, depth),
                            static_cast<std::uint8_t>(transparent && (*transparent)[0] == gray ? 0 : 255)};
                    break;
                }
                case 2: {
                    const std::array<std::uint32_t, 3> rgb{sample(row, first, depth), sample(row, first + 1, depth),
                                                           sample(row, first + 2, depth)};
                    rgba = {to_8bit(rgb[0], depth), to_8bit(rgb[1], depth), to_8bit(rgb[2], depth),
                            static_cast<std::uint8_t>(transparent && *transparent == rgb ? 0 : 255)};
                    break;
                }
                case 3: {
                    const auto index = sample(row, first, depth);
                    if (index >= palette.size()) {
                        return fail(Errc::invalid_value, "PNG pixel outside its palette", 0);
                    }
                    rgba = palette[index];
                    break;
                }
                case 4:
                    rgba = {to_8bit(sample(row, first, depth), depth), to_8bit(sample(row, first, depth), depth),
                            to_8bit(sample(row, first, depth), depth), to_8bit(sample(row, first + 1, depth), depth)};
                    break;
                default:
                    rgba = {to_8bit(sample(row, first, depth), depth), to_8bit(sample(row, first + 1, depth), depth),
                            to_8bit(sample(row, first + 2, depth), depth),
                            to_8bit(sample(row, first + 3, depth), depth)};
                    break;
                }
                std::ranges::copy(rgba, image.pixels.begin() + static_cast<std::ptrdiff_t>(at));
            }
            prior.swap(row);
        }
    }
    return image;
}

#else

bool png_supported() noexcept
{
    return false;
}

Result<std::vector<std::byte>> encode_png(const RgbaImage& /*image*/)
{
    return fail(Errc::unsupported, "built without zlib: no PNG");
}

Result<RgbaImage> decode_png(std::span<const std::byte> /*png*/, std::uint64_t /*max_pixels*/)
{
    return fail(Errc::unsupported, "built without zlib: no PNG");
}

#endif

}  // namespace farland::codec
