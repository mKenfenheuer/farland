// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>
#include <farland/codec/dib.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

namespace farland::codec {

namespace {

constexpr std::uint32_t bi_rgb = 0;
constexpr std::uint32_t bi_bitfields = 3;
constexpr std::uint32_t bi_alphabitfields = 6;
constexpr std::uint32_t info_header_size = 40;
constexpr std::uint32_t v5_header_size = 124;
constexpr std::size_t file_header_size = 14;
constexpr std::uint32_t lcs_srgb = 0x73524742;  // 'sRGB'
constexpr std::uint32_t lcs_gm_images = 4;
constexpr std::int32_t pixels_per_meter_72dpi = 2835;

/// One colour channel of a bit-field pixel.
struct Channel {
    std::uint32_t mask = 0;
    unsigned shift = 0;
    unsigned bits = 0;

    [[nodiscard]] std::uint8_t extract(std::uint32_t pixel, std::uint8_t absent) const noexcept
    {
        if (mask == 0) {
            return absent;
        }
        const std::uint32_t value = (pixel & mask) >> shift;
        if (bits >= 8) {
            return static_cast<std::uint8_t>(value >> (bits - 8));
        }
        return static_cast<std::uint8_t>((value * 255U) / ((1U << bits) - 1U));
    }
};

Result<Channel> make_channel(std::uint32_t mask, unsigned bits_per_pixel, std::size_t offset)
{
    Channel channel{mask, 0, 0};
    if (mask == 0) {
        return channel;
    }
    channel.shift = static_cast<unsigned>(std::countr_zero(mask));
    const std::uint32_t shifted = mask >> channel.shift;
    channel.bits = static_cast<unsigned>(std::popcount(shifted));
    if ((shifted & (shifted + 1U)) != 0 || channel.shift + channel.bits > bits_per_pixel) {
        return fail(Errc::invalid_value, "DIB colour mask is not a contiguous field of the pixel", offset);
    }
    return channel;
}

/// The parsed header: what decoding the pixels needs.
struct DibLayout {
    std::uint32_t header_size = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool top_down = false;
    std::uint16_t bits_per_pixel = 0;
    std::uint32_t compression = 0;
    std::array<std::uint32_t, 4> masks{};  ///< R, G, B, A
    bool alpha_mask = false;
    std::uint32_t colors = 0;       ///< palette entries
    std::size_t colors_offset = 0;  ///< where the colour table starts
    std::size_t pixel_offset = 0;   ///< where the pixels start in a packed DIB
    std::size_t stride = 0;
};

Result<DibLayout> parse_layout(std::span<const std::byte> dib)
{
    Reader r(dib);
    DibLayout layout;
    FARLAND_TRY(layout.header_size, r.u32le());
    if (layout.header_size < info_header_size || layout.header_size > dib.size()) {
        return fail(layout.header_size == 12 ? Errc::unsupported : Errc::invalid_length,
                    "DIB header size is not one of BITMAPINFOHEADER or later", 0);
    }
    FARLAND_TRY(const auto width, r.u32le());
    FARLAND_TRY(const auto height, r.u32le());
    FARLAND_TRY(const auto planes, r.u16le());
    FARLAND_TRY(layout.bits_per_pixel, r.u16le());
    FARLAND_TRY(layout.compression, r.u32le());
    FARLAND_TRY_VOID(r.skip(12));  // biSizeImage, biXPelsPerMeter, biYPelsPerMeter
    FARLAND_TRY(const auto colors_used, r.u32le());
    FARLAND_TRY_VOID(r.skip(4));  // biClrImportant

    const auto signed_width = static_cast<std::int32_t>(width);
    const auto signed_height = static_cast<std::int32_t>(height);
    if (signed_width <= 0 || signed_height == 0 || signed_height == std::numeric_limits<std::int32_t>::min()) {
        return fail(Errc::invalid_value, "DIB with an empty size", 4);
    }
    layout.width = width;
    layout.top_down = signed_height < 0;
    layout.height = static_cast<std::uint32_t>(signed_height < 0 ? -signed_height : signed_height);
    if (layout.width > max_image_dimension || layout.height > max_image_dimension ||
        std::uint64_t{layout.width} * layout.height > max_image_pixels) {
        return fail(Errc::limit_exceeded, "DIB too large", 4);
    }
    if (planes != 1) {
        return fail(Errc::invalid_value, "DIB planes must be 1", 12);
    }
    const auto bpp = layout.bits_per_pixel;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
        return fail(Errc::invalid_value, "DIB bit count is not 1, 4, 8, 16, 24 or 32", 14);
    }

    std::size_t masks_after_header = 0;
    if (layout.compression == bi_bitfields || layout.compression == bi_alphabitfields) {
        if (bpp != 16 && bpp != 32) {
            return fail(Errc::invalid_value, "DIB bit fields need 16 or 32 bits per pixel", 16);
        }
        const std::size_t mask_count = layout.compression == bi_alphabitfields ? 4 : 3;
        std::size_t in_header = 0;
        if (layout.header_size >= 56) {
            in_header = 4;
        } else if (layout.header_size >= 52) {
            in_header = 3;
        }
        Reader masks(dib.subspan(info_header_size), info_header_size);
        if (in_header < mask_count) {
            // BITMAPINFOHEADER: the masks follow the header.
            masks_after_header = mask_count * 4;
            masks = Reader(dib.subspan(layout.header_size), layout.header_size);
        }
        const std::size_t count = std::max(in_header, mask_count);
        for (std::size_t i = 0; i < count && i < 4; ++i) {
            FARLAND_TRY(layout.masks.at(i), masks.u32le());
        }
        layout.alpha_mask = count == 4 && layout.masks[3] != 0;
    } else if (layout.compression == bi_rgb) {
        if (bpp == 16) {
            layout.masks = {0x7C00, 0x03E0, 0x001F, 0};
        } else if (bpp == 32) {
            layout.masks = {0x00FF0000, 0x0000FF00, 0x000000FF, 0};
        }
    } else {
        return fail(Errc::unsupported, "compressed DIB", 16);
    }

    if (bpp <= 8) {
        const std::uint32_t max_colors = 1U << bpp;
        if (colors_used > max_colors) {
            return fail(Errc::invalid_value, "DIB colour table larger than its bit count allows", 32);
        }
        layout.colors = colors_used == 0 ? max_colors : colors_used;
    } else if (colors_used > 256) {
        return fail(Errc::invalid_value, "DIB colour table too large", 32);
    }
    const std::uint32_t table = bpp <= 8 ? layout.colors : colors_used;  // an optional table to skip for > 8 bpp
    layout.colors_offset = layout.header_size + masks_after_header;
    layout.pixel_offset = layout.colors_offset + (std::size_t{table} * 4);
    layout.stride = ((std::size_t{layout.width} * bpp + 31) / 32) * 4;
    return layout;
}

void write_pixel(RgbaImage& image, std::size_t index, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
{
    image.pixels[(index * 4) + 0] = r;
    image.pixels[(index * 4) + 1] = g;
    image.pixels[(index * 4) + 2] = b;
    image.pixels[(index * 4) + 3] = a;
}

}  // namespace

bool RgbaImage::opaque() const noexcept
{
    for (std::size_t i = 3; i < pixels.size(); i += 4) {
        if (pixels[i] != 255) {
            return false;
        }
    }
    return true;
}

Result<RgbaImage> decode_dib(std::span<const std::byte> dib)
{
    FARLAND_TRY(const auto layout, parse_layout(dib));
    if (layout.pixel_offset > dib.size() || layout.stride * layout.height > dib.size() - layout.pixel_offset) {
        return fail(Errc::truncated, "DIB pixels end early", dib.size());
    }
    std::vector<std::array<std::uint8_t, 3>> palette(layout.colors);
    for (std::size_t i = 0; i < palette.size(); ++i) {
        const auto entry = dib.subspan(layout.colors_offset + (i * 4), 3);
        palette[i] = {std::to_integer<std::uint8_t>(entry[2]), std::to_integer<std::uint8_t>(entry[1]),
                      std::to_integer<std::uint8_t>(entry[0])};
    }
    std::array<Channel, 4> channels{};
    for (std::size_t i = 0; i < channels.size(); ++i) {
        const std::uint32_t mask = i == 3 && !layout.alpha_mask ? 0 : layout.masks.at(i);
        FARLAND_TRY(channels.at(i), make_channel(mask, layout.bits_per_pixel, 40 + (i * 4)));
    }

    RgbaImage image;
    image.width = layout.width;
    image.height = layout.height;
    image.pixels.resize(std::size_t{layout.width} * layout.height * 4);
    bool any_alpha = false;
    for (std::uint32_t y = 0; y < layout.height; ++y) {
        const std::uint32_t source_row = layout.top_down ? y : layout.height - 1 - y;
        const auto row = dib.subspan(layout.pixel_offset + (source_row * layout.stride), layout.stride);
        for (std::uint32_t x = 0; x < layout.width; ++x) {
            const std::size_t index = (std::size_t{y} * layout.width) + x;
            const auto byte = [&row](std::size_t i) { return std::to_integer<std::uint32_t>(row[i]); };
            switch (layout.bits_per_pixel) {
            case 1:
            case 4:
            case 8: {
                const unsigned bpp = layout.bits_per_pixel;
                const std::size_t bit = std::size_t{x} * bpp;
                const std::uint32_t value = (byte(bit / 8) >> (8U - bpp - (bit % 8))) & ((1U << bpp) - 1U);
                if (value >= palette.size()) {
                    return fail(Errc::invalid_value, "DIB pixel outside its colour table", layout.pixel_offset);
                }
                const auto& color = palette[value];
                write_pixel(image, index, color[0], color[1], color[2], 255);
                break;
            }
            case 24: {
                const std::size_t at = std::size_t{x} * 3;
                write_pixel(image, index, static_cast<std::uint8_t>(byte(at + 2)),
                            static_cast<std::uint8_t>(byte(at + 1)), static_cast<std::uint8_t>(byte(at)), 255);
                break;
            }
            default: {
                const std::size_t at = std::size_t{x} * (layout.bits_per_pixel / 8U);
                std::uint32_t pixel = byte(at) | (byte(at + 1) << 8U);
                if (layout.bits_per_pixel == 32) {
                    pixel |= (byte(at + 2) << 16U) | (byte(at + 3) << 24U);
                }
                const std::uint8_t alpha = channels[3].extract(pixel, 255);
                any_alpha = any_alpha || (channels[3].mask != 0 && alpha != 0);
                write_pixel(image, index, channels[0].extract(pixel, 0), channels[1].extract(pixel, 0),
                            channels[2].extract(pixel, 0), alpha);
                break;
            }
            }
        }
    }
    if (channels[3].mask != 0 && !any_alpha) {
        // An alpha mask with nothing in it: the application did not fill it in.
        for (std::size_t i = 3; i < image.pixels.size(); i += 4) {
            image.pixels[i] = 255;
        }
    }
    return image;
}

std::vector<std::byte> encode_dib(const RgbaImage& image, DibHeader header)
{
    const std::size_t stride = std::size_t{image.width} * 4;
    const std::size_t pixel_bytes = stride * image.height;
    const std::uint32_t header_size = header == DibHeader::v5 ? v5_header_size : info_header_size;
    Writer w(header_size + pixel_bytes);
    w.u32le(header_size);
    w.u32le(image.width);
    w.u32le(image.height);  // positive: bottom-up
    w.u16le(1);
    w.u16le(32);
    w.u32le(header == DibHeader::v5 ? bi_bitfields : bi_rgb);
    w.u32le(static_cast<std::uint32_t>(pixel_bytes));
    w.u32le(static_cast<std::uint32_t>(pixels_per_meter_72dpi));
    w.u32le(static_cast<std::uint32_t>(pixels_per_meter_72dpi));
    w.u32le(0);  // biClrUsed
    w.u32le(0);  // biClrImportant
    if (header == DibHeader::v5) {
        w.u32le(0x00FF0000);
        w.u32le(0x0000FF00);
        w.u32le(0x000000FF);
        w.u32le(0xFF000000);
        w.u32le(lcs_srgb);
        w.zeros(36 + 12);  // endpoints, gamma
        w.u32le(lcs_gm_images);
        w.zeros(12);  // profile data and size, reserved
    }
    for (std::uint32_t y = image.height; y-- > 0;) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
            w.u8(image.pixels[at + 2]);
            w.u8(image.pixels[at + 1]);
            w.u8(image.pixels[at + 0]);
            w.u8(image.pixels[at + 3]);
        }
    }
    return std::move(w).take();
}

Result<std::vector<std::byte>> dib_to_bmp(std::span<const std::byte> dib)
{
    FARLAND_TRY(const auto layout, parse_layout(dib));
    if (layout.pixel_offset > dib.size() || layout.stride * layout.height > dib.size() - layout.pixel_offset) {
        return fail(Errc::truncated, "DIB pixels end early", dib.size());
    }
    const std::size_t size = file_header_size + layout.pixel_offset + (layout.stride * layout.height);
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        return fail(Errc::limit_exceeded, "DIB too large for a BMP file", 0);
    }
    Writer w(size);
    w.u8('B');
    w.u8('M');
    w.u32le(static_cast<std::uint32_t>(size));
    w.u32le(0);  // bfReserved1, bfReserved2
    w.u32le(static_cast<std::uint32_t>(file_header_size + layout.pixel_offset));
    w.bytes(dib.first(size - file_header_size));
    return std::move(w).take();
}

Result<std::vector<std::byte>> bmp_to_dib(std::span<const std::byte> bmp)
{
    Reader r(bmp);
    FARLAND_TRY(const auto magic, r.u16le());
    if (magic != 0x4D42) {
        return fail(Errc::invalid_value, "BMP file does not start with BM", 0);
    }
    FARLAND_TRY_VOID(r.skip(8));  // bfSize, reserved
    FARLAND_TRY(const auto pixels_at, r.u32le());
    const auto dib = bmp.subspan(file_header_size);
    FARLAND_TRY(const auto layout, parse_layout(dib));
    const std::size_t pixel_bytes = layout.stride * layout.height;
    if (pixels_at < file_header_size + layout.pixel_offset || pixels_at > bmp.size() ||
        pixel_bytes > bmp.size() - pixels_at || layout.pixel_offset > dib.size()) {
        return fail(Errc::invalid_length, "BMP pixel offset outside the file", 10);
    }
    std::vector<std::byte> out(dib.begin(), dib.begin() + static_cast<std::ptrdiff_t>(layout.pixel_offset));
    const auto pixels = bmp.subspan(pixels_at, pixel_bytes);
    out.insert(out.end(), pixels.begin(), pixels.end());
    return out;
}

}  // namespace farland::codec
