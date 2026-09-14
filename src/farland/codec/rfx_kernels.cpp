// SPDX-FileCopyrightText: 2011 Vic Lee
// SPDX-FileCopyrightText: 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
// SPDX-FileCopyrightText: 2019 Armin Novak <armin.novak@thincast.com>
// SPDX-FileCopyrightText: 2019 Thincast Technologies GmbH
// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// RemoteFX kernels: colour conversion, the forward and inverse DWTs,
// quantization and RLGR encoding, [MS-RDPRFX] 3.1.8 and [MS-RDPEGFX] 3.2.8.1.
//
// Translated from FreeRDP 3 (Apache-2.0): libfreerdp/codec/rfx_dwt.c,
// rfx_quantization.c, rfx_rlgr.c, progressive.c (reduce-extrapolate IDWT) and
// libfreerdp/primitives/prim_colors.c. Modified: the loops are rearranged so
// that the compiler vectorises them (rows instead of columns, even and odd
// lifting steps in separate loops), the RGB to YCbCr conversion has SSE2,
// AVX2 and NEON versions, and the forward reduce-extrapolate DWT is new.
// Every rearrangement keeps FreeRDP's arithmetic; the unit tests compare each
// kernel and instruction set against the straightforward scalar code.
//
// This file uses raw pointers (Clang's safe-buffer check is off below): the
// kernels work on whole Coefficients arrays or on image rows whose bounds
// load_tile() checks first, never on bytes whose length came from the wire.
// The decoder only calls the inverse DWTs from here.

#include <farland/base/assert.hpp>
#include <farland/codec/rfx_common.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#if defined(__x86_64__)
#include <immintrin.h>
#define FARLAND_RFX_X86 1
#elif defined(__aarch64__)
#include <arm_neon.h>
#define FARLAND_RFX_NEON 1
#endif

#if defined(__clang__)
#pragma clang unsafe_buffer_usage begin
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast)
#endif

// Inlined into both the baseline and the AVX2 entry points, so each gets its
// own vectorised copy.
#define FARLAND_KERNEL [[gnu::always_inline]] inline

namespace farland::codec::rfx {

namespace {

using i16 = std::int16_t;
using i32 = std::int32_t;

FARLAND_KERNEL i16 wrap16(i32 v) noexcept
{
    return static_cast<i16>(v);  // modulo 2^16, as in a release FreeRDP build
}

FARLAND_KERNEL i16 sat16(i32 v) noexcept
{
    return static_cast<i16>(std::clamp<i32>(v, INT16_MIN, INT16_MAX));
}

// ---------------------------------------------------------------------------
// RGB to YCbCr, one tile row of 64 B, G, R, X pixels (rgb_to_ycbcr).

constexpr std::size_t row_pixels = tile_size;

void rgb_row_scalar(const std::uint8_t* src, i16* y, i16* cb, i16* cr) noexcept
{
    for (std::size_t i = 0; i < row_pixels; ++i) {
        const Ycbcr c = rgb_to_ycbcr(src[(4 * i) + 2], src[(4 * i) + 1], src[4 * i]);
        y[i] = c.y;
        cb[i] = c.cb;
        cr[i] = c.cr;
    }
}

// Coefficients in pixel byte order B, G, R, X (rgb_to_ycbcr's are R, G, B).
constexpr std::array<i16, 3> coef_y{3735, 19235, 9798};
constexpr std::array<i16, 3> coef_cb{16403, -10868, -5535};
constexpr std::array<i16, 3> coef_cr{-2663, -13714, 16377};

#if defined(FARLAND_RFX_X86)

template <class T>
FARLAND_KERNEL T load(const void* p) noexcept
{
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

template <class T>
FARLAND_KERNEL void store(void* p, const T& v) noexcept
{
    std::memcpy(p, &v, sizeof v);
}

/// B, G, R, 0 coefficients for two pixels (pmaddwd pairs B*b + G*g, R*r + X*0).
FARLAND_KERNEL __m128i sse2_coef(const std::array<i16, 3>& c) noexcept
{
    return _mm_setr_epi16(c[0], c[1], c[2], 0, c[0], c[1], c[2], 0);
}

/// One component of four pixels, given as two pairs of 16-bit B, G, R, X.
FARLAND_KERNEL __m128i sse2_component(__m128i px01, __m128i px23, __m128i coef) noexcept
{
    const __m128 m0 = _mm_castsi128_ps(_mm_madd_epi16(px01, coef));
    const __m128 m1 = _mm_castsi128_ps(_mm_madd_epi16(px23, coef));
    const __m128i even = _mm_castps_si128(_mm_shuffle_ps(m0, m1, _MM_SHUFFLE(2, 0, 2, 0)));
    const __m128i odd = _mm_castps_si128(_mm_shuffle_ps(m0, m1, _MM_SHUFFLE(3, 1, 3, 1)));
    return _mm_srai_epi32(_mm_add_epi32(even, odd), 10);
}

void rgb_row_sse2(const std::uint8_t* src, i16* y, i16* cb, i16* cr) noexcept
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i cy = sse2_coef(coef_y);
    const __m128i ccb = sse2_coef(coef_cb);
    const __m128i ccr = sse2_coef(coef_cr);
    const __m128i level = _mm_set1_epi32(4096);
    const __m128i lo = _mm_set1_epi16(-4096);
    const __m128i hi = _mm_set1_epi16(4095);
    for (std::size_t i = 0; i < row_pixels; i += 8) {
        const auto p0 = load<__m128i>(src + (4 * i));
        const auto p1 = load<__m128i>(src + (4 * i) + 16);
        const __m128i a0 = _mm_unpacklo_epi8(p0, zero);
        const __m128i a1 = _mm_unpackhi_epi8(p0, zero);
        const __m128i a2 = _mm_unpacklo_epi8(p1, zero);
        const __m128i a3 = _mm_unpackhi_epi8(p1, zero);
        const auto out = [&](i16* dst, __m128i coef, __m128i offset) {
            const __m128i v = _mm_packs_epi32(_mm_sub_epi32(sse2_component(a0, a1, coef), offset),
                                              _mm_sub_epi32(sse2_component(a2, a3, coef), offset));
            store(dst + i, _mm_min_epi16(_mm_max_epi16(v, lo), hi));
        };
        out(y, cy, level);
        out(cb, ccb, zero);
        out(cr, ccr, zero);
    }
}

/// B, G, R, 0 coefficients for four pixels.
__attribute__((target("avx2"))) FARLAND_KERNEL __m256i avx2_coef(const std::array<i16, 3>& c) noexcept
{
    return _mm256_setr_epi16(c[0], c[1], c[2], 0, c[0], c[1], c[2], 0, c[0], c[1], c[2], 0, c[0], c[1], c[2], 0);
}

/// One component of 16 pixels (q0..q3: four pixels each as 16-bit B, G, R,
/// X), stored to `dst`.
__attribute__((target("avx2"))) FARLAND_KERNEL void avx2_component(i16* dst, __m256i q0, __m256i q1, __m256i q2,
                                                                   __m256i q3, __m256i coef, __m256i offset) noexcept
{
    // hadd gives pixels 0 1 4 5 | 2 3 6 7 of each group of eight.
    const __m256i a = _mm256_hadd_epi32(_mm256_madd_epi16(q0, coef), _mm256_madd_epi16(q1, coef));
    const __m256i b = _mm256_hadd_epi32(_mm256_madd_epi16(q2, coef), _mm256_madd_epi16(q3, coef));
    const __m256i packed = _mm256_packs_epi32(_mm256_sub_epi32(_mm256_srai_epi32(a, 10), offset),
                                              _mm256_sub_epi32(_mm256_srai_epi32(b, 10), offset));
    // packs leaves pixel pairs in the order 01 45 89 CD | 23 67 AB EF.
    const __m256i v = _mm256_permutevar8x32_epi32(packed, _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7));
    store(dst, _mm256_min_epi16(_mm256_max_epi16(v, _mm256_set1_epi16(-4096)), _mm256_set1_epi16(4095)));
}

__attribute__((target("avx2"))) void rgb_row_avx2(const std::uint8_t* src, i16* y, i16* cb, i16* cr) noexcept
{
    const __m256i cy = avx2_coef(coef_y);
    const __m256i ccb = avx2_coef(coef_cb);
    const __m256i ccr = avx2_coef(coef_cr);
    const __m256i zero = _mm256_setzero_si256();
    const __m256i level = _mm256_set1_epi32(4096);
    for (std::size_t i = 0; i < row_pixels; i += 16) {
        const std::uint8_t* p = src + (4 * i);
        const __m256i q0 = _mm256_cvtepu8_epi16(load<__m128i>(p));
        const __m256i q1 = _mm256_cvtepu8_epi16(load<__m128i>(p + 16));
        const __m256i q2 = _mm256_cvtepu8_epi16(load<__m128i>(p + 32));
        const __m256i q3 = _mm256_cvtepu8_epi16(load<__m128i>(p + 48));
        avx2_component(y + i, q0, q1, q2, q3, cy, level);
        avx2_component(cb + i, q0, q1, q2, q3, ccb, zero);
        avx2_component(cr + i, q0, q1, q2, q3, ccr, zero);
    }
}

#endif

#if defined(FARLAND_RFX_NEON)

void rgb_row_neon(const std::uint8_t* src, i16* y, i16* cb, i16* cr) noexcept
{
    const int16x8_t lo = vdupq_n_s16(-4096);
    const int16x8_t hi = vdupq_n_s16(4095);
    for (std::size_t i = 0; i < row_pixels; i += 16) {
        const uint8x16x4_t px = vld4q_u8(src + (4 * i));  // B, G, R, X planes
        for (std::size_t half = 0; half < 2; ++half) {
            const auto widen = [&](uint8x16_t v) {
                return vreinterpretq_s16_u16(half == 0 ? vmovl_u8(vget_low_u8(v)) : vmovl_high_u8(v));
            };
            const int16x8_t b = widen(px.val[0]);
            const int16x8_t g = widen(px.val[1]);
            const int16x8_t r = widen(px.val[2]);
            const auto out = [&](i16* dst, const std::array<i16, 3>& c, i32 offset) {
                int32x4_t low = vmull_n_s16(vget_low_s16(b), c[0]);
                low = vmlal_n_s16(low, vget_low_s16(g), c[1]);
                low = vmlal_n_s16(low, vget_low_s16(r), c[2]);
                int32x4_t high = vmull_high_n_s16(b, c[0]);
                high = vmlal_high_n_s16(high, g, c[1]);
                high = vmlal_high_n_s16(high, r, c[2]);
                const int32x4_t off = vdupq_n_s32(offset);
                const int16x8_t v = vcombine_s16(vqmovn_s32(vsubq_s32(vshrq_n_s32(low, 10), off)),
                                                 vqmovn_s32(vsubq_s32(vshrq_n_s32(high, 10), off)));
                vst1q_s16(dst + i + (8 * half), vminq_s16(vmaxq_s16(v, lo), hi));
            };
            out(y, coef_y, 4096);
            out(cb, coef_cb, 0);
            out(cr, coef_cr, 0);
        }
    }
}

#endif

// ---------------------------------------------------------------------------
// Classic DWT (FreeRDP rfx_dwt.c)

/// One level of the forward transform on the (2 * sw) x (2 * sw) image at
/// `buf`: bands HL, LH, HH and LL of sw x sw each, back to back.
FARLAND_KERNEL void dwt_encode_block(i16* buf, i16* dwt, std::size_t sw) noexcept
{
    const std::size_t total = sw * 2;
    // Vertical: rows 2n, 2n + 1 and 2n + 2 give L row n (at dwt) and H row n
    // (at dwt + sw rows).
    for (std::size_t n = 0; n < sw; ++n) {
        const i16* __restrict r0 = buf + (2 * n * total);
        const i16* __restrict r1 = r0 + total;
        const i16* __restrict r2 = n + 1 < sw ? r0 + (2 * total) : r0;
        i16* __restrict l = dwt + (n * total);
        i16* __restrict h = dwt + ((sw + n) * total);
        if (n == 0) {
            for (std::size_t x = 0; x < total; ++x) {
                const i16 hx = wrap16((r1[x] - ((r0[x] + r2[x]) >> 1)) >> 1);
                h[x] = hx;
                l[x] = wrap16(r0[x] + hx);
            }
        } else {
            const i16* __restrict hp = h - total;
            for (std::size_t x = 0; x < total; ++x) {
                const i16 hx = wrap16((r1[x] - ((r0[x] + r2[x]) >> 1)) >> 1);
                h[x] = hx;
                l[x] = wrap16(r0[x] + ((hp[x] + hx) >> 1));
            }
        }
    }
    // Horizontal: L gives HL and LL, H gives HH and LH.
    const auto lift = [sw](const i16* __restrict src, i16* __restrict high, i16* __restrict low) {
        for (std::size_t n = 0; n + 1 < sw; ++n) {
            high[n] = wrap16((src[(2 * n) + 1] - ((src[2 * n] + src[(2 * n) + 2]) >> 1)) >> 1);
        }
        const std::size_t last = (2 * sw) - 2;
        high[sw - 1] = wrap16((src[last + 1] - ((src[last] + src[last]) >> 1)) >> 1);
        low[0] = wrap16(src[0] + high[0]);
        for (std::size_t n = 1; n < sw; ++n) {
            low[n] = wrap16(src[2 * n] + ((high[n - 1] + high[n]) >> 1));
        }
    };
    const std::size_t band = sw * sw;
    for (std::size_t y = 0; y < sw; ++y) {
        lift(dwt + (y * total), buf + (y * sw), buf + (3 * band) + (y * sw));
        lift(dwt + ((sw + y) * total), buf + (2 * band) + (y * sw), buf + band + (y * sw));
    }
}

FARLAND_KERNEL void dwt_decode_block(i16* buf, i16* idwt, std::size_t sw) noexcept
{
    const std::size_t total = sw * 2;
    const std::size_t band = sw * sw;
    // Horizontal: LL + HL -> L, LH + HH -> H.
    const auto unlift = [sw](const i16* __restrict low, const i16* __restrict high, i16* __restrict dst) {
        dst[0] = wrap16(low[0] - ((high[0] + high[0] + 1) >> 1));
        for (std::size_t n = 1; n < sw; ++n) {
            dst[2 * n] = wrap16(low[n] - ((high[n - 1] + high[n] + 1) >> 1));
        }
        for (std::size_t n = 0; n + 1 < sw; ++n) {
            dst[(2 * n) + 1] = wrap16((high[n] * 2) + ((dst[2 * n] + dst[(2 * n) + 2]) >> 1));
        }
        dst[(2 * sw) - 1] = wrap16((high[sw - 1] * 2) + dst[(2 * sw) - 2]);
    };
    for (std::size_t y = 0; y < sw; ++y) {
        unlift(buf + (3 * band) + (y * sw), buf + (y * sw), idwt + (y * total));
        unlift(buf + band + (y * sw), buf + (2 * band) + (y * sw), idwt + ((sw + y) * total));
    }
    // Vertical: L + H -> output, row by row.
    const i16* lrows = idwt;
    const i16* hrows = idwt + (sw * total);
    {
        i16* __restrict dst = buf;
        const i16* __restrict l0 = lrows;
        const i16* __restrict h0 = hrows;
        for (std::size_t x = 0; x < total; ++x) {
            dst[x] = wrap16(l0[x] - (((h0[x] * 2) + 1) >> 1));
        }
    }
    for (std::size_t n = 1; n < sw; ++n) {
        const i16* __restrict ln = lrows + (n * total);
        const i16* __restrict hp = hrows + ((n - 1) * total);
        const i16* __restrict hn = hp + total;
        i16* __restrict even = buf + (2 * n * total);
        i16* __restrict odd = even - total;
        const i16* __restrict prev = even - (2 * total);
        for (std::size_t x = 0; x < total; ++x) {
            even[x] = wrap16(ln[x] - ((hp[x] + hn[x] + 1) >> 1));
        }
        for (std::size_t x = 0; x < total; ++x) {
            odd[x] = wrap16((hp[x] * 2) + ((prev[x] + even[x]) >> 1));
        }
    }
    {
        const i16* __restrict hl = hrows + ((sw - 1) * total);
        const i16* __restrict last = buf + ((2 * sw - 2) * total);
        i16* __restrict odd = buf + ((2 * sw - 1) * total);
        for (std::size_t x = 0; x < total; ++x) {
            odd[x] = wrap16((hl[x] * 2) + ((last[x] * 2) >> 1));
        }
    }
}

// ---------------------------------------------------------------------------
// Reduce-extrapolate DWT. The inverse is FreeRDP progressive.c
// progressive_rfx_idwt_x/y, rewritten as separate even and odd steps (the
// same values: even outputs depend only on the input, odd outputs on the
// neighbouring evens). Integer division rounds toward zero, as in FreeRDP.

[[nodiscard]] constexpr std::size_t band_l_count(std::size_t level) noexcept
{
    return (64U >> level) + 1;
}

[[nodiscard]] constexpr std::size_t band_h_count(std::size_t level) noexcept
{
    return level == 1 ? (64U >> 1U) - 1 : (64U + (1U << (level - 1))) >> level;
}

/// A lifting step applied to a whole row at once (`width` values), or to a
/// single value (width 1): the same code drives the horizontal pass (one
/// line, step 1) and the vertical pass (rows of `width`, step `stride`).
struct Lines {
    i16* base;
    std::size_t step;  // distance between consecutive elements of a line
    [[nodiscard]] i16* at(std::size_t i) const noexcept { return base + (i * step); }
};

/// Inverse lifting of nl low and nh high lines into nl + nh output lines,
/// each line `width` values long (progressive_rfx_idwt_x/y).
FARLAND_KERNEL void unlift_extrapolate(Lines low, Lines high, Lines dst, std::size_t nl, std::size_t nh,
                                       std::size_t width) noexcept
{
    const auto rows = [width](auto&& f) {
        for (std::size_t x = 0; x < width; ++x) {
            f(x);
        }
    };
    // Even outputs.
    {
        const i16* __restrict l0 = low.at(0);
        const i16* __restrict h0 = high.at(0);
        i16* __restrict e0 = dst.at(0);
        rows([&](std::size_t x) { e0[x] = sat16(l0[x] - h0[x]); });
    }
    for (std::size_t j = 1; j < nh; ++j) {
        const i16* __restrict lj = low.at(j);
        const i16* __restrict hp = high.at(j - 1);
        const i16* __restrict hj = high.at(j);
        i16* __restrict ej = dst.at(2 * j);
        rows([&](std::size_t x) { ej[x] = sat16(lj[x] - ((hp[x] + hj[x]) / 2)); });
    }
    // The last even output and whatever follows it.
    const i16* __restrict hl = high.at(nh - 1);
    const i16* __restrict el = dst.at((2 * nh) - 2);
    if (nl > nh) {
        i16* __restrict e = dst.at(2 * nh);
        const i16* __restrict l = low.at(nh);
        if (nl == nh + 1) {
            rows([&](std::size_t x) { e[x] = sat16(l[x] - hl[x]); });
        } else {
            rows([&](std::size_t x) { e[x] = sat16(l[x] - (hl[x] / 2)); });
            const i16* __restrict l1 = low.at(nh + 1);
            i16* __restrict tail = dst.at((2 * nh) + 1);
            rows([&](std::size_t x) { tail[x] = sat16((e[x] + l1[x]) / 2); });
        }
        i16* __restrict o = dst.at((2 * nh) - 1);
        rows([&](std::size_t x) { o[x] = sat16(((e[x] + el[x]) / 2) + (2 * hl[x])); });
    } else {
        i16* __restrict o = dst.at((2 * nh) - 1);
        rows([&](std::size_t x) { o[x] = sat16(el[x] + (2 * hl[x])); });
    }
    // The other odd outputs.
    for (std::size_t j = 0; j + 1 < nh; ++j) {
        const i16* __restrict even = dst.at(2 * j);
        const i16* __restrict next_even = dst.at((2 * j) + 2);
        const i16* __restrict hj = high.at(j);
        i16* __restrict o = dst.at((2 * j) + 1);
        rows([&](std::size_t x) { o[x] = sat16(((even[x] + next_even[x]) / 2) + (2 * hj[x])); });
    }
}

/// Forward lifting of nl + nh input lines into nl low and nh high lines, the
/// inverse of unlift_extrapolate ([MS-RDPEGFX] 3.2.8.1.2.2). With nl = nh + 2
/// (the first level: 64 inputs, 33 low, 31 high) a 65th input is
/// extrapolated from the last two; its high coefficient is zero and dropped.
FARLAND_KERNEL void lift_extrapolate(Lines src, Lines low, Lines high, std::size_t nl, std::size_t nh,
                                     std::size_t width) noexcept
{
    const auto rows = [width](auto&& f) {
        for (std::size_t x = 0; x < width; ++x) {
            f(x);
        }
    };
    // High: the odd inputs minus the mean of their neighbours, halved.
    for (std::size_t j = 0; j < nh; ++j) {
        const i16* __restrict s0 = src.at(2 * j);
        const i16* __restrict s1 = src.at((2 * j) + 1);
        const i16* __restrict s2 = src.at((2 * j) + 2);
        i16* __restrict h = high.at(j);
        rows([&](std::size_t x) { h[x] = sat16((s1[x] - ((s0[x] + s2[x]) / 2)) / 2); });
    }
    // Low: the even inputs plus the mean of the neighbouring highs, the
    // first and last mirrored.
    {
        const i16* __restrict s = src.at(0);
        const i16* __restrict h = high.at(0);
        i16* __restrict l = low.at(0);
        rows([&](std::size_t x) { l[x] = sat16(s[x] + h[x]); });
    }
    for (std::size_t j = 1; j < nh; ++j) {
        const i16* __restrict s = src.at(2 * j);
        const i16* __restrict hp = high.at(j - 1);
        const i16* __restrict hj = high.at(j);
        i16* __restrict l = low.at(j);
        rows([&](std::size_t x) { l[x] = sat16(s[x] + ((hp[x] + hj[x]) / 2)); });
    }
    const i16* __restrict s = src.at(2 * nh);
    const i16* __restrict hl = high.at(nh - 1);
    i16* __restrict l = low.at(nh);
    if (nl == nh + 1) {
        rows([&](std::size_t x) { l[x] = sat16(s[x] + hl[x]); });
    } else {
        FARLAND_ASSERT(nl == nh + 2);
        // x[64] = 2 x[63] - x[62]: the high at 63 is zero, so low[32] = x[64].
        const i16* __restrict s1 = src.at((2 * nh) + 1);
        i16* __restrict l1 = low.at(nh + 1);
        rows([&](std::size_t x) { l[x] = sat16(s[x] + (hl[x] / 2)); });
        rows([&](std::size_t x) { l1[x] = sat16((2 * s1[x]) - s[x]); });
    }
}

/// One level of the forward reduce-extrapolate DWT on the n x n image at
/// `buf` (n = nl + nh, rows of n): HL (nl x nh), LH (nh x nl), HH (nh x nh)
/// and LL (nl x nl) back to back, LL being the next level's input.
FARLAND_KERNEL void dwt_encode_extrapolate_block(i16* buf, i16* temp, std::size_t level) noexcept
{
    const std::size_t nl = band_l_count(level);
    const std::size_t nh = band_h_count(level);
    const std::size_t n = nl + nh;
    // Vertical: rows of the image into L rows and H rows of n values.
    lift_extrapolate({buf, n}, {temp, n}, {temp + (nl * n), n}, nl, nh, n);
    // Horizontal: each L row into LL and HL, each H row into LH and HH.
    i16* hl = buf;
    i16* lh = hl + (nl * nh);
    i16* hh = lh + (nh * nl);
    i16* ll = hh + (nh * nh);
    for (std::size_t i = 0; i < nl; ++i) {
        lift_extrapolate({temp + (i * n), 1}, {ll + (i * nl), 1}, {hl + (i * nh), 1}, nl, nh, 1);
    }
    for (std::size_t i = 0; i < nh; ++i) {
        lift_extrapolate({temp + ((nl + i) * n), 1}, {lh + (i * nl), 1}, {hh + (i * nh), 1}, nl, nh, 1);
    }
}

FARLAND_KERNEL void dwt_decode_extrapolate_block(i16* buf, i16* temp, std::size_t level) noexcept
{
    const std::size_t nl = band_l_count(level);
    const std::size_t nh = band_h_count(level);
    const std::size_t n = nl + nh;
    i16* hl = buf;
    i16* lh = hl + (nh * nl);
    i16* hh = lh + (nl * nh);
    i16* ll = hh + (nh * nh);
    // Horizontal (LL + HL -> L rows, LH + HH -> H rows).
    for (std::size_t i = 0; i < nl; ++i) {
        unlift_extrapolate({ll + (i * nl), 1}, {hl + (i * nh), 1}, {temp + (i * n), 1}, nl, nh, 1);
    }
    for (std::size_t i = 0; i < nh; ++i) {
        unlift_extrapolate({lh + (i * nl), 1}, {hh + (i * nh), 1}, {temp + ((nl + i) * n), 1}, nl, nh, 1);
    }
    // Vertical (L + H -> output), whole rows at once.
    unlift_extrapolate({temp, n}, {temp + (nl * n), n}, {buf, n}, nl, nh, n);
}

// ---------------------------------------------------------------------------
// Quantization (FreeRDP rfx_quantization_encode)

FARLAND_KERNEL void quantize_bands(i16* data, const Quant& quant, const Layout& layout) noexcept
{
    for (std::size_t b = 0; b < band_count; ++b) {
        const std::uint32_t factor = quant.bands[b] - 6U;
        i16* __restrict p = data + layout[b].offset;
        const std::size_t size = layout[b].size;
        // A rounding shift by the band's factor, then by 5 for the 11.5 fixed
        // point of the colour transform, each wrapped to 16 bits.
        if (factor == 0) {
            for (std::size_t i = 0; i < size; ++i) {
                p[i] = wrap16((p[i] + 16) >> 5);
            }
        } else {
            const i32 half = i32{1} << (factor - 1);
            for (std::size_t i = 0; i < size; ++i) {
                const i16 v = wrap16((p[i] + half) >> factor);
                p[i] = wrap16((v + 16) >> 5);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Entry points per instruction set.

using DwtFn = void (*)(i16*, i16*) noexcept;
using QuantizeFn = void (*)(i16*, const Quant&, const Layout&) noexcept;
using RgbRowFn = void (*)(const std::uint8_t*, i16*, i16*, i16*) noexcept;

struct Kernels {
    Isa isa;
    RgbRowFn rgb_row;
    DwtFn dwt_encode;
    DwtFn dwt_encode_extrapolate;
    DwtFn dwt_decode;
    DwtFn dwt_decode_extrapolate;
    QuantizeFn quantize;
};

void dwt_encode_base(i16* data, i16* scratch) noexcept
{
    dwt_encode_block(data, scratch, 32);
    dwt_encode_block(data + 3072, scratch, 16);
    dwt_encode_block(data + 3840, scratch, 8);
}

void dwt_decode_base(i16* data, i16* scratch) noexcept
{
    dwt_decode_block(data + 3840, scratch, 8);
    dwt_decode_block(data + 3072, scratch, 16);
    dwt_decode_block(data, scratch, 32);
}

void dwt_encode_extrapolate_base(i16* data, i16* scratch) noexcept
{
    dwt_encode_extrapolate_block(data, scratch, 1);
    dwt_encode_extrapolate_block(data + 3007, scratch, 2);
    dwt_encode_extrapolate_block(data + 3807, scratch, 3);
}

void dwt_decode_extrapolate_base(i16* data, i16* scratch) noexcept
{
    dwt_decode_extrapolate_block(data + 3807, scratch, 3);
    dwt_decode_extrapolate_block(data + 3007, scratch, 2);
    dwt_decode_extrapolate_block(data, scratch, 1);
}

void quantize_base(i16* data, const Quant& quant, const Layout& layout) noexcept
{
    quantize_bands(data, quant, layout);
}

constexpr Kernels scalar_kernels{Isa::scalar,     rgb_row_scalar,
                                 dwt_encode_base, dwt_encode_extrapolate_base,
                                 dwt_decode_base, dwt_decode_extrapolate_base,
                                 quantize_base};

#if defined(FARLAND_RFX_X86)

__attribute__((target("avx2"))) void dwt_encode_avx2(i16* data, i16* scratch) noexcept
{
    dwt_encode_block(data, scratch, 32);
    dwt_encode_block(data + 3072, scratch, 16);
    dwt_encode_block(data + 3840, scratch, 8);
}

__attribute__((target("avx2"))) void dwt_decode_avx2(i16* data, i16* scratch) noexcept
{
    dwt_decode_block(data + 3840, scratch, 8);
    dwt_decode_block(data + 3072, scratch, 16);
    dwt_decode_block(data, scratch, 32);
}

__attribute__((target("avx2"))) void dwt_encode_extrapolate_avx2(i16* data, i16* scratch) noexcept
{
    dwt_encode_extrapolate_block(data, scratch, 1);
    dwt_encode_extrapolate_block(data + 3007, scratch, 2);
    dwt_encode_extrapolate_block(data + 3807, scratch, 3);
}

__attribute__((target("avx2"))) void dwt_decode_extrapolate_avx2(i16* data, i16* scratch) noexcept
{
    dwt_decode_extrapolate_block(data + 3807, scratch, 3);
    dwt_decode_extrapolate_block(data + 3007, scratch, 2);
    dwt_decode_extrapolate_block(data, scratch, 1);
}

__attribute__((target("avx2"))) void quantize_avx2(i16* data, const Quant& quant, const Layout& layout) noexcept
{
    quantize_bands(data, quant, layout);
}

constexpr Kernels sse2_kernels{Isa::sse2,       rgb_row_sse2,
                               dwt_encode_base, dwt_encode_extrapolate_base,
                               dwt_decode_base, dwt_decode_extrapolate_base,
                               quantize_base};
constexpr Kernels avx2_kernels{Isa::avx2,       rgb_row_avx2,
                               dwt_encode_avx2, dwt_encode_extrapolate_avx2,
                               dwt_decode_avx2, dwt_decode_extrapolate_avx2,
                               quantize_avx2};

#endif

#if defined(FARLAND_RFX_NEON)
constexpr Kernels neon_kernels{Isa::neon,       rgb_row_neon,
                               dwt_encode_base, dwt_encode_extrapolate_base,
                               dwt_decode_base, dwt_decode_extrapolate_base,
                               quantize_base};
#endif

struct Available {
    std::array<const Kernels*, 3> kernels{};
    std::array<Isa, 3> isas{};
    std::size_t count = 0;

    void add(const Kernels& k) noexcept
    {
        kernels.at(count) = &k;
        isas.at(count) = k.isa;
        ++count;
    }
};

const Available& available() noexcept
{
    static const Available list = [] {
        Available a;
        a.add(scalar_kernels);
#if defined(FARLAND_RFX_X86)
        a.add(sse2_kernels);
        __builtin_cpu_init();
        if (__builtin_cpu_supports("avx2")) {
            a.add(avx2_kernels);
        }
#elif defined(FARLAND_RFX_NEON)
        a.add(neon_kernels);
#endif
        return a;
    }();
    return list;
}

std::atomic<const Kernels*>& active_kernels() noexcept
{
    static std::atomic<const Kernels*> active{available().kernels.at(available().count - 1)};
    return active;
}

const Kernels& kernels() noexcept
{
    return *active_kernels().load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// RLGR encoding (FreeRDP rfx_rlgr.c, [MS-RDPRFX] 3.1.8.1.7.3.2)

constexpr std::uint32_t kp_max = 80;  // KPMAX: largest kp or krp
constexpr std::uint32_t lsgr = 3;     // LSGR: kp >> LSGR = k
constexpr std::int32_t up_gr = 4;     // UP_GR: kp increase after a zero run in RL mode
constexpr std::int32_t dn_gr = 6;     // DN_GR: kp decrease after a nonzero symbol in RL mode
constexpr std::int32_t uq_gr = 3;     // UQ_GR: kp increase after a zero symbol in GR mode
constexpr std::int32_t dq_gr = 3;     // DQ_GR: kp decrease after a nonzero symbol in GR mode

/// MSB-first bit output with a 64-bit accumulator; the same bytes as
/// BitWriter.
class BitSink {
public:
    explicit BitSink(std::vector<std::byte>& out) noexcept : out_(out) {}

    /// Writes `count` (0..32) bits; `bits` must be below 2^count.
    FARLAND_KERNEL void put(std::uint32_t bits, std::uint32_t count)
    {
        acc_ = (acc_ << count) | bits;
        pending_ += count;
        if (pending_ >= 32) {
            pending_ -= 32;
            const auto word = static_cast<std::uint32_t>(acc_ >> pending_);
            out_.push_back(static_cast<std::byte>(word >> 24U));
            out_.push_back(static_cast<std::byte>(word >> 16U));
            out_.push_back(static_cast<std::byte>(word >> 8U));
            out_.push_back(static_cast<std::byte>(word));
        }
    }
    void put_ones(std::uint32_t count)
    {
        while (count >= 32) {
            put(0xFFFFFFFFU, 32);
            count -= 32;
        }
        put((std::uint32_t{1} << count) - 1U, count);
    }
    void flush()
    {
        if (pending_ % 8 != 0) {
            put(0, 8 - (pending_ % 8));
        }
        while (pending_ >= 8) {
            pending_ -= 8;
            out_.push_back(static_cast<std::byte>(acc_ >> pending_));
        }
    }

private:
    // A short-lived writer on the caller's buffer, never copied or stored.
    std::vector<std::byte>& out_;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)
    std::uint64_t acc_ = 0;  // the low `pending_` bits are unwritten output
    std::uint32_t pending_ = 0;
};

/// UpdateParam: adds `delta`, clamps to [0, KPMAX], returns param >> LSGR.
FARLAND_KERNEL std::uint32_t update_param(std::uint32_t& param, std::int32_t delta) noexcept
{
    const std::int64_t v = std::clamp<std::int64_t>(std::int64_t{param} + delta, 0, kp_max);
    param = static_cast<std::uint32_t>(v);
    return param >> lsgr;
}

/// Golomb-Rice code of a non-negative value (rfx_rlgr_code_gr).
FARLAND_KERNEL void code_gr(BitSink& bs, std::uint32_t& krp, std::uint32_t val)
{
    const std::uint32_t kr = krp >> lsgr;
    const std::uint32_t vk = val >> kr;
    const std::uint32_t low = val & ((std::uint32_t{1} << kr) - 1U);
    if (vk + 1 + kr <= 32) {
        // vk ones, a zero, then the kr low bits, in one go.
        const std::uint64_t ones = ((std::uint64_t{1} << vk) - 1U) << (kr + 1);
        bs.put(static_cast<std::uint32_t>(ones | low), vk + 1 + kr);
    } else {
        bs.put_ones(vk);
        bs.put(0, 1);
        bs.put(low, kr);
    }
    if (vk == 0) {
        update_param(krp, -2);
    } else if (vk > 1) {
        update_param(krp, static_cast<std::int32_t>(std::min<std::uint32_t>(vk, kp_max)));
    }
}

[[nodiscard]] FARLAND_KERNEL std::uint32_t two_mag_sign(std::int32_t input) noexcept
{
    return input >= 0 ? static_cast<std::uint32_t>(2 * input) : static_cast<std::uint32_t>((-2 * input) - 1);
}

/// The index of the first nonzero value at or after `i`, but at most
/// size - 1 (the last value ends a run even when it is zero).
[[nodiscard]] FARLAND_KERNEL std::size_t run_end(const i16* data, std::size_t i, std::size_t size) noexcept
{
    while (i + 4 < size) {
        std::uint64_t four = 0;
        std::memcpy(&four, data + i, sizeof four);
        if (four != 0) {
            break;
        }
        i += 4;
    }
    while (i + 1 < size && data[i] == 0) {
        ++i;
    }
    return i;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public entry points

void load_tile(const ImageView& image, std::uint32_t x0, std::uint32_t y0, Planes& out)
{
    FARLAND_ASSERT(x0 < image.width && y0 < image.height);
    FARLAND_ASSERT(image.stride >= std::size_t{image.width} * 4);
    const std::size_t w = std::min<std::size_t>(tile_size, image.width - x0);
    const std::size_t h = std::min<std::size_t>(tile_size, image.height - y0);
    const std::size_t first = (std::size_t{y0} * image.stride) + (std::size_t{x0} * 4);
    FARLAND_ASSERT(image.data.size() >= first + ((h - 1) * image.stride) + (w * 4));
    const auto* base = reinterpret_cast<const std::uint8_t*>(image.data.data()) + first;
    const RgbRowFn row_fn = kernels().rgb_row;
    std::array<std::uint8_t, tile_size * 4> padded{};
    for (std::size_t row = 0; row < tile_size; ++row) {
        // Rows and columns past the edge repeat the last real one.
        const std::uint8_t* line = base + (std::min(row, h - 1) * image.stride);
        if (w < tile_size) {
            std::memcpy(padded.data(), line, w * 4);
            for (std::size_t col = w; col < tile_size; ++col) {
                std::memcpy(padded.data() + (col * 4), line + ((w - 1) * 4), 4);
            }
            line = padded.data();
        }
        const std::size_t at = row * tile_size;
        row_fn(line, out.y.data() + at, out.cb.data() + at, out.cr.data() + at);
    }
}

void dwt_encode(Coefficients& data, Coefficients& scratch) noexcept
{
    kernels().dwt_encode(data.data(), scratch.data());
}

void dwt_decode(Coefficients& data, Coefficients& scratch) noexcept
{
    kernels().dwt_decode(data.data(), scratch.data());
}

void dwt_encode_extrapolate(Coefficients& data, Coefficients& scratch) noexcept
{
    kernels().dwt_encode_extrapolate(data.data(), scratch.data());
}

void dwt_decode_extrapolate(Coefficients& data, Coefficients& scratch) noexcept
{
    kernels().dwt_decode_extrapolate(data.data(), scratch.data());
}

void quantize(Coefficients& data, const Quant& quant, const Layout& layout) noexcept
{
    for (const std::uint8_t q : quant.bands) {
        FARLAND_ASSERT(q >= 6 && q <= 15);
    }
    kernels().quantize(data.data(), quant, layout);
}

void rlgr_encode(RlgrMode mode, std::span<const std::int16_t> data, std::vector<std::byte>& out)
{
    BitSink bs(out);
    const i16* d = data.data();
    const std::size_t size = data.size();
    std::uint32_t k = 1;
    std::uint32_t kp = 1U << lsgr;
    std::uint32_t krp = 1U << lsgr;
    std::size_t idx = 0;
    // GetNextInput: zeros once the input is exhausted.
    const auto next = [&]() -> std::int32_t { return idx < size ? d[idx++] : 0; };

    while (idx < size) {
        if (k != 0) {
            // Run-length mode: a run of zeros, then the value that ends it.
            const std::size_t end = run_end(d, idx, size);
            auto zeros = static_cast<std::uint32_t>(end - idx);
            const std::int32_t input = d[end];
            idx = end + 1;
            std::uint32_t runmax = 1U << k;
            while (zeros >= runmax) {
                bs.put(0, 1);
                zeros -= runmax;
                k = update_param(kp, up_gr);
                runmax = 1U << k;
            }
            bs.put(1, 1);
            bs.put(zeros, k);
            const auto mag = static_cast<std::uint32_t>(input < 0 ? -input : input);
            bs.put(input < 0 ? 1U : 0U, 1);
            code_gr(bs, krp, mag > 0 ? mag - 1 : 0);
            k = update_param(kp, -dn_gr);
        } else if (mode == RlgrMode::rlgr1) {
            // Golomb-Rice mode, RLGR1: one value as 2 * magnitude - sign.
            const std::uint32_t two_ms = two_mag_sign(next());
            code_gr(bs, krp, two_ms);
            k = two_ms != 0 ? update_param(kp, -dq_gr) : update_param(kp, uq_gr);
        } else {
            // Golomb-Rice mode, RLGR3: the sum of two values, then the first.
            const std::uint32_t two_ms1 = two_mag_sign(next());
            const std::uint32_t two_ms2 = two_mag_sign(next());
            const std::uint32_t sum = two_ms1 + two_ms2;
            code_gr(bs, krp, sum);
            bs.put(two_ms1, static_cast<std::uint32_t>(std::bit_width(sum)));
            if (two_ms1 != 0 && two_ms2 != 0) {
                k = update_param(kp, -2 * dq_gr);
            } else if (two_ms1 == 0 && two_ms2 == 0) {
                k = update_param(kp, 2 * uq_gr);
            }
        }
    }
    bs.flush();
}

// ---------------------------------------------------------------------------
// Kernel selection

std::span<const Isa> available_isas() noexcept
{
    const Available& a = available();
    return std::span(a.isas).first(a.count);
}

Isa active_isa() noexcept
{
    return kernels().isa;
}

void set_isa(Isa isa) noexcept
{
    const Available& a = available();
    for (std::size_t i = 0; i < a.count; ++i) {
        if (a.isas.at(i) == isa) {
            active_kernels().store(a.kernels.at(i), std::memory_order_relaxed);
            return;
        }
    }
    FARLAND_ASSERT(false && "instruction set not available");
}

const char* isa_name(Isa isa) noexcept
{
    switch (isa) {
    case Isa::scalar:
        return "scalar";
    case Isa::sse2:
        return "SSE2";
    case Isa::avx2:
        return "AVX2";
    case Isa::neon:
        return "NEON";
    }
    return "?";
}

}  // namespace farland::codec::rfx

#if defined(__clang__)
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic,cppcoreguidelines-pro-type-reinterpret-cast)
#pragma clang unsafe_buffer_usage end
#endif
