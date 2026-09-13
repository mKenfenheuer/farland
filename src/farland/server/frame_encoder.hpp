// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/codec/image.hpp>
#include <farland/proto/share.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

/// Turns desktop frames into legacy bitmap updates (M1's first pixels): the
/// frame is split into 64x64 tiles, tiles that changed since the last frame
/// (or were invalidated) are encoded, and the tiles are packed into
/// TS_UPDATE_BITMAP_DATA blobs that fit the negotiated update size. The GFX
/// pipeline replaces this in M3.
namespace farland::server {

enum class BitmapCodec : std::uint8_t {
    planar,        ///< RDP 6.0 planar, 32 bpp only
    uncompressed,  ///< raw bottom-up rows at the session color depth
};

class FrameEncoder {
public:
    static constexpr std::uint32_t tile_size = 64;

    FrameEncoder(std::uint32_t width, std::uint32_t height, std::uint16_t bits_per_pixel, BitmapCodec codec,
                 bool omit_compression_header);

    /// Encodes the tiles that differ from the previous frame. Each returned
    /// blob is one TS_UPDATE_BITMAP_DATA of at most `max_update_size` bytes.
    [[nodiscard]] std::vector<std::vector<std::byte>> encode(const codec::ImageView& frame,
                                                             std::size_t max_update_size);

    /// Forces the tiles covering `area` (inclusive bounds) into the next update.
    void invalidate(const proto::Rectangle16& area);
    void invalidate_all();

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

private:
    struct EncodedTile {
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        std::uint32_t visible_width = 0;
        std::uint32_t height = 0;
        std::uint32_t padded_width = 0;
        std::vector<std::byte> data;
    };

    [[nodiscard]] bool tile_changed(const codec::ImageView& frame, std::uint32_t tx, std::uint32_t ty) const;
    [[nodiscard]] EncodedTile encode_tile(const codec::ImageView& frame, std::uint32_t tx, std::uint32_t ty) const;
    void remember_tile(const codec::ImageView& frame, std::uint32_t tx, std::uint32_t ty);

    std::uint32_t width_;
    std::uint32_t height_;
    std::uint16_t bpp_;
    BitmapCodec codec_;
    bool omit_header_;
    std::uint32_t tiles_x_;
    std::uint32_t tiles_y_;
    std::vector<std::byte> previous_;  ///< Last frame sent, tightly packed BGRX
    std::vector<bool> dirty_;
};

}  // namespace farland::server
