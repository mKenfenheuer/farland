// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/codec/image.hpp>
#include <farland/server/display_layout.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/// Pictures on the CPU for outputs that cannot place or scale them on the
/// client: bitmap updates, which take one desktop picture, and GFX clients
/// without MapSurfaceToScaledOutput, whose surfaces take scaled pictures.
namespace farland::server {

/// Scales `source` to `width` x `height` into `destination` (BGRX at
/// `stride`). Every destination pixel is the average of the source pixels it
/// covers, which keeps text readable when shrinking; enlarging repeats pixels.
void scale_image(const codec::ImageView& source, std::span<std::byte> destination, std::uint32_t width,
                 std::uint32_t height, std::size_t stride);

/// A picture scaled to another size, in a buffer kept between frames.
class ScaledPicture {
public:
    /// `source` at `width` x `height`: `source` itself when it has that size.
    /// Valid until the next call.
    [[nodiscard]] codec::ImageView scale(const codec::ImageView& source, std::uint32_t width, std::uint32_t height);

private:
    std::vector<std::byte> pixels_;
};

/// One desktop picture from several screens.
class Compositor {
public:
    /// The desktop of `layout`: each screen's picture (`pictures[screen]`) at
    /// its target, scaled where that is smaller, and black elsewhere. A
    /// screen whose picture is nullopt keeps what the last call drew there
    /// (black after a layout change). With one unscaled picture covering the
    /// whole desktop, that picture itself. Valid until the next call and as
    /// long as the pictures.
    [[nodiscard]] codec::ImageView compose(const OutputLayout& layout,
                                           std::span<const std::optional<codec::ImageView>> pictures);

private:
    std::vector<std::byte> canvas_;
    /// What the canvas was drawn for; nullopt after a picture passed through.
    std::optional<OutputLayout> canvas_layout_;
};

}  // namespace farland::server
