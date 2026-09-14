// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/codec/avc420.hpp>
#include <farland/codec/image.hpp>
#include <farland/codec/yuv.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/// YUV 4:4:4 for the AVC444 codecs of RDPGFX ([MS-RDPEGFX] 2.2.4.5, 2.2.4.6
/// and 3.3.8.3): a full-resolution YUV444 picture travels as two YUV420
/// pictures of the same size, the main view (luma, a normal 4:2:0 picture)
/// and the auxiliary view (the chroma samples the main view leaves out).
///
/// Colour conversion is the full-range BT.709 of yuv.hpp, per pixel and
/// without subsampling. Both AVC444 versions share the main view:
///
///     B1  Ymain(x, y) = Y444(x, y)
///     B2  Umain(x, y) = avg U444(2x..2x+1, 2y..2y+1)
///     B3  Vmain(x, y) = avg V444(2x..2x+1, 2y..2y+1)
///
/// The main chroma is the 2x2 average, not the sample at (2x, 2y): that is
/// what FreeRDP's encoder sends (prim_YUV.c general_RGBToAVC444YUV*) and
/// what the "reverse filter" of 3.3.8.3.2 undoes on the client, which
/// recovers U444(2x, 2y) as 4 Umain - U444(2x+1, 2y) - U444(2x, 2y+1) -
/// U444(2x+1, 2y+1). farland rounds the average, FreeRDP truncates it; the
/// client's reverse filter then misses the true sample by at most 2 instead
/// of 3.
///
/// The auxiliary view of AVC444 (v1, 3.3.8.3.2), in 16-line macroblock rows:
///
///     B4  Yaux(x, 16 k + j)     = U444(x, 16 k + 2 j + 1)   j = 0..7
///     B5  Yaux(x, 16 k + 8 + j) = V444(x, 16 k + 2 j + 1)   j = 0..7
///     B6  Uaux(x, y) = U444(2x + 1, 2y)
///     B7  Vaux(x, y) = V444(2x + 1, 2y)
///
/// and of AVC444v2 (3.3.8.3.3), in picture halves and quarters, W the
/// picture width:
///
///     B4  Yaux(x, y)         = U444(2x + 1, y)          x < W/2
///     B5  Yaux(W/2 + x, y)   = V444(2x + 1, y)
///     B6  Uaux(x, y)         = U444(4x, 2y + 1)         x < W/4
///     B7  Uaux(W/4 + x, y)   = V444(4x, 2y + 1)
///     B8  Vaux(x, y)         = U444(4x + 2, 2y + 1)
///     B9  Vaux(W/4 + x, y)   = V444(4x + 2, 2y + 1)
///
/// Which W the v2 halves use is not settled between implementations:
/// FreeRDP's encoder takes the surface width, its decoder the surface width
/// rounded up to 32 (codec/yuv.c yuv444_combine_work_callback), and the coded
/// picture width is the natural reading of the spec. They agree when the
/// surface width is a multiple of 32, so only use v2 then (see
/// server::GraphicsPipeline).
///
/// The functions here work on whole pictures whose sides are multiples of 16,
/// as H.264 codes them; the client model (apply_main_view, apply_aux_view,
/// yuv444_to_bgrx) mirrors FreeRDP's decoder (prim_YUV.c
/// general_YUV420CombineToYUV444 and general_YUV444ToRGB_8u_P3AC4R) and
/// serves the tests as an oracle.
namespace farland::codec {

enum class Avc444Version : std::uint8_t {
    v1,  ///< RDPGFX_CODECID_AVC444 (0x000E), [MS-RDPEGFX] 3.3.8.3.2
    v2,  ///< RDPGFX_CODECID_AVC444V2 (0x000F), [MS-RDPEGFX] 3.3.8.3.3
};

/// Read-only view of a YUV444 picture: three full-resolution planes with a
/// common stride in bytes.
struct Yuv444View {
    std::span<const std::byte> y;
    std::span<const std::byte> u;
    std::span<const std::byte> v;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t stride = 0;
};

/// A YUV444 picture with tightly packed planes (stride `width`).
class Yuv444Frame {
public:
    Yuv444Frame() = default;
    /// Asserts width and height in 1..max_yuv_dimension. Planes start zeroed.
    Yuv444Frame(std::uint32_t width, std::uint32_t height);

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::size_t stride() const noexcept { return width_; }

    [[nodiscard]] std::span<std::byte> y() noexcept;
    [[nodiscard]] std::span<std::byte> u() noexcept;
    [[nodiscard]] std::span<std::byte> v() noexcept;
    [[nodiscard]] Yuv444View view() const noexcept;

private:
    [[nodiscard]] std::size_t plane_size() const noexcept { return std::size_t{width_} * height_; }

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<std::byte> data_;
};

/// Converts B, G, R, X pixels to YUV444 into the top-left `image.width` x
/// `image.height` of `out`, and fills the rest by repeating the last column
/// and row (as bgrx_to_yuv420 does). Asserts a non-empty image that
/// `image.data` holds and that fits in `out`.
void bgrx_to_yuv444(const ImageView& image, Yuv444Frame& out);

/// Splits a YUV444 picture into the main and auxiliary views of `version`.
/// Asserts that all three pictures have the same size, with sides that are
/// multiples of 16.
void split_avc444(const Yuv444View& yuv, Avc444Version version, Yuv420Frame& main, Yuv420Frame& aux);

/// bgrx_to_yuv444 into `scratch`, then split_avc444. `scratch`, `main` and
/// `aux` have the same size.
void bgrx_to_avc444(const ImageView& image, Avc444Version version, Yuv444Frame& scratch, Yuv420Frame& main,
                    Yuv420Frame& aux);

/// Client side of a luma subframe ([MS-RDPEGFX] 3.3.8.3.2: "color conversion
/// MUST be performed as in YUV420p mode using only the data in the main
/// view"): within `rect` (picture coordinates), copies Y and spreads each
/// main-view chroma sample over its 2x2 block of `out`. Asserts that `main`
/// and `out` have the same size, and a non-empty rectangle inside them with
/// even left and top.
void apply_main_view(const Yuv420View& main, const avc::Rect16& rect, Yuv444Frame& out);

/// Client side of a chroma subframe: within `rect`, writes the chroma
/// samples the auxiliary view carries into `out`, leaving U444 and V444 at
/// (2x, 2y) to the main view. Same assertions as apply_main_view.
///
/// FreeRDP's decoder counts the v1 B4/B5 lines from the top of each
/// rectangle, so for it v1 chroma rectangles must start on a multiple of 16;
/// this model counts from the top of the picture and has no such limit.
void apply_aux_view(const Yuv420View& aux, Avc444Version version, const avc::Rect16& rect, Yuv444Frame& out);

/// How yuv444_to_bgrx treats U444 and V444 at (2x, 2y), which after
/// apply_main_view hold the main view's 2x2 average.
enum class ChromaFilter : std::uint8_t {
    none,     ///< Show the average.
    reverse,  ///< Always recover the sample: 4 avg - the other three, clamped.
    /// [MS-RDPEGFX] 3.3.8.3.2 and FreeRDP: recover the sample only if that
    /// changes it by 30 or more, so coding noise is not amplified.
    freerdp,
};

/// Writes the top-left `width` x `height` of `yuv` as B, G, R, A (alpha
/// 0xFF, stride width * 4) into `out`, with yuv_to_r/g/b per pixel. The
/// filter applies to every 2x2 block that lies inside `yuv`. Asserts that
/// `yuv` covers the area and that `out` holds exactly width * height * 4
/// bytes.
void yuv444_to_bgrx(const Yuv444View& yuv, std::uint32_t width, std::uint32_t height, ChromaFilter filter,
                    std::span<std::byte> out);

}  // namespace farland::codec
