// SPDX-FileCopyrightText: 2011 Vic Lee
// SPDX-FileCopyrightText: 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
// SPDX-FileCopyrightText: 2019 Armin Novak <armin.novak@thincast.com>
// SPDX-FileCopyrightText: 2019 Thincast Technologies GmbH
// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// RemoteFX primitives, [MS-RDPRFX] 3.1.8 and [MS-RDPEGFX] 3.1.8.1.
//
// Translated from FreeRDP 3 (Apache-2.0): libfreerdp/codec/rfx_dwt.c,
// rfx_quantization.c, rfx_rlgr.c, rfx_differential.h, rfx_bitstream.h,
// progressive.c (reduce-extrapolate IDWT, SRL/RAW upgrade reads) and
// libfreerdp/primitives/prim_colors.c. Modified: rewritten over std::span
// with bounds-checked indexing, wrapping casts instead of assertions, and a
// position-based bit reader equivalent to winpr's wBitStream.

#include <farland/base/assert.hpp>
#include <farland/codec/rfx_common.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace farland::codec::rfx {

namespace {

[[nodiscard]] constexpr std::int16_t to_i16(std::int32_t v) noexcept
{
    return static_cast<std::int16_t>(v);  // modulo 2^16, as in a release FreeRDP build
}

[[nodiscard]] constexpr std::int16_t clamp_i16(std::int32_t v) noexcept
{
    return static_cast<std::int16_t>(std::clamp<std::int32_t>(v, INT16_MIN, INT16_MAX));
}

}  // namespace

// ---------------------------------------------------------------------------
// RFX_COMPONENT_CODEC_QUANT, [MS-RDPEGFX] 2.2.4.2.1.5.2

Result<Quant> read_component_quant(Reader& r) noexcept
{
    FARLAND_TRY(const auto raw, r.bytes(component_quant_size));
    const auto lo = [&](std::size_t i) { return static_cast<std::uint8_t>(std::to_integer<unsigned>(raw[i]) & 0x0FU); };
    const auto hi = [&](std::size_t i) { return static_cast<std::uint8_t>(std::to_integer<unsigned>(raw[i]) >> 4U); };
    Quant q;
    q[Band::ll3] = lo(0);
    q[Band::hl3] = hi(0);
    q[Band::lh3] = lo(1);
    q[Band::hh3] = hi(1);
    q[Band::hl2] = lo(2);
    q[Band::lh2] = hi(2);
    q[Band::hh2] = lo(3);
    q[Band::hl1] = hi(3);
    q[Band::lh1] = lo(4);
    q[Band::hh1] = hi(4);
    return q;
}

void write_component_quant(Writer& w, const Quant& q)
{
    const auto pack = [](std::uint8_t lo, std::uint8_t hi) {
        FARLAND_ASSERT(lo <= 0x0F && hi <= 0x0F);
        return static_cast<std::uint8_t>(lo | (hi << 4U));
    };
    w.u8(pack(q[Band::ll3], q[Band::hl3]));
    w.u8(pack(q[Band::lh3], q[Band::hh3]));
    w.u8(pack(q[Band::hl2], q[Band::lh2]));
    w.u8(pack(q[Band::hh2], q[Band::hl1]));
    w.u8(pack(q[Band::lh1], q[Band::hh1]));
}

// ---------------------------------------------------------------------------
// Colour conversion

void load_tile(const ImageView& image, std::uint32_t x0, std::uint32_t y0, Planes& out)
{
    FARLAND_ASSERT(x0 < image.width && y0 < image.height);
    const std::size_t w = std::min<std::size_t>(tile_size, image.width - x0);
    const std::size_t h = std::min<std::size_t>(tile_size, image.height - y0);
    FARLAND_ASSERT(image.stride >= std::size_t{image.width} * 4);
    for (std::size_t row = 0; row < tile_size; ++row) {
        // Rows and columns past the edge repeat the last real one.
        const std::size_t src_row = std::min(row, h - 1);
        const auto line = image.data.subspan(((y0 + src_row) * image.stride) + (std::size_t{x0} * 4), w * 4);
        for (std::size_t col = 0; col < tile_size; ++col) {
            const auto pixel = line.subspan(std::min(col, w - 1) * 4, 4);
            const auto c =
                rgb_to_ycbcr(std::to_integer<std::int32_t>(pixel[2]), std::to_integer<std::int32_t>(pixel[1]),
                             std::to_integer<std::int32_t>(pixel[0]));
            const std::size_t i = (row * tile_size) + col;
            out.y[i] = c.y;
            out.cb[i] = c.cb;
            out.cr[i] = c.cr;
        }
    }
}

void store_tile(const Planes& planes, std::span<std::byte> out)
{
    FARLAND_ASSERT(out.size() == tile_coefficients * 4);
    for (std::size_t i = 0; i < tile_coefficients; ++i) {
        const auto c = ycbcr_to_rgb(planes.y[i], planes.cb[i], planes.cr[i]);
        const auto pixel = out.subspan(i * 4, 4);
        pixel[0] = std::byte{c.b};
        pixel[1] = std::byte{c.g};
        pixel[2] = std::byte{c.r};
        pixel[3] = std::byte{0xFF};
    }
}

// ---------------------------------------------------------------------------
// Classic DWT (FreeRDP rfx_dwt.c)

namespace {

/// One level of the forward transform on the block at `base`, whose input is
/// a (2 * sw) x (2 * sw) image. Output bands HL, LH, HH, LL of sw x sw each.
void dwt_encode_block(std::span<std::int16_t> buf, std::span<std::int16_t> dwt, std::size_t sw) noexcept
{
    const std::size_t total = sw * 2;
    // Vertical: L and H halves into dwt.
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
    // Horizontal: L gives HL and LL, H gives HH and LH.
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

void dwt_decode_block(std::span<std::int16_t> buf, std::span<std::int16_t> idwt, std::size_t sw) noexcept
{
    const std::size_t total = sw * 2;
    const std::size_t band = sw * sw;
    // Horizontal: LL + HL -> L, LH + HH -> H.
    for (std::size_t y = 0; y < sw; ++y) {
        const auto hl = buf.subspan(y * sw, sw);
        const auto lh = buf.subspan(band + (y * sw), sw);
        const auto hh = buf.subspan((2 * band) + (y * sw), sw);
        const auto ll = buf.subspan((3 * band) + (y * sw), sw);
        const auto l_dst = idwt.subspan(y * total, total);
        const auto h_dst = idwt.subspan((2 * band) + (y * total), total);
        // Even coefficients.
        l_dst[0] = to_i16(ll[0] - ((hl[0] + hl[0] + 1) >> 1));
        h_dst[0] = to_i16(lh[0] - ((hh[0] + hh[0] + 1) >> 1));
        for (std::size_t n = 1; n < sw; ++n) {
            l_dst[n * 2] = to_i16(ll[n] - ((hl[n - 1] + hl[n] + 1) >> 1));
            h_dst[n * 2] = to_i16(lh[n] - ((hh[n - 1] + hh[n] + 1) >> 1));
        }
        // Odd coefficients.
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
    // Vertical: L + H -> output.
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

// ---------------------------------------------------------------------------
// Reduce-extrapolate IDWT (FreeRDP progressive.c progressive_rfx_idwt_x/y)

/// Strided access into a span: element i of a line at `base` with `step`.
struct Line {
    std::span<std::int16_t> data;
    std::size_t base = 0;
    std::size_t step = 1;
    [[nodiscard]] std::int16_t& operator[](std::size_t i) const { return data[base + (i * step)]; }
};

/// Inverse lifting on one line of `low` (low_count) and `high` (high_count)
/// coefficients into `dst`.
void idwt_line(const Line& low, const Line& high, const Line& dst, std::size_t low_count,
               std::size_t high_count) noexcept
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

[[nodiscard]] constexpr std::size_t band_l_count(std::size_t level) noexcept
{
    return (64U >> level) + 1;
}

[[nodiscard]] constexpr std::size_t band_h_count(std::size_t level) noexcept
{
    return level == 1 ? (64U >> 1U) - 1 : (64U + (1U << (level - 1))) >> level;
}

void dwt_decode_extrapolate_block(std::span<std::int16_t> buf, std::span<std::int16_t> temp, std::size_t level) noexcept
{
    const std::size_t nl = band_l_count(level);
    const std::size_t nh = band_h_count(level);
    const std::size_t step = nl + nh;
    const std::size_t hl = 0;
    const std::size_t lh = hl + (nh * nl);
    const std::size_t hh = lh + (nl * nh);
    const std::size_t ll = hh + (nh * nh);
    const std::size_t l = 0;
    const std::size_t h = nl * step;
    // Horizontal (LL + HL -> L), nl rows.
    for (std::size_t i = 0; i < nl; ++i) {
        idwt_line({buf, ll + (i * nl), 1}, {buf, hl + (i * nh), 1}, {temp, l + (i * step), 1}, nl, nh);
    }
    // Horizontal (LH + HH -> H), nh rows.
    for (std::size_t i = 0; i < nh; ++i) {
        idwt_line({buf, lh + (i * nl), 1}, {buf, hh + (i * nh), 1}, {temp, h + (i * step), 1}, nl, nh);
    }
    // Vertical (L + H -> LL), one column at a time.
    for (std::size_t i = 0; i < step; ++i) {
        idwt_line({temp, l + i, step}, {temp, h + i, step}, {buf, i, step}, nl, nh);
    }
}

}  // namespace

void dwt_encode(Coefficients& data, Coefficients& scratch) noexcept
{
    const std::span<std::int16_t> buf(data);
    dwt_encode_block(buf, scratch, 32);
    dwt_encode_block(buf.subspan(3072), scratch, 16);
    dwt_encode_block(buf.subspan(3840), scratch, 8);
}

void dwt_decode(Coefficients& data, Coefficients& scratch) noexcept
{
    const std::span<std::int16_t> buf(data);
    dwt_decode_block(buf.subspan(3840), scratch, 8);
    dwt_decode_block(buf.subspan(3072), scratch, 16);
    dwt_decode_block(buf, scratch, 32);
}

void dwt_decode_extrapolate(Coefficients& data, Coefficients& scratch) noexcept
{
    const std::span<std::int16_t> buf(data);
    dwt_decode_extrapolate_block(buf.subspan(3807), scratch, 3);
    dwt_decode_extrapolate_block(buf.subspan(3007), scratch, 2);
    dwt_decode_extrapolate_block(buf, scratch, 1);
}

// ---------------------------------------------------------------------------
// Quantization (FreeRDP rfx_quantization.c, progressive.c)

namespace {

void quantize_block(std::span<std::int16_t> block, std::uint32_t factor) noexcept
{
    if (factor == 0) {
        return;
    }
    const std::int32_t half = std::int32_t{1} << (factor - 1);
    for (auto& v : block) {
        v = to_i16((v + half) >> factor);
    }
}

}  // namespace

void quantize(Coefficients& data, const Quant& quant) noexcept
{
    const std::span<std::int16_t> buf(data);
    for (std::size_t b = 0; b < band_count; ++b) {
        FARLAND_ASSERT(quant.bands[b] >= 6 && quant.bands[b] <= 15);
        const auto& band = standard_layout[b];
        quantize_block(buf.subspan(band.offset, band.size), quant.bands[b] - 6U);
    }
    // The colour transform scaled everything by 32 (11.5 fixed point).
    quantize_block(buf, 5);
}

bool dequantize(Coefficients& data, const Layout& layout, const Quant& shift) noexcept
{
    const std::span<std::int16_t> buf(data);
    for (std::size_t b = 0; b < band_count; ++b) {
        const std::uint32_t s = shift.bands[b];
        if (s == 0) {
            continue;
        }
        if (s >= 16) {
            return false;
        }
        const auto& band = layout[b];
        for (auto& v : buf.subspan(band.offset, band.size)) {
            v = to_i16(v * (std::int32_t{1} << s));
        }
    }
    return true;
}

void differential_encode(std::span<std::int16_t> band) noexcept
{
    for (std::size_t i = band.size(); i-- > 1;) {
        band[i] = to_i16(band[i] - band[i - 1]);
    }
}

void differential_decode(std::span<std::int16_t> band) noexcept
{
    for (std::size_t i = 1; i < band.size(); ++i) {
        band[i] = to_i16(band[i - 1] + band[i]);
    }
}

// ---------------------------------------------------------------------------
// Bit streams

std::uint32_t BitReader::peek32() const noexcept
{
    const std::size_t byte = position_ / 8;
    const auto shift = static_cast<std::uint32_t>(position_ % 8);
    std::uint64_t window = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        window <<= 8U;
        if (byte < data_.size() && i < data_.size() - byte) {
            window |= std::to_integer<std::uint64_t>(data_[byte + i]);
        }
    }
    return static_cast<std::uint32_t>(window >> (8U - shift));
}

std::uint32_t BitReader::read(std::uint32_t count) noexcept
{
    FARLAND_ASSERT(count < 32);
    if (count == 0) {
        return 0;
    }
    const std::uint32_t value = peek32() >> (32U - count);
    position_ += count;
    return value;
}

void BitWriter::put(std::uint32_t bits, std::uint32_t count)
{
    FARLAND_ASSERT(count <= 32);
    if (count == 0) {
        return;
    }
    const std::uint64_t mask = (std::uint64_t{1} << count) - 1;
    acc_ = (acc_ << count) | (bits & mask);
    pending_ += count;
    while (pending_ >= 8) {
        pending_ -= 8;
        out_->push_back(static_cast<std::byte>(static_cast<std::uint8_t>(acc_ >> pending_)));
    }
    acc_ &= (std::uint64_t{1} << pending_) - 1;
}

void BitWriter::put_repeated(bool bit, std::size_t count)
{
    const std::uint32_t pattern = bit ? 0xFFFFFFFFU : 0;
    while (count >= 32) {
        put(pattern, 32);
        count -= 32;
    }
    put(pattern, static_cast<std::uint32_t>(count));
}

void BitWriter::flush()
{
    if (pending_ > 0) {
        put(0, 8 - pending_);
    }
}

// ---------------------------------------------------------------------------
// RLGR (FreeRDP rfx_rlgr.c, [MS-RDPRFX] 3.1.8.1.7.3)

namespace {

constexpr std::uint32_t kp_max = 80;  // KPMAX: largest kp or krp
constexpr std::uint32_t lsgr = 3;     // LSGR: kp >> LSGR = k
constexpr std::int32_t up_gr = 4;     // UP_GR: kp increase after a zero run in RL mode
constexpr std::int32_t dn_gr = 6;     // DN_GR: kp decrease after a nonzero symbol in RL mode
constexpr std::int32_t uq_gr = 3;     // UQ_GR: kp increase after a zero symbol in GR mode
constexpr std::int32_t dq_gr = 3;     // DQ_GR: kp decrease after a nonzero symbol in GR mode

/// UpdateParam: adds `delta`, clamps to [0, KPMAX], returns param >> LSGR.
std::uint32_t update_param(std::uint32_t& param, std::int32_t delta) noexcept
{
    const std::int64_t v = std::clamp<std::int64_t>(std::int64_t{param} + delta, 0, kp_max);
    param = static_cast<std::uint32_t>(v);
    return param >> lsgr;
}

/// Golomb-Rice code of a non-negative value (rfx_rlgr_code_gr).
void code_gr(BitWriter& bs, std::uint32_t& krp, std::uint32_t val)
{
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
}

[[nodiscard]] constexpr std::uint32_t two_mag_sign(std::int32_t input) noexcept
{
    return input >= 0 ? static_cast<std::uint32_t>(2 * input) : static_cast<std::uint32_t>((-2 * input) - 1);
}

/// Counts consecutive bits equal to `bit` from the current position, at most
/// the bits remaining, and consumes them (the lzcnt loops of rfx_rlgr_decode).
std::size_t count_run(BitReader& bs, bool bit) noexcept
{
    std::size_t count = 0;
    for (;;) {
        const std::size_t remaining = bs.remaining();
        if (remaining == 0) {
            break;
        }
        const std::uint32_t word = bit ? ~bs.peek32() : bs.peek32();
        const auto run = static_cast<std::size_t>(std::countl_zero(word));
        const std::size_t take = std::min(run, remaining);
        bs.skip(take);
        count += take;
        if (take < 32) {
            break;
        }
    }
    return count;
}

}  // namespace

void rlgr_encode(RlgrMode mode, std::span<const std::int16_t> data, std::vector<std::byte>& out)
{
    BitWriter bs(out);
    std::uint32_t k = 1;
    std::uint32_t kp = 1U << lsgr;
    std::uint32_t krp = 1U << lsgr;
    std::size_t idx = 0;
    // GetNextInput: zeros once the input is exhausted.
    const auto next = [&]() -> std::int32_t { return idx < data.size() ? data[idx++] : 0; };

    while (idx < data.size()) {
        if (k != 0) {
            // Run-length mode: a run of zeros, then the value that ends it.
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

Result<void> rlgr_decode(RlgrMode mode, std::span<const std::byte> src, std::span<std::int16_t> out) noexcept
{
    if (src.empty()) {
        return fail(Errc::invalid_length, "RLGR data is empty");
    }
    BitReader bs(src);
    std::uint32_t k = 1;
    std::uint32_t kp = k << lsgr;
    std::uint32_t kr = 1;
    std::uint32_t krp = kr << lsgr;
    std::size_t written = 0;
    const auto emit = [&](std::int16_t v) {
        if (written < out.size()) {
            out[written++] = v;
        }
    };
    // The kr parameter update shared by both modes.
    const auto update_kr = [&](std::size_t vk) {
        if (vk == 0) {
            krp = krp > 2 ? krp - 2 : 0;
        } else if (vk != 1) {
            krp = static_cast<std::uint32_t>(std::min<std::size_t>(krp + vk, kp_max));
        }
        kr = krp >> lsgr;
    };

    while (bs.remaining() > 0 && written < out.size()) {
        if (k != 0) {
            // Run-length mode.
            std::size_t vk = count_run(bs, false);
            if (bs.remaining() < 1) {
                break;
            }
            bs.skip(1);
            std::size_t run = 0;
            for (; vk > 0; --vk) {
                run += std::size_t{1} << k;
                kp = std::min(kp + up_gr, kp_max);
                k = kp >> lsgr;
            }
            if (bs.remaining() < k) {
                break;
            }
            run += bs.read(k);
            if (bs.remaining() < 1) {
                break;
            }
            const bool sign = bs.read(1) != 0;
            vk = count_run(bs, true);
            if (bs.remaining() < 1) {
                break;
            }
            bs.skip(1);
            if (bs.remaining() < kr) {
                break;
            }
            // `code` is 16 bits wide in FreeRDP.
            const auto code = static_cast<std::uint16_t>(bs.read(kr) | static_cast<std::uint32_t>(vk << kr));
            update_kr(vk);
            kp = kp > static_cast<std::uint32_t>(dn_gr) ? kp - dn_gr : 0;
            k = kp >> lsgr;
            const std::int32_t mag = std::int32_t{code} + 1;
            const std::size_t zeros = std::min(run, out.size() - written);
            std::fill_n(out.subspan(written).begin(), zeros, std::int16_t{0});
            written += zeros;
            emit(to_i16(sign ? -mag : mag));
        } else {
            // Golomb-Rice mode.
            const std::size_t vk = count_run(bs, true);
            if (bs.remaining() < 1) {
                break;
            }
            bs.skip(1);
            if (bs.remaining() < kr) {
                break;
            }
            const auto code = static_cast<std::uint16_t>(bs.read(kr) | static_cast<std::uint32_t>(vk << kr));
            update_kr(vk);
            if (mode == RlgrMode::rlgr1) {
                std::int32_t mag = 0;
                if (code == 0) {
                    kp = std::min(kp + uq_gr, kp_max);
                } else {
                    kp = kp > static_cast<std::uint32_t>(dq_gr) ? kp - dq_gr : 0;
                    // code = 2 * mag - sign
                    mag = (code & 1U) != 0 ? -((code + 1) >> 1) : code >> 1;
                }
                k = kp >> lsgr;
                emit(to_i16(mag));
            } else {
                std::uint32_t n_idx = 0;
                if (code != 0) {
                    n_idx = static_cast<std::uint32_t>(std::bit_width(static_cast<std::uint32_t>(to_i16(code))));
                }
                if (bs.remaining() < n_idx) {
                    break;
                }
                const std::uint32_t val1 = n_idx < 32 ? bs.read(n_idx) : 0;
                const std::uint32_t val2 = std::uint32_t{code} - val1;
                if (val1 != 0 && val2 != 0) {
                    kp = kp > 2U * dq_gr ? kp - (2U * dq_gr) : 0;
                } else if (val1 == 0 && val2 == 0) {
                    kp = std::min(kp + (2U * uq_gr), kp_max);
                }
                k = kp >> lsgr;
                const auto value = [](std::uint32_t v) {
                    return (v & 1U) != 0 ? to_i16(-static_cast<std::int32_t>((v + 1) >> 1))
                                         : to_i16(static_cast<std::int32_t>(v >> 1));
                };
                emit(value(val1));
                emit(value(val2));
            }
        }
    }
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(written), out.end(), std::int16_t{0});
    return {};
}

// ---------------------------------------------------------------------------
// Upgrade passes (FreeRDP progressive.c)

std::int16_t srl_read(UpgradeState& state, std::uint32_t num_bits) noexcept
{
    FARLAND_ASSERT(num_bits >= 1 && num_bits < 32);
    BitReader& bs = state.srl;
    if (state.nz > 0) {
        --state.nz;
        return 0;
    }
    const std::uint32_t k = state.kp / 8;
    if (!state.unary) {
        // Zero run-length ([MS-RDPEGFX] 3.1.8.1.5.1).
        if (bs.read(1) == 0) {
            // A full run of 1 << k zeros.
            state.nz = (1U << k) - 1;
            state.kp = std::min(state.kp + 4, kp_max);
            return 0;
        }
        // A shorter run of the next k bits, then a nonzero value.
        state.nz = bs.read(k);
        state.unary = true;
        if (state.nz > 0) {
            --state.nz;
            return 0;
        }
    }
    // Unary-coded nonzero value ([MS-RDPEGFX] 3.1.8.1.5.2).
    state.unary = false;
    const bool sign = bs.read(1) != 0;
    state.kp = state.kp < 6 ? 0 : state.kp - 6;
    if (num_bits == 1) {
        return sign ? -1 : 1;
    }
    std::uint32_t mag = 1;
    const std::uint32_t max = (1U << num_bits) - 1;
    while (mag < max) {
        if (bs.remaining() == 0) {
            // Past the end every bit is 0: the loop would run to `max`.
            bs.skip(max - mag);
            mag = max;
            break;
        }
        if (bs.read(1) != 0) {
            break;
        }
        ++mag;
    }
    const auto magnitude = static_cast<std::int32_t>(std::min<std::uint32_t>(mag, INT16_MAX));
    return static_cast<std::int16_t>(sign ? -magnitude : magnitude);
}

void upgrade_band(UpgradeState& state, std::span<std::int16_t> current, std::span<std::int16_t> sign,
                  std::uint32_t shift, std::uint32_t num_bits, bool non_ll) noexcept
{
    FARLAND_ASSERT(current.size() == sign.size());
    if (num_bits < 1) {
        return;
    }
    FARLAND_ASSERT(num_bits < 32);
    const auto add = [&](std::size_t i, std::int64_t input) {
        const std::int64_t shifted = input * (std::int64_t{1} << std::min<std::uint32_t>(shift, 32));
        current[i] = static_cast<std::int16_t>(static_cast<std::uint16_t>(current[i] + shifted));
    };
    // FreeRDP's rawShift returns int16_t, so raw values of 16 bits wrap.
    const auto raw = [&] { return static_cast<std::int16_t>(state.raw.read(num_bits)); };
    for (std::size_t i = 0; i < current.size(); ++i) {
        if (!non_ll || sign[i] > 0) {
            // LL3 elements, and elements already known to be positive.
            add(i, raw());
        } else if (sign[i] < 0) {
            add(i, -std::int64_t{raw()});
        } else {
            const std::int16_t input = srl_read(state, num_bits);
            sign[i] = input;
            add(i, input);
        }
    }
}

}  // namespace farland::codec::rfx
