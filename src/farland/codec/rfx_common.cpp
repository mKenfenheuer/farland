// SPDX-FileCopyrightText: 2011 Vic Lee
// SPDX-FileCopyrightText: 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
// SPDX-FileCopyrightText: 2019 Armin Novak <armin.novak@thincast.com>
// SPDX-FileCopyrightText: 2019 Thincast Technologies GmbH
// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// RemoteFX primitives, [MS-RDPRFX] 3.1.8 and [MS-RDPEGFX] 3.1.8.1.
//
// Translated from FreeRDP 3 (Apache-2.0): libfreerdp/codec/rfx_quantization.c,
// rfx_rlgr.c, rfx_differential.h, rfx_bitstream.h, progressive.c (SRL/RAW
// upgrade reads) and libfreerdp/primitives/prim_colors.c. Modified: rewritten
// over std::span with bounds-checked indexing, wrapping casts instead of
// assertions, and a position-based bit reader equivalent to winpr's
// wBitStream. The encoder side of upgrade passes (SRL and RAW writing) is new;
// FreeRDP has none. The encoder's hot paths are in rfx_kernels.cpp.

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
// Colour conversion (load_tile is in rfx_kernels.cpp)

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
// Dequantization and differential coding (FreeRDP rfx_quantization.c,
// progressive.c, rfx_differential.h)

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
    if (byte < data_.size() && data_.size() - byte >= 5) {
        const auto b = data_.subspan(byte, 5);
        window = (std::to_integer<std::uint64_t>(b[0]) << 32U) | (std::to_integer<std::uint64_t>(b[1]) << 24U) |
                 (std::to_integer<std::uint64_t>(b[2]) << 16U) | (std::to_integer<std::uint64_t>(b[3]) << 8U) |
                 std::to_integer<std::uint64_t>(b[4]);
    } else {
        for (std::size_t i = 0; i < 5; ++i) {
            window <<= 8U;
            if (byte < data_.size() && i < data_.size() - byte) {
                window |= std::to_integer<std::uint64_t>(data_[byte + i]);
            }
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
// RLGR decoding (FreeRDP rfx_rlgr.c, [MS-RDPRFX] 3.1.8.1.7.3; the encoder is
// in rfx_kernels.cpp)

namespace {

constexpr std::uint32_t kp_max = 80;  // KPMAX: largest kp or krp
constexpr std::uint32_t lsgr = 3;     // LSGR: kp >> LSGR = k
constexpr std::int32_t up_gr = 4;     // UP_GR: kp increase after a zero run in RL mode
constexpr std::int32_t dn_gr = 6;     // DN_GR: kp decrease after a nonzero symbol in RL mode
constexpr std::int32_t uq_gr = 3;     // UQ_GR: kp increase after a zero symbol in GR mode
constexpr std::int32_t dq_gr = 3;     // DQ_GR: kp decrease after a nonzero symbol in GR mode

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
// Upgrade passes, decoder side (FreeRDP progressive.c)

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

// ---------------------------------------------------------------------------
// Upgrade passes, encoder side ([MS-RDPEGFX] 3.2.8.1.5.2 and 3.1.8.1.5): the
// exact mirror of srl_read and upgrade_band.

namespace {

/// Zero runs of the SRL stream, as srl_read consumes them: a 0 bit for each
/// full run of 2^k zeros (k = kp / 8, kp growing by 4), then a 1 bit and the
/// rest in k bits.
void srl_put_run(BitWriter& bits, std::uint32_t& kp, std::uint32_t zeros)
{
    std::uint32_t k = kp / 8;
    while (zeros >= (1U << k)) {
        bits.put(0, 1);
        zeros -= 1U << k;
        kp = std::min(kp + 4, kp_max);
        k = kp / 8;
    }
    bits.put(1, 1);
    bits.put(zeros, k);
}

}  // namespace

void upgrade_encode_band(std::span<const std::int16_t> sb, std::uint32_t from, std::uint32_t to, BitWriter& srl_bits,
                         BitWriter& raw_bits, std::uint32_t& kp, std::uint32_t& zeros)
{
    FARLAND_ASSERT(from > to && from - to < 16);
    const std::uint32_t num_bits = from - to;
    const std::uint32_t max = (1U << num_bits) - 1;
    for (const std::int16_t v : sb) {
        const auto magnitude = static_cast<std::uint32_t>(v < 0 ? -std::int32_t{v} : std::int32_t{v});
        if ((magnitude >> from) != 0) {
            // The decoder has a nonzero value, so it knows the sign: the next
            // bits of the magnitude go out raw.
            raw_bits.put((magnitude >> to) & max, num_bits);
            continue;
        }
        const std::uint32_t q = magnitude >> to;  // at most `max`
        if (q == 0) {
            ++zeros;
            continue;
        }
        srl_put_run(srl_bits, kp, zeros);
        zeros = 0;
        // Unary magnitude: sign, q - 1 zeros, and a 1 unless q is the largest.
        srl_bits.put(v < 0 ? 1 : 0, 1);
        kp = kp < 6 ? 0 : kp - 6;
        if (num_bits > 1) {
            srl_bits.put_repeated(false, q - 1);
            if (q < max) {
                srl_bits.put(1, 1);
            }
        }
    }
}

void upgrade_encode_finish(BitWriter& srl_bits, std::vector<std::byte>& srl, std::uint32_t& kp, std::uint32_t& zeros)
{
    // Trailing zeros as full runs; the last may reach past the band's end,
    // which the decoder never reads.
    while (zeros > 0) {
        const std::uint32_t run = 1U << (kp / 8);
        srl_bits.put(0, 1);
        zeros = zeros > run ? zeros - run : 0;
        kp = std::min(kp + 4, kp_max);
    }
    srl_bits.flush();
    if (!srl.empty()) {
        srl.push_back(std::byte{0});
    }
}

}  // namespace farland::codec::rfx
