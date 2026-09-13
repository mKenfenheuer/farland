// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/base/writer.hpp>
#include <farland/codec/planar.hpp>
#include <farland/proto/bitmap.hpp>
#include <farland/server/frame_encoder.hpp>

#include <algorithm>

namespace farland::server {

namespace {

constexpr std::size_t bytes_per_source_pixel = 4;
constexpr std::size_t update_header_size = 4;  // updateType + numberRectangles

std::span<const std::byte> source_row(const codec::ImageView& image, std::uint32_t y, std::uint32_t x,
                                      std::uint32_t pixels)
{
    return image.data.subspan((static_cast<std::size_t>(y) * image.stride) + (x * bytes_per_source_pixel),
                              pixels * bytes_per_source_pixel);
}

/// Copies a tile into a tight BGRX buffer `padded_width` pixels wide,
/// repeating the last visible column into the padding.
std::vector<std::byte> copy_tile(const codec::ImageView& frame, std::uint32_t x, std::uint32_t y,
                                 std::uint32_t visible_width, std::uint32_t height, std::uint32_t padded_width)
{
    std::vector<std::byte> tile(static_cast<std::size_t>(padded_width) * height * bytes_per_source_pixel);
    const std::size_t stride = static_cast<std::size_t>(padded_width) * bytes_per_source_pixel;
    for (std::uint32_t row = 0; row < height; ++row) {
        const auto src = source_row(frame, y + row, x, visible_width);
        const auto dst = std::span(tile).subspan(row * stride, stride);
        std::ranges::copy(src, dst.begin());
        const auto last = src.last(bytes_per_source_pixel);
        for (std::uint32_t col = visible_width; col < padded_width; ++col) {
            std::ranges::copy(last, dst.subspan(col * bytes_per_source_pixel, bytes_per_source_pixel).begin());
        }
    }
    return tile;
}

/// Raw bitmap data, [MS-RDPBCGR] 2.2.9.1.1.3.1.2.2: bottom-up rows padded to
/// four bytes, BGR(A) at 24/32 bpp, RGB565 at 16 bpp.
std::vector<std::byte> uncompressed(std::span<const std::byte> tile, std::uint32_t width, std::uint32_t height,
                                    std::uint16_t bpp)
{
    const std::size_t pixel_bytes = bpp / 8U;
    const std::size_t row_bytes = ((width * pixel_bytes) + 3U) & ~std::size_t{3};
    Writer out(row_bytes * height);
    for (std::uint32_t row = height; row-- > 0;) {
        const auto src = tile.subspan(static_cast<std::size_t>(row) * width * bytes_per_source_pixel,
                                      static_cast<std::size_t>(width) * bytes_per_source_pixel);
        const std::size_t start = out.size();
        for (std::uint32_t col = 0; col < width; ++col) {
            const auto px = src.subspan(col * bytes_per_source_pixel, bytes_per_source_pixel);
            const auto b = std::to_integer<std::uint32_t>(px[0]);
            const auto g = std::to_integer<std::uint32_t>(px[1]);
            const auto r = std::to_integer<std::uint32_t>(px[2]);
            if (bpp == 32) {
                out.bytes(px.first(3));
                out.u8(0xFF);
            } else if (bpp == 24) {
                out.bytes(px.first(3));
            } else {
                out.u16le(static_cast<std::uint16_t>(((r >> 3U) << 11U) | ((g >> 2U) << 5U) | (b >> 3U)));
            }
        }
        out.zeros(row_bytes - (out.size() - start));
    }
    return std::move(out).take();
}

}  // namespace

FrameEncoder::FrameEncoder(std::uint32_t width, std::uint32_t height, std::uint16_t bits_per_pixel, BitmapCodec codec,
                           bool omit_compression_header)
    : width_(width), height_(height), bpp_(bits_per_pixel), codec_(codec), omit_header_(omit_compression_header),
      tiles_x_((width + tile_size - 1) / tile_size), tiles_y_((height + tile_size - 1) / tile_size),
      previous_(static_cast<std::size_t>(width) * height * bytes_per_source_pixel),
      dirty_(static_cast<std::size_t>(tiles_x_) * tiles_y_, true)
{
    FARLAND_ASSERT(width > 0 && height > 0 && width <= 0xFFFF && height <= 0xFFFF);
    FARLAND_ASSERT(bpp_ == 32 || bpp_ == 24 || bpp_ == 16);
    FARLAND_ASSERT(codec_ != BitmapCodec::planar || bpp_ == 32);
}

void FrameEncoder::invalidate_all()
{
    std::ranges::fill(dirty_, true);
}

void FrameEncoder::invalidate(const proto::Rectangle16& area)
{
    if (area.left > area.right || area.top > area.bottom) {
        return;
    }
    const std::uint32_t x0 = std::min<std::uint32_t>(area.left / tile_size, tiles_x_);
    const std::uint32_t y0 = std::min<std::uint32_t>(area.top / tile_size, tiles_y_);
    const std::uint32_t x1 = std::min<std::uint32_t>((area.right / tile_size) + 1, tiles_x_);
    const std::uint32_t y1 = std::min<std::uint32_t>((area.bottom / tile_size) + 1, tiles_y_);
    for (std::uint32_t ty = y0; ty < y1; ++ty) {
        for (std::uint32_t tx = x0; tx < x1; ++tx) {
            dirty_[(static_cast<std::size_t>(ty) * tiles_x_) + tx] = true;
        }
    }
}

bool FrameEncoder::tile_changed(const codec::ImageView& frame, std::uint32_t tx, std::uint32_t ty) const
{
    const std::uint32_t x = tx * tile_size;
    const std::uint32_t y = ty * tile_size;
    const std::uint32_t w = std::min(tile_size, width_ - x);
    const std::uint32_t h = std::min(tile_size, height_ - y);
    const std::size_t stride = static_cast<std::size_t>(width_) * bytes_per_source_pixel;
    for (std::uint32_t row = 0; row < h; ++row) {
        const auto now = source_row(frame, y + row, x, w);
        const auto before =
            std::span(previous_).subspan(((y + row) * stride) + (x * bytes_per_source_pixel), now.size());
        if (!std::ranges::equal(now, before)) {
            return true;
        }
    }
    return false;
}

void FrameEncoder::remember_tile(const codec::ImageView& frame, std::uint32_t tx, std::uint32_t ty)
{
    const std::uint32_t x = tx * tile_size;
    const std::uint32_t y = ty * tile_size;
    const std::uint32_t w = std::min(tile_size, width_ - x);
    const std::uint32_t h = std::min(tile_size, height_ - y);
    const std::size_t stride = static_cast<std::size_t>(width_) * bytes_per_source_pixel;
    for (std::uint32_t row = 0; row < h; ++row) {
        const auto now = source_row(frame, y + row, x, w);
        std::ranges::copy(now,
                          std::span(previous_).subspan(((y + row) * stride) + (x * bytes_per_source_pixel)).begin());
    }
}

FrameEncoder::EncodedTile FrameEncoder::encode_tile(const codec::ImageView& frame, std::uint32_t tx,
                                                    std::uint32_t ty) const
{
    EncodedTile tile;
    tile.x = tx * tile_size;
    tile.y = ty * tile_size;
    tile.visible_width = std::min(tile_size, width_ - tile.x);
    tile.height = std::min(tile_size, height_ - tile.y);
    // Widths stay a multiple of four: required for raw rows, and the safe
    // choice for every client's planar decoder.
    tile.padded_width = (tile.visible_width + 3U) & ~3U;
    const auto pixels = copy_tile(frame, tile.x, tile.y, tile.visible_width, tile.height, tile.padded_width);
    if (codec_ == BitmapCodec::planar) {
        const codec::ImageView view{pixels, tile.padded_width, tile.height,
                                    static_cast<std::size_t>(tile.padded_width) * bytes_per_source_pixel};
        tile.data =
            codec::planar::encode(view, {codec::planar::Mode::automatic, codec::planar::Orientation::bottom_up});
    } else {
        tile.data = uncompressed(pixels, tile.padded_width, tile.height, bpp_);
    }
    return tile;
}

std::vector<std::vector<std::byte>> FrameEncoder::encode(const codec::ImageView& frame, std::size_t max_update_size)
{
    FARLAND_ASSERT(frame.width == width_ && frame.height == height_);
    std::vector<EncodedTile> tiles;
    for (std::uint32_t ty = 0; ty < tiles_y_; ++ty) {
        for (std::uint32_t tx = 0; tx < tiles_x_; ++tx) {
            const std::size_t index = (static_cast<std::size_t>(ty) * tiles_x_) + tx;
            if (!dirty_[index] && !tile_changed(frame, tx, ty)) {
                continue;
            }
            tiles.push_back(encode_tile(frame, tx, ty));
            remember_tile(frame, tx, ty);
            dirty_[index] = false;
        }
    }

    const std::uint16_t flags =
        codec_ == BitmapCodec::planar
            ? static_cast<std::uint16_t>(proto::bitmap_flags::compression |
                                         (omit_header_ ? proto::bitmap_flags::no_bitmap_compression_hdr : 0U))
            : std::uint16_t{0};
    std::vector<std::vector<std::byte>> updates;
    std::vector<proto::BitmapData> batch;
    std::size_t batch_size = update_header_size;
    const auto flush = [&] {
        if (batch.empty()) {
            return;
        }
        Writer w(batch_size);
        proto::encode_bitmap_update(w, batch);
        updates.push_back(std::move(w).take());
        batch.clear();
        batch_size = update_header_size;
    };
    for (const auto& tile : tiles) {
        const proto::BitmapData rect{
            static_cast<std::uint16_t>(tile.x),
            static_cast<std::uint16_t>(tile.y),
            static_cast<std::uint16_t>(tile.x + tile.visible_width - 1),
            static_cast<std::uint16_t>(tile.y + tile.height - 1),
            static_cast<std::uint16_t>(tile.padded_width),
            static_cast<std::uint16_t>(tile.height),
            bpp_,
            flags,
            tile.data,
        };
        const std::size_t size = proto::encoded_size(rect);
        if (batch_size + size > max_update_size) {
            flush();
        }
        FARLAND_ASSERT(update_header_size + size <= max_update_size);
        batch.push_back(rect);
        batch_size += size;
    }
    flush();
    return updates;
}

}  // namespace farland::server
