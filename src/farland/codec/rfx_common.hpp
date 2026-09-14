// SPDX-FileCopyrightText: 2011 Vic Lee
// SPDX-FileCopyrightText: 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
// SPDX-FileCopyrightText: 2019 Armin Novak <armin.novak@thincast.com>
// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>
#include <farland/codec/image.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

/// RemoteFX building blocks shared by the RemoteFX Progressive encoder and
/// decoder ([MS-RDPRFX] 3.1.8, [MS-RDPEGFX] 3.1.8.1, 3.2.8.1 and 3.3.8.2):
/// the ICT colour transform, the 5/3 DWT (classic and reduce-extrapolate),
/// scalar quantization, LL3 differential coding, RLGR1/RLGR3 and the SRL/RAW
/// streams of progressive upgrade passes.
///
/// The arithmetic follows FreeRDP 3 (rfx_dwt.c, rfx_quantization.c,
/// rfx_rlgr.c, rfx_differential.h, prim_colors.c and progressive.c, Apache-2.0)
/// bit for bit, including its fixed-point constants and rounding, because the
/// codec is lossy and both ends must agree on every intermediate value.
/// Where FreeRDP relies on unchecked casts, farland wraps modulo 2^16 (the
/// behaviour of a release FreeRDP build) instead of asserting.
///
/// The encoder's hot paths (colour conversion, DWTs, quantization, RLGR
/// encoding) run as kernels over the fixed-size tile arrays
/// (rfx_kernels.cpp): loops the compiler vectorises, and hand-written SSE2,
/// AVX2 and NEON colour conversion. They are chosen at run time (Isa) and
/// every variant gives the same bits as the scalar one.
namespace farland::codec::rfx {

inline constexpr std::size_t tile_size = 64;
inline constexpr std::size_t tile_coefficients = tile_size * tile_size;

/// One colour component of a 64 x 64 tile: pixels, or DWT coefficients in
/// linearized band order ([MS-RDPRFX] 3.1.8.1.6).
using Coefficients = std::array<std::int16_t, tile_coefficients>;

/// Sub-bands in linearization order ([MS-RDPRFX] 3.1.8.1.6).
enum class Band : std::uint8_t { hl1, lh1, hh1, hl2, lh2, hh2, hl3, lh3, hh3, ll3 };
inline constexpr std::size_t band_count = 10;

/// Quantization factor (or progressive extra shift) per sub-band.
struct Quant {
    std::array<std::uint8_t, band_count> bands{};

    [[nodiscard]] constexpr std::uint8_t& operator[](Band band) noexcept { return bands[std::to_underlying(band)]; }
    [[nodiscard]] constexpr std::uint8_t operator[](Band band) const noexcept
    {
        return bands[std::to_underlying(band)];
    }
    friend constexpr bool operator==(const Quant&, const Quant&) = default;
};

/// Builds a Quant from the ten factors in TS_RFX_CODEC_QUANT order ([MS-RDPRFX]
/// 2.2.2.1.5): LL3, LH3, HL3, HH3, LH2, HL2, HH2, LH1, HL1, HH1.
[[nodiscard]] constexpr Quant quant_from_rfx_order(const std::array<std::uint8_t, band_count>& q) noexcept
{
    Quant out;
    out[Band::ll3] = q[0];
    out[Band::lh3] = q[1];
    out[Band::hl3] = q[2];
    out[Band::hh3] = q[3];
    out[Band::lh2] = q[4];
    out[Band::hl2] = q[5];
    out[Band::hh2] = q[6];
    out[Band::lh1] = q[7];
    out[Band::hl1] = q[8];
    out[Band::hh1] = q[9];
    return out;
}

/// The same factor for every band.
[[nodiscard]] constexpr Quant uniform_quant(std::uint8_t value) noexcept
{
    Quant out;
    out.bands.fill(value);
    return out;
}

/// Position and length of each band inside Coefficients.
struct BandLayout {
    std::uint16_t offset = 0;
    std::uint16_t size = 0;
};
using Layout = std::array<BandLayout, band_count>;

/// Classic DWT: 32x32, 16x16 and 8x8 bands ([MS-RDPRFX] 3.1.8.1.4).
inline constexpr Layout standard_layout{{
    {0, 1024},
    {1024, 1024},
    {2048, 1024},
    {3072, 256},
    {3328, 256},
    {3584, 256},
    {3840, 64},
    {3904, 64},
    {3968, 64},
    {4032, 64},
}};

/// Reduce-extrapolate DWT: 31x33, 33x31, 31x31, 16x17, 17x16, 16x16, 8x9, 9x8,
/// 8x8 and a 9x9 LL3 band ([MS-RDPEGFX] 3.2.8.1.2.2).
inline constexpr Layout extrapolate_layout{{
    {0, 1023},
    {1023, 1023},
    {2046, 961},
    {3007, 272},
    {3279, 272},
    {3551, 256},
    {3807, 72},
    {3879, 72},
    {3951, 64},
    {4015, 81},
}};

// ---------------------------------------------------------------------------
// RFX_COMPONENT_CODEC_QUANT ([MS-RDPEGFX] 2.2.4.2.1.5.2): five bytes, low
// nibble first: LL3 HL3 | LH3 HH3 | HL2 LH2 | HH2 HL1 | LH1 HH1.

inline constexpr std::size_t component_quant_size = 5;

[[nodiscard]] Result<Quant> read_component_quant(Reader& r) noexcept;
void write_component_quant(Writer& w, const Quant& q);

// ---------------------------------------------------------------------------
// Colour conversion (ICT), 11.5 fixed point: values are scaled by 32.

/// RGB to YCbCr, [MS-RDPRFX] 3.1.8.1.3, as FreeRDP's
/// general_RGBToYCbCr_16s16s_P3P3. Y is level-shifted by -128 (<< 5); all
/// three results are clamped to [-4096, 4095].
struct Ycbcr {
    std::int16_t y = 0;
    std::int16_t cb = 0;
    std::int16_t cr = 0;
};
[[nodiscard]] constexpr Ycbcr rgb_to_ycbcr(std::int32_t r, std::int32_t g, std::int32_t b) noexcept
{
    const std::int32_t y = ((r * 9798) + (g * 19235) + (b * 3735)) >> 10;
    const std::int32_t cb = ((r * -5535) + (g * -10868) + (b * 16403)) >> 10;
    const std::int32_t cr = ((r * 16377) + (g * -13714) + (b * -2663)) >> 10;
    const auto clamp = [](std::int32_t v) { return static_cast<std::int16_t>(std::clamp(v, -4096, 4095)); };
    return {.y = clamp(y - 4096), .cb = clamp(cb), .cr = clamp(cr)};
}

/// YCbCr to RGB, [MS-RDPRFX] 3.1.8.2.5, as FreeRDP's
/// general_yCbCrToRGB_16s8u_P3AC4R (factors scaled by 2^16). The 32-bit
/// intermediate values wrap exactly as in FreeRDP; that only matters for
/// coefficients far outside the range a real encoder produces.
struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};
[[nodiscard]] constexpr Rgb ycbcr_to_rgb(std::int16_t y, std::int16_t cb, std::int16_t cr) noexcept
{
    const auto wrap = [](std::int64_t v) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(v)); };
    const std::int32_t yy = wrap(static_cast<std::int64_t>(y + 4096) * 65536);
    const std::int32_t cr_r = wrap(cr * std::int64_t{91916});
    const std::int32_t cr_g = wrap(cr * std::int64_t{46819});
    const std::int32_t cb_g = wrap(cb * std::int64_t{22527});
    const std::int32_t cb_b = wrap(cb * std::int64_t{115992});
    const auto channel = [&](std::int64_t sum) {
        const auto v = static_cast<std::int16_t>((wrap(sum) >> 16) >> 5);
        return static_cast<std::uint8_t>(std::clamp<int>(v, 0, 255));
    };
    return {.r = channel(std::int64_t{cr_r} + yy),
            .g = channel(std::int64_t{yy} - cb_g - cr_g),
            .b = channel(std::int64_t{cb_b} + yy)};
}

/// The three components of a tile.
struct Planes {
    Coefficients y{};
    Coefficients cb{};
    Coefficients cr{};
};

/// Reads the 64 x 64 tile at pixel (x0, y0) of `image` and converts it to
/// YCbCr. Pixels beyond the right or bottom edge repeat the last column and
/// row (FreeRDP rfx_encode.c). Asserts that (x0, y0) lies inside the image.
void load_tile(const ImageView& image, std::uint32_t x0, std::uint32_t y0, Planes& out);

/// Converts decoded planes to 64 x 64 B, G, R, A pixels (A = 0xFF), rows of
/// 256 bytes. `out` must hold exactly 16384 bytes (asserted).
void store_tile(const Planes& planes, std::span<std::byte> out);

// ---------------------------------------------------------------------------
// DWT. `scratch` is working memory; its contents are undefined afterwards.

/// Forward three-level 2D DWT, classic boundary handling ([MS-RDPRFX]
/// 3.1.8.1.4, FreeRDP rfx_dwt_2d_encode).
void dwt_encode(Coefficients& data, Coefficients& scratch) noexcept;
/// Inverse of dwt_encode ([MS-RDPRFX] 3.1.8.2.4, FreeRDP rfx_dwt_2d_decode).
void dwt_decode(Coefficients& data, Coefficients& scratch) noexcept;
/// Forward reduce-extrapolate DWT ([MS-RDPEGFX] 3.2.8.1.2.2): the first level
/// extends each line of 64 by a 65th value extrapolated from the last two and
/// gives 33 low and 31 high coefficients, the next levels 17 + 16 and 9 + 8.
/// The result is in extrapolate_layout. FreeRDP has no forward transform;
/// this one is the exact inverse of dwt_decode_extrapolate's lifting steps
/// except for the rounding of the high bands (1/32 of a pixel level per
/// step, at most a fifth of a level after three levels), and saturates
/// instead of wrapping where extrapolated values leave int16.
void dwt_encode_extrapolate(Coefficients& data, Coefficients& scratch) noexcept;
/// Inverse reduce-extrapolate DWT ([MS-RDPEGFX] 3.3.8.2.2, FreeRDP
/// rfx_dwt_2d_extrapolate_decode). Data is in extrapolate_layout.
void dwt_decode_extrapolate(Coefficients& data, Coefficients& scratch) noexcept;

// ---------------------------------------------------------------------------
// Quantization ([MS-RDPRFX] 3.1.8.1.5 and 3.1.8.2.3).

/// FreeRDP rfx_quantization_encode: per band of `layout` a rounding right
/// shift by (quant - 6), then a rounding shift by 5 that removes the 11.5
/// scaling of the colour transform. Asserts every factor in 6..15.
void quantize(Coefficients& data, const Quant& quant, const Layout& layout = standard_layout) noexcept;

/// Left-shifts every band by `shift[band]` (FreeRDP lShiftC_16s_inplace,
/// wrapping modulo 2^16). Returns false, leaving `data` partly shifted, if a
/// shift is 16 or more, which FreeRDP treats as a decoding failure.
[[nodiscard]] bool dequantize(Coefficients& data, const Layout& layout, const Quant& shift) noexcept;

/// LL3 differential coding ([MS-RDPRFX] 3.1.8.1.6). The span is the LL3 band.
void differential_encode(std::span<std::int16_t> band) noexcept;
void differential_decode(std::span<std::int16_t> band) noexcept;

// ---------------------------------------------------------------------------
// RLGR entropy coding ([MS-RDPRFX] 3.1.8.1.7).

enum class RlgrMode : std::uint8_t { rlgr1, rlgr3 };

/// Appends the RLGR encoding of `data` to `out` (FreeRDP rfx_rlgr_encode,
/// [MS-RDPRFX] 3.1.8.1.7.3.2). Like FreeRDP (and therefore every encoder
/// mstsc is known to accept), a trailing zero after a zero run is sent as a
/// terminating value of magnitude 1.
void rlgr_encode(RlgrMode mode, std::span<const std::int16_t> data, std::vector<std::byte>& out);

/// Decodes `src` into exactly `out.size()` values, zero-filling whatever the
/// bitstream does not cover (FreeRDP rfx_rlgr_decode, [MS-RDPRFX]
/// 3.1.8.1.7.3.1). Fails only on empty input, as FreeRDP does.
[[nodiscard]] Result<void> rlgr_decode(RlgrMode mode, std::span<const std::byte> src,
                                       std::span<std::int16_t> out) noexcept;

// ---------------------------------------------------------------------------
// Bit streams.

/// MSB-first bit reader over untrusted bytes with FreeRDP's wBitStream
/// semantics: bits past the end read as zero and the position may run past
/// the end ([MS-RDPEGFX] 3.2.8.1.5.2.1).
class BitReader {
public:
    constexpr BitReader() noexcept = default;
    constexpr explicit BitReader(std::span<const std::byte> data) noexcept : data_(data) {}

    /// The next 32 bits, left-aligned, zero past the end.
    [[nodiscard]] std::uint32_t peek32() const noexcept;
    /// Reads `count` (0..31) bits.
    [[nodiscard]] std::uint32_t read(std::uint32_t count) noexcept;
    void skip(std::size_t count) noexcept { position_ += count; }
    /// Bits left before the end (0 once the position has passed it).
    [[nodiscard]] std::size_t remaining() const noexcept
    {
        const std::size_t length = data_.size() * 8;
        return position_ < length ? length - position_ : 0;
    }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }

private:
    std::span<const std::byte> data_;
    std::size_t position_ = 0;
};

/// MSB-first bit writer (FreeRDP rfx_bitstream.h); the last byte is padded
/// with zero bits.
class BitWriter {
public:
    explicit BitWriter(std::vector<std::byte>& out) noexcept : out_(&out) {}
    /// Writes the low `count` (0..32) bits of `bits`, most significant first.
    void put(std::uint32_t bits, std::uint32_t count);
    /// Writes `count` copies of `bit`.
    void put_repeated(bool bit, std::size_t count);
    void flush();

private:
    std::vector<std::byte>* out_;
    std::uint64_t acc_ = 0;
    std::uint32_t pending_ = 0;
};

// ---------------------------------------------------------------------------
// Upgrade passes ([MS-RDPEGFX] 3.1.8.1.5, 3.3.8.2.1.2; FreeRDP
// progressive_rfx_upgrade_block).

/// Decoder state shared by all bands of one component in an upgrade pass.
struct UpgradeState {
    BitReader srl;
    BitReader raw;
    std::uint32_t kp = 8;
    std::uint32_t nz = 0;
    bool unary = false;
};

/// One value from the SRL stream (FreeRDP progressive_rfx_srl_read).
/// `num_bits` is 1..31.
[[nodiscard]] std::int16_t srl_read(UpgradeState& state, std::uint32_t num_bits) noexcept;

/// Adds one band's upgrade to `current` (FreeRDP progressive_rfx_upgrade_block).
/// Non-LL bands read raw bits for elements whose `sign` is nonzero and SRL
/// otherwise, updating `sign`; the LL band reads raw bits only.
void upgrade_band(UpgradeState& state, std::span<std::int16_t> current, std::span<std::int16_t> sign,
                  std::uint32_t shift, std::uint32_t num_bits, bool non_ll) noexcept;

/// Appends the upgrade of one non-LL band from extra quantization `from` to
/// `to` (from > to, from - to < 16) to `srl_bits` and `raw_bits`. `sb` holds
/// the band's quantized coefficients at full quality: the decoder has
/// trunc(sb / 2^from) * 2^from of each ([MS-RDPEGFX] 3.2.8.1.5.1) and gets
/// the next from - to bits. `kp` is the SRL state, shared by the bands of a
/// component and starting at 8; `zeros` the pending zero run.
void upgrade_encode_band(std::span<const std::int16_t> sb, std::uint32_t from, std::uint32_t to, BitWriter& srl_bits,
                         BitWriter& raw_bits, std::uint32_t& kp, std::uint32_t& zeros);
/// Ends the SRL stream of a component: the pending zero run, then the
/// padding and the extra zero byte of [MS-RDPEGFX] 3.1.8.1.5 (only when the
/// stream has any bits).
void upgrade_encode_finish(BitWriter& srl_bits, std::vector<std::byte>& srl, std::uint32_t& kp, std::uint32_t& zeros);

// ---------------------------------------------------------------------------
// Kernel selection.

/// Instruction sets the kernels can use. `scalar` is plain C++ without
/// explicit vector code (the compiler may still vectorise it for the
/// baseline of the target: SSE2 on x86-64, NEON on aarch64).
enum class Isa : std::uint8_t { scalar, sse2, avx2, neon };

/// The instruction sets this CPU runs, `scalar` first and the best last.
[[nodiscard]] std::span<const Isa> available_isas() noexcept;
/// The instruction set the kernels use: the best available unless
/// set_isa() chose another.
[[nodiscard]] Isa active_isa() noexcept;
/// Uses `isa` (asserted to be available) from now on, for tests and
/// benchmarks. Affects every thread.
void set_isa(Isa isa) noexcept;
[[nodiscard]] const char* isa_name(Isa isa) noexcept;

}  // namespace farland::codec::rfx
