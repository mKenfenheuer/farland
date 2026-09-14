// SPDX-FileCopyrightText: 2011 Vic Lee
// SPDX-FileCopyrightText: 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
// SPDX-FileCopyrightText: 2019 Armin Novak <armin.novak@thincast.com>
// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Straightforward scalar versions of the RemoteFX kernels, as farland had
// them before rfx_kernels.cpp (translated from FreeRDP 3's rfx_dwt.c,
// rfx_quantization.c, rfx_rlgr.c, progressive.c and prim_colors.c;
// modified: std::span). The tests hold every kernel and instruction set to
// these bit for bit. The forward reduce-extrapolate DWT has no FreeRDP
// counterpart; its reference follows [MS-RDPEGFX] 3.2.8.1.2.2 one line at a
// time.

#include <farland/codec/image.hpp>
#include <farland/codec/rfx_common.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace farland::test::rfx_reference {

namespace rfx = farland::codec::rfx;

inline std::int16_t to_i16(std::int32_t v)
{
    return static_cast<std::int16_t>(v);
}

inline std::int16_t clamp_i16(std::int32_t v)
{
    return static_cast<std::int16_t>(std::clamp<std::int32_t>(v, INT16_MIN, INT16_MAX));
}

inline void load_tile(const codec::ImageView& image, std::uint32_t x0, std::uint32_t y0, rfx::Planes& out)
{
    const std::size_t w = std::min<std::size_t>(64, image.width - x0);
    const std::size_t h = std::min<std::size_t>(64, image.height - y0);
    for (std::size_t row = 0; row < 64; ++row) {
        const std::size_t src_row = std::min(row, h - 1);
        const auto line = image.data.subspan(((y0 + src_row) * image.stride) + (std::size_t{x0} * 4), w * 4);
        for (std::size_t col = 0; col < 64; ++col) {
            const auto pixel = line.subspan(std::min(col, w - 1) * 4, 4);
            const auto c =
                rfx::rgb_to_ycbcr(std::to_integer<std::int32_t>(pixel[2]), std::to_integer<std::int32_t>(pixel[1]),
                                  std::to_integer<std::int32_t>(pixel[0]));
            const std::size_t i = (row * 64) + col;
            out.y[i] = c.y;
            out.cb[i] = c.cb;
            out.cr[i] = c.cr;
        }
    }
}

inline void dwt_encode_block(std::span<std::int16_t> buf, std::span<std::int16_t> dwt, std::size_t sw)
{
    const std::size_t total = sw * 2;
    for (std::size_t x = 0; x < total; ++x) {
        for (std::size_t n = 0; n < sw; ++n) {
            const std::size_t y = n * 2;
            const std::size_t l = (n * total) + x;
            const std::size_t h = l + (sw * total);
            const std::size_t s = (y * total) + x;
            const std::size_t next_even = n < sw - 1 ? s + (2 * total) : s;
            dwt[h] = to_i16((buf[s + total] - ((buf[s] + buf[next_even]) >> 1)) >> 1);
            dwt[l] = to_i16(buf[s] + (n == 0 ? dwt[h] : (dwt[h - total] + dwt[h]) >> 1));
        }
    }
    const std::size_t band = sw * sw;
    for (std::size_t y = 0; y < sw; ++y) {
        const auto l_src = dwt.subspan(y * total, total);
        const auto h_src = dwt.subspan((2 * band) + (y * total), total);
        const auto hl = buf.subspan(y * sw, sw);
        const auto lh = buf.subspan(band + (y * sw), sw);
        const auto hh = buf.subspan((2 * band) + (y * sw), sw);
        const auto ll = buf.subspan((3 * band) + (y * sw), sw);
        for (std::size_t n = 0; n < sw; ++n) {
            const std::size_t x = n * 2;
            const std::size_t xn = n < sw - 1 ? x + 2 : x;
            hl[n] = to_i16((l_src[x + 1] - ((l_src[x] + l_src[xn]) >> 1)) >> 1);
            ll[n] = to_i16(l_src[x] + (n == 0 ? hl[n] : (hl[n - 1] + hl[n]) >> 1));
        }
        for (std::size_t n = 0; n < sw; ++n) {
            const std::size_t x = n * 2;
            const std::size_t xn = n < sw - 1 ? x + 2 : x;
            hh[n] = to_i16((h_src[x + 1] - ((h_src[x] + h_src[xn]) >> 1)) >> 1);
            lh[n] = to_i16(h_src[x] + (n == 0 ? hh[n] : (hh[n - 1] + hh[n]) >> 1));
        }
    }
}

inline void dwt_decode_block(std::span<std::int16_t> buf, std::span<std::int16_t> idwt, std::size_t sw)
{
    const std::size_t total = sw * 2;
    const std::size_t band = sw * sw;
    for (std::size_t y = 0; y < sw; ++y) {
        const auto hl = buf.subspan(y * sw, sw);
        const auto lh = buf.subspan(band + (y * sw), sw);
        const auto hh = buf.subspan((2 * band) + (y * sw), sw);
        const auto ll = buf.subspan((3 * band) + (y * sw), sw);
        const auto l_dst = idwt.subspan(y * total, total);
        const auto h_dst = idwt.subspan((2 * band) + (y * total), total);
        l_dst[0] = to_i16(ll[0] - ((hl[0] + hl[0] + 1) >> 1));
        h_dst[0] = to_i16(lh[0] - ((hh[0] + hh[0] + 1) >> 1));
        for (std::size_t n = 1; n < sw; ++n) {
            l_dst[n * 2] = to_i16(ll[n] - ((hl[n - 1] + hl[n] + 1) >> 1));
            h_dst[n * 2] = to_i16(lh[n] - ((hh[n - 1] + hh[n] + 1) >> 1));
        }
        std::size_t n = 0;
        for (; n < sw - 1; ++n) {
            const std::size_t x = n * 2;
            l_dst[x + 1] = to_i16((hl[n] * 2) + ((l_dst[x] + l_dst[x + 2]) >> 1));
            h_dst[x + 1] = to_i16((hh[n] * 2) + ((h_dst[x] + h_dst[x + 2]) >> 1));
        }
        const std::size_t x = n * 2;
        l_dst[x + 1] = to_i16((hl[n] * 2) + l_dst[x]);
        h_dst[x + 1] = to_i16((hh[n] * 2) + h_dst[x]);
    }
    for (std::size_t x = 0; x < total; ++x) {
        std::size_t l = x;
        std::size_t h = x + (sw * total);
        std::size_t dst = x;
        buf[dst] = to_i16(idwt[l] - ((idwt[h] * 2 + 1) >> 1));
        for (std::size_t n = 1; n < sw; ++n) {
            l += total;
            h += total;
            buf[dst + (2 * total)] = to_i16(idwt[l] - ((idwt[h - total] + idwt[h] + 1) >> 1));
            buf[dst + total] = to_i16((idwt[h - total] * 2) + ((buf[dst] + buf[dst + (2 * total)]) >> 1));
            dst += 2 * total;
        }
        buf[dst + total] = to_i16((idwt[h] * 2) + ((buf[dst] * 2) >> 1));
    }
}

inline void dwt_encode(rfx::Coefficients& data)
{
    std::vector<std::int16_t> scratch(4096);
    const std::span<std::int16_t> buf(data);
    dwt_encode_block(buf, scratch, 32);
    dwt_encode_block(buf.subspan(3072), scratch, 16);
    dwt_encode_block(buf.subspan(3840), scratch, 8);
}

inline void dwt_decode(rfx::Coefficients& data)
{
    std::vector<std::int16_t> scratch(4096);
    const std::span<std::int16_t> buf(data);
    dwt_decode_block(buf.subspan(3840), scratch, 8);
    dwt_decode_block(buf.subspan(3072), scratch, 16);
    dwt_decode_block(buf, scratch, 32);
}

/// Strided access into a span.
struct Line {
    std::span<std::int16_t> data;
    std::size_t base = 0;
    std::size_t step = 1;
    [[nodiscard]] std::int16_t& operator[](std::size_t i) const { return data[base + (i * step)]; }
};

/// FreeRDP progressive_rfx_idwt_x/y for one line.
inline void idwt_line(const Line& low, const Line& high, const Line& dst, std::size_t low_count, std::size_t high_count)
{
    std::int32_t h0 = high[0];
    std::int32_t l0 = low[0];
    std::int32_t x0 = clamp_i16(l0 - h0);
    std::int32_t x2 = x0;
    std::size_t li = 1;
    std::size_t hi = 1;
    std::size_t di = 0;
    for (std::size_t j = 0; j + 1 < high_count; ++j) {
        const std::int32_t h1 = high[hi++];
        l0 = low[li++];
        x2 = clamp_i16(l0 - ((h0 + h1) / 2));
        const std::int32_t x1 = clamp_i16(((x0 + x2) / 2) + (2 * h0));
        dst[di++] = static_cast<std::int16_t>(x0);
        dst[di++] = static_cast<std::int16_t>(x1);
        x0 = x2;
        h0 = h1;
    }
    if (low_count <= high_count + 1) {
        if (low_count <= high_count) {
            dst[di++] = static_cast<std::int16_t>(x2);
            dst[di] = clamp_i16(x2 + (2 * h0));
        } else {
            l0 = low[li];
            x0 = clamp_i16(l0 - h0);
            dst[di++] = static_cast<std::int16_t>(x2);
            dst[di++] = clamp_i16(((x0 + x2) / 2) + (2 * h0));
            dst[di] = static_cast<std::int16_t>(x0);
        }
    } else {
        l0 = low[li++];
        x0 = clamp_i16(l0 - (h0 / 2));
        dst[di++] = static_cast<std::int16_t>(x2);
        dst[di++] = clamp_i16(((x0 + x2) / 2) + (2 * h0));
        dst[di++] = static_cast<std::int16_t>(x0);
        l0 = low[li];
        dst[di] = clamp_i16((x0 + l0) / 2);
    }
}

constexpr std::size_t band_l_count(std::size_t level)
{
    return (64U >> level) + 1;
}

constexpr std::size_t band_h_count(std::size_t level)
{
    return level == 1 ? (64U >> 1U) - 1 : (64U + (1U << (level - 1))) >> level;
}

inline void dwt_decode_extrapolate(rfx::Coefficients& data)
{
    std::vector<std::int16_t> temp(4096);
    const auto block = [&](std::span<std::int16_t> buf, std::size_t level) {
        const std::size_t nl = band_l_count(level);
        const std::size_t nh = band_h_count(level);
        const std::size_t step = nl + nh;
        const std::size_t hl = 0;
        const std::size_t lh = hl + (nh * nl);
        const std::size_t hh = lh + (nl * nh);
        const std::size_t ll = hh + (nh * nh);
        const std::size_t h = nl * step;
        for (std::size_t i = 0; i < nl; ++i) {
            idwt_line({buf, ll + (i * nl), 1}, {buf, hl + (i * nh), 1}, {temp, i * step, 1}, nl, nh);
        }
        for (std::size_t i = 0; i < nh; ++i) {
            idwt_line({buf, lh + (i * nl), 1}, {buf, hh + (i * nh), 1}, {temp, h + (i * step), 1}, nl, nh);
        }
        for (std::size_t i = 0; i < step; ++i) {
            idwt_line({temp, i, step}, {temp, h + i, step}, {buf, i, step}, nl, nh);
        }
    };
    const std::span<std::int16_t> buf(data);
    block(buf.subspan(3807), 3);
    block(buf.subspan(3007), 2);
    block(buf, 1);
}

/// [MS-RDPEGFX] 3.2.8.1.2.2 on one line of nl + nh values: nl low and nh
/// high results. Level 1 (nl = nh + 2) extrapolates x[64] = 2 x[63] - x[62].
inline void fdwt_line(const Line& src, const Line& low, const Line& high, std::size_t nl, std::size_t nh)
{
    const std::size_t n = nl + nh;
    std::vector<std::int32_t> x(n + 1);
    for (std::size_t i = 0; i < n; ++i) {
        x[i] = src[i];
    }
    const bool extrapolate = nl == nh + 2;
    if (extrapolate) {
        x[n] = (2 * x[n - 1]) - x[n - 2];
    }
    // All highs, including the dropped (zero) one of level 1.
    const std::size_t highs = extrapolate ? nh + 1 : nh;
    std::vector<std::int32_t> hv(highs);
    for (std::size_t j = 0; j < highs; ++j) {
        hv[j] = (x[(2 * j) + 1] - ((x[2 * j] + x[(2 * j) + 2]) / 2)) / 2;
    }
    for (std::size_t j = 0; j < nh; ++j) {
        high[j] = clamp_i16(hv[j]);
    }
    for (std::size_t j = 0; j < nl; ++j) {
        const std::int32_t before = hv[j == 0 ? 0 : j - 1];
        const std::int32_t after = hv[std::min(j, highs - 1)];
        low[j] = clamp_i16(x[2 * j] + ((before + after) / 2));
    }
}

inline void dwt_encode_extrapolate(rfx::Coefficients& data)
{
    const auto block = [&](std::span<std::int16_t> buf, std::size_t level) {
        const std::size_t nl = band_l_count(level);
        const std::size_t nh = band_h_count(level);
        const std::size_t n = nl + nh;
        std::vector<std::int16_t> temp(n * n);
        for (std::size_t col = 0; col < n; ++col) {
            fdwt_line({buf, col, n}, {temp, col, n}, {temp, (nl * n) + col, n}, nl, nh);
        }
        const std::size_t hl = 0;
        const std::size_t lh = hl + (nl * nh);
        const std::size_t hh = lh + (nh * nl);
        const std::size_t ll = hh + (nh * nh);
        for (std::size_t i = 0; i < nl; ++i) {
            fdwt_line({temp, i * n, 1}, {buf, ll + (i * nl), 1}, {buf, hl + (i * nh), 1}, nl, nh);
        }
        for (std::size_t i = 0; i < nh; ++i) {
            fdwt_line({temp, (nl + i) * n, 1}, {buf, lh + (i * nl), 1}, {buf, hh + (i * nh), 1}, nl, nh);
        }
    };
    const std::span<std::int16_t> buf(data);
    block(buf, 1);
    block(buf.subspan(3007), 2);
    block(buf.subspan(3807), 3);
}

inline void quantize(rfx::Coefficients& data, const rfx::Quant& quant, const rfx::Layout& layout)
{
    const auto shift = [](std::span<std::int16_t> block, std::uint32_t factor) {
        if (factor == 0) {
            return;
        }
        const std::int32_t half = std::int32_t{1} << (factor - 1);
        for (auto& v : block) {
            v = to_i16((v + half) >> factor);
        }
    };
    const std::span<std::int16_t> buf(data);
    for (std::size_t b = 0; b < rfx::band_count; ++b) {
        shift(buf.subspan(layout[b].offset, layout[b].size), quant.bands[b] - 6U);
    }
    shift(buf, 5);
}

inline void rlgr_encode(rfx::RlgrMode mode, std::span<const std::int16_t> data, std::vector<std::byte>& out)
{
    constexpr std::uint32_t kp_max = 80;
    constexpr std::uint32_t lsgr = 3;
    const auto update_param = [](std::uint32_t& param, std::int32_t delta) {
        const std::int64_t v = std::clamp<std::int64_t>(std::int64_t{param} + delta, 0, kp_max);
        param = static_cast<std::uint32_t>(v);
        return param >> lsgr;
    };
    rfx::BitWriter bs(out);
    const auto code_gr = [&](std::uint32_t& krp, std::uint32_t val) {
        const std::uint32_t kr = krp >> lsgr;
        const std::uint32_t vk = val >> kr;
        bs.put_repeated(true, vk);
        bs.put(0, 1);
        if (kr > 0) {
            bs.put(val & ((1U << kr) - 1U), kr);
        }
        if (vk == 0) {
            update_param(krp, -2);
        } else if (vk > 1) {
            update_param(krp, static_cast<std::int32_t>(std::min<std::uint32_t>(vk, kp_max)));
        }
    };
    const auto two_mag_sign = [](std::int32_t input) {
        return input >= 0 ? static_cast<std::uint32_t>(2 * input) : static_cast<std::uint32_t>((-2 * input) - 1);
    };
    std::uint32_t k = 1;
    std::uint32_t kp = 1U << lsgr;
    std::uint32_t krp = 1U << lsgr;
    std::size_t idx = 0;
    const auto next = [&]() -> std::int32_t { return idx < data.size() ? data[idx++] : 0; };
    while (idx < data.size()) {
        if (k != 0) {
            std::uint32_t zeros = 0;
            std::int32_t input = next();
            while (input == 0 && idx < data.size()) {
                ++zeros;
                input = next();
            }
            std::uint32_t runmax = 1U << k;
            while (zeros >= runmax) {
                bs.put(0, 1);
                zeros -= runmax;
                k = update_param(kp, 4);
                runmax = 1U << k;
            }
            bs.put(1, 1);
            bs.put(zeros, k);
            const auto mag = static_cast<std::uint32_t>(input < 0 ? -input : input);
            bs.put(input < 0 ? 1U : 0U, 1);
            code_gr(krp, mag > 0 ? mag - 1 : 0);
            k = update_param(kp, -6);
        } else if (mode == rfx::RlgrMode::rlgr1) {
            const std::uint32_t two_ms = two_mag_sign(next());
            code_gr(krp, two_ms);
            k = two_ms != 0 ? update_param(kp, -3) : update_param(kp, 3);
        } else {
            const std::uint32_t two_ms1 = two_mag_sign(next());
            const std::uint32_t two_ms2 = two_mag_sign(next());
            const std::uint32_t sum = two_ms1 + two_ms2;
            code_gr(krp, sum);
            // GetMinBits by hand: GCC 16 at -O2 with -fwrapv (which the
            // test gets through libspa's -fno-strict-overflow) miscompiles
            // std::bit_width here.
            std::uint32_t min_bits = 0;
            for (std::uint32_t rest = sum; rest != 0; rest >>= 1U) {
                ++min_bits;
            }
            bs.put(two_ms1, min_bits);
            if (two_ms1 != 0 && two_ms2 != 0) {
                k = update_param(kp, -6);
            } else if (two_ms1 == 0 && two_ms2 == 0) {
                k = update_param(kp, 6);
            }
        }
    }
    bs.flush();
}

}  // namespace farland::test::rfx_reference
