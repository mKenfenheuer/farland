// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// ZGFX (RDP 8.0 bulk compression), [MS-RDPEGFX] 2.2.5 and 3.1.9.1, and its
// RDP 8.0 Lite variant, [MS-RDPEDYC] 2.2.3.3.
//
// Written independently from the specification. Where the specification
// leaves room, the decoder follows FreeRDP's libfreerdp/codec/zgfx.c
// (Apache-2.0): raw segments enter the history like decoded bytes, a match may
// overlap its own output, and the token table is searched as one prefix code.

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>
#include <farland/codec/zgfx.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace farland::codec {

namespace {

// ---------------------------------------------------------------------------
// Token code, [MS-RDPEGFX] 3.1.9.1.2.3

/// One row of the token table: a prefix of `prefix_length` bits, followed by
/// `value_bits` bits that are added to `base`. A literal token yields the
/// byte `base + value`; a match token yields the distance `base + value`,
/// where distance 0 introduces an unencoded run.
struct Token {
    std::uint8_t prefix_length = 0;
    std::uint16_t prefix = 0;
    std::uint8_t value_bits = 0;
    bool match = false;
    std::uint32_t base = 0;
};

constexpr std::size_t match_token_first = 1;
constexpr std::size_t match_token_count = 11;

// The table of [MS-RDPEGFX] 3.1.9.1.2.3. FreeRDP also knows three 9-bit match
// tokens (101111100 to 101111110) for distances above 4,511,391; they are
// reserved in the specification and could only point beyond the history, so
// they decode as reserved here.
constexpr std::array tokens{
    Token{.prefix_length = 1, .prefix = 0b0, .value_bits = 8, .match = false, .base = 0},  // literal xxxxxxxx
    // Match distances, in increasing order (match_token_first .. + match_token_count).
    Token{.prefix_length = 5, .prefix = 0b10001, .value_bits = 5, .match = true, .base = 0},  // 0 = unencoded run
    Token{.prefix_length = 5, .prefix = 0b10010, .value_bits = 7, .match = true, .base = 32},
    Token{.prefix_length = 5, .prefix = 0b10011, .value_bits = 9, .match = true, .base = 160},
    Token{.prefix_length = 5, .prefix = 0b10100, .value_bits = 10, .match = true, .base = 672},
    Token{.prefix_length = 5, .prefix = 0b10101, .value_bits = 12, .match = true, .base = 1696},
    Token{.prefix_length = 6, .prefix = 0b101100, .value_bits = 14, .match = true, .base = 5792},
    Token{.prefix_length = 6, .prefix = 0b101101, .value_bits = 15, .match = true, .base = 22176},
    Token{.prefix_length = 7, .prefix = 0b1011100, .value_bits = 18, .match = true, .base = 54944},
    Token{.prefix_length = 7, .prefix = 0b1011101, .value_bits = 20, .match = true, .base = 317088},
    Token{.prefix_length = 8, .prefix = 0b10111100, .value_bits = 20, .match = true, .base = 1365664},
    Token{.prefix_length = 8, .prefix = 0b10111101, .value_bits = 21, .match = true, .base = 2414240},
    // Literals with short codes; their 9-bit forms are reserved for encoders.
    Token{.prefix_length = 5, .prefix = 0b11000, .value_bits = 0, .match = false, .base = 0x00},
    Token{.prefix_length = 5, .prefix = 0b11001, .value_bits = 0, .match = false, .base = 0x01},
    Token{.prefix_length = 6, .prefix = 0b110100, .value_bits = 0, .match = false, .base = 0x02},
    Token{.prefix_length = 6, .prefix = 0b110101, .value_bits = 0, .match = false, .base = 0x03},
    Token{.prefix_length = 6, .prefix = 0b110110, .value_bits = 0, .match = false, .base = 0xFF},
    Token{.prefix_length = 7, .prefix = 0b1101110, .value_bits = 0, .match = false, .base = 0x04},
    Token{.prefix_length = 7, .prefix = 0b1101111, .value_bits = 0, .match = false, .base = 0x05},
    Token{.prefix_length = 7, .prefix = 0b1110000, .value_bits = 0, .match = false, .base = 0x06},
    Token{.prefix_length = 7, .prefix = 0b1110001, .value_bits = 0, .match = false, .base = 0x07},
    Token{.prefix_length = 7, .prefix = 0b1110010, .value_bits = 0, .match = false, .base = 0x08},
    Token{.prefix_length = 7, .prefix = 0b1110011, .value_bits = 0, .match = false, .base = 0x09},
    Token{.prefix_length = 7, .prefix = 0b1110100, .value_bits = 0, .match = false, .base = 0x0A},
    Token{.prefix_length = 7, .prefix = 0b1110101, .value_bits = 0, .match = false, .base = 0x0B},
    Token{.prefix_length = 7, .prefix = 0b1110110, .value_bits = 0, .match = false, .base = 0x3A},
    Token{.prefix_length = 7, .prefix = 0b1110111, .value_bits = 0, .match = false, .base = 0x3B},
    Token{.prefix_length = 7, .prefix = 0b1111000, .value_bits = 0, .match = false, .base = 0x3C},
    Token{.prefix_length = 7, .prefix = 0b1111001, .value_bits = 0, .match = false, .base = 0x3D},
    Token{.prefix_length = 7, .prefix = 0b1111010, .value_bits = 0, .match = false, .base = 0x3E},
    Token{.prefix_length = 7, .prefix = 0b1111011, .value_bits = 0, .match = false, .base = 0x3F},
    Token{.prefix_length = 7, .prefix = 0b1111100, .value_bits = 0, .match = false, .base = 0x40},
    Token{.prefix_length = 7, .prefix = 0b1111101, .value_bits = 0, .match = false, .base = 0x80},
    Token{.prefix_length = 8, .prefix = 0b11111100, .value_bits = 0, .match = false, .base = 0x0C},
    Token{.prefix_length = 8, .prefix = 0b11111101, .value_bits = 0, .match = false, .base = 0x38},
    Token{.prefix_length = 8, .prefix = 0b11111110, .value_bits = 0, .match = false, .base = 0x39},
    Token{.prefix_length = 8, .prefix = 0b11111111, .value_bits = 0, .match = false, .base = 0x66},
};

constexpr std::span<const Token> match_tokens = std::span(tokens).subspan<match_token_first, match_token_count>();

/// Longest token prefix; the decoder looks up this many bits at once.
constexpr unsigned lookup_bits = 9;
constexpr std::uint8_t reserved_token = 0xFF;

/// Maps the next `lookup_bits` bits of the stream to the index of the token
/// whose prefix they start with, or reserved_token.
constexpr std::array<std::uint8_t, std::size_t{1} << lookup_bits> build_token_lookup()
{
    std::array<std::uint8_t, std::size_t{1} << lookup_bits> lookup{};
    lookup.fill(reserved_token);
    for (std::size_t t = 0; t < tokens.size(); ++t) {
        const unsigned free_bits = lookup_bits - tokens[t].prefix_length;
        const std::size_t first = std::size_t{tokens[t].prefix} << free_bits;
        std::fill_n(std::span(lookup).subspan(first).begin(), std::size_t{1} << free_bits,
                    static_cast<std::uint8_t>(t));
    }
    return lookup;
}

/// Whether the prefixes form a prefix code (no prefix starts another).
constexpr bool is_prefix_code()
{
    for (const Token& a : tokens) {
        for (const Token& b : tokens) {
            if (&a != &b && a.prefix_length <= b.prefix_length &&
                (b.prefix >> (b.prefix_length - a.prefix_length)) == a.prefix) {
                return false;
            }
        }
    }
    return true;
}

static_assert(is_prefix_code());

constexpr auto token_lookup = build_token_lookup();

// The prefixes left over are reserved: 10000xxxx and 1011111xx.
static_assert(std::ranges::count(token_lookup, reserved_token) == 16 + 4);

struct Code {
    std::uint32_t bits = 0;
    unsigned length = 0;

    friend constexpr bool operator==(const Code&, const Code&) = default;
};

/// The code of each literal: its short token, or 0 followed by the byte.
constexpr std::array<Code, 256> build_literal_codes()
{
    std::array<Code, 256> codes{};
    for (std::size_t b = 0; b < codes.size(); ++b) {
        codes[b] = {.bits = static_cast<std::uint32_t>(b), .length = 9};
    }
    for (const Token& t : tokens) {
        if (!t.match && t.value_bits == 0) {
            codes[t.base] = {.bits = t.prefix, .length = t.prefix_length};
        }
    }
    return codes;
}

constexpr auto literal_codes = build_literal_codes();

static_assert(literal_codes[0x49] == Code{.bits = 0b001001001, .length = 9});
static_assert(literal_codes[0x00] == Code{.bits = 0b11000, .length = 5});
static_assert(literal_codes[0x66] == Code{.bits = 0b11111111, .length = 8});

/// The match length code after a match distance ([MS-RDPEGFX] 3.1.9.1.2.3):
/// 0 for length 3, otherwise k ones and a zero followed by k + 1 bits of
/// length - 2^(k + 1), for lengths 2^(k + 1) .. 2^(k + 2) - 1, k = 1 .. 14.
constexpr std::size_t min_match = 3;
constexpr std::size_t max_match = 65535;
constexpr unsigned max_length_ones = 14;

constexpr Code length_code(std::size_t length)
{
    if (length == min_match) {
        return {.bits = 0, .length = 1};
    }
    const auto k = static_cast<unsigned>(std::bit_width(length)) - 2U;
    const std::uint32_t prefix = ((1U << k) - 1U) << 1U;
    const auto value = static_cast<std::uint32_t>(length - (std::size_t{1} << (k + 1U)));
    return {.bits = (prefix << (k + 1U)) | value, .length = (2U * k) + 2U};
}

// [MS-RDPEGFX] 3.1.9.1.2.5 and [MS-RDPEDYC] 4.3.3.
static_assert(length_code(13) == Code{.bits = 0b110101, .length = 6});
static_assert(length_code(1594) == Code{.bits = 0b1111111110'1000111010, .length = 20});
static_assert(length_code(max_match).length == 30);

struct DistanceCode {
    Code prefix;
    Code value;
};

constexpr DistanceCode distance_code(std::uint32_t distance)
{
    std::size_t t = match_tokens.size() - 1;
    while (match_tokens[t].base > distance) {
        --t;
    }
    const Token& token = match_tokens[t];
    return {.prefix = {.bits = token.prefix, .length = token.prefix_length},
            .value = {.bits = distance - token.base, .length = token.value_bits}};
}

static_assert(distance_code(44).prefix.bits == 0b10010 && distance_code(44).value.bits == 12);
static_assert(distance_code(2'500'000).prefix.bits == 0b10111101);

// ---------------------------------------------------------------------------
// Variants

struct Params {
    std::size_t history = 0;
    std::size_t max_segment = 0;
    std::uint8_t type = 0;
};

constexpr Params params(ZgfxVariant variant)
{
    if (variant == ZgfxVariant::rdp8_lite) {
        return {.history = zgfx::lite_history_size,
                .max_segment = zgfx::lite_max_segment_size,
                .type = zgfx::packet_compr_type_rdp8_lite};
    }
    return {.history = zgfx::history_size, .max_segment = zgfx::max_segment_size, .type = zgfx::packet_compr_type_rdp8};
}

// ---------------------------------------------------------------------------
// Bit I/O, most significant bit first ([MS-RDPEGFX] 3.1.9.1.2.3)

class BitWriter {
public:
    explicit BitWriter(std::vector<std::byte>& out) : out_(&out) {}

    /// Appends the low `count` (<= 32) bits of `value`.
    void put(std::uint32_t value, unsigned count)
    {
        acc_ = (acc_ << count) | value;
        pending_ += count;
        while (pending_ >= 8) {
            pending_ -= 8;
            out_->push_back(static_cast<std::byte>(static_cast<std::uint8_t>(acc_ >> pending_)));
        }
    }

    void put(Code code) { put(code.bits, code.length); }

    /// Bits written into the current, incomplete byte.
    [[nodiscard]] unsigned pending_bits() const { return pending_; }

    void align()
    {
        if (pending_ != 0) {
            put(0, 8 - pending_);
        }
    }

    /// Appends whole bytes; the stream must be byte-aligned.
    void bytes(std::span<const std::byte> data)
    {
        FARLAND_ASSERT(pending_ == 0);
        out_->insert(out_->end(), data.begin(), data.end());
    }

    /// Pads the last byte and appends the trailer: the number of padding bits
    /// ([MS-RDPEGFX] 3.1.9.1.2.4).
    void finish()
    {
        const unsigned unused = (8 - pending_) % 8;
        align();
        out_->push_back(static_cast<std::byte>(unused));
    }

private:
    std::vector<std::byte>* out_;
    std::uint64_t acc_ = 0;
    unsigned pending_ = 0;
};

/// Reads the bit stream of one compressed segment: exactly `bit_count` bits
/// from `bytes`, which holds at least that many.
class BitReader {
public:
    BitReader(Reader bytes, std::size_t bit_count) : r_(bytes), remaining_(bit_count)
    {
        FARLAND_ASSERT(bit_count <= bytes.size() * 8);
    }

    [[nodiscard]] std::size_t remaining() const { return remaining_; }

    /// The next `count` (1..32) bits without consuming them. Past the end of
    /// the data they read as zeros; the caller then fails on consuming them.
    [[nodiscard]] std::uint32_t peek(unsigned count)
    {
        while (buffered_ < count && !r_.empty()) {
            const auto b = r_.u8();
            acc_ |= std::uint64_t{*b} << (56U - buffered_);
            buffered_ += 8;
        }
        return static_cast<std::uint32_t>(acc_ >> (64U - count));
    }

    [[nodiscard]] Result<void> skip(unsigned count)
    {
        if (count > remaining_) {
            return fail(Errc::truncated, "ZGFX token runs past the end of the bit stream", r_.offset());
        }
        FARLAND_ASSERT(count <= buffered_);  // peek() buffered them: remaining_ <= what the data holds.
        acc_ <<= count;
        buffered_ -= count;
        remaining_ -= count;
        return {};
    }

    [[nodiscard]] Result<std::uint32_t> read(unsigned count)
    {
        if (count == 0) {
            return 0U;
        }
        const std::uint32_t value = peek(count);
        FARLAND_TRY_VOID(skip(count));
        return value;
    }

    /// Drops the rest of the current byte and appends the next `count` whole
    /// bytes to `out` (an unencoded run, [MS-RDPEGFX] 3.1.9.1.2.3).
    [[nodiscard]] Result<void> read_aligned(std::size_t count, std::vector<std::byte>& out)
    {
        const unsigned drop = buffered_ % 8;
        acc_ <<= drop;
        buffered_ -= drop;
        remaining_ -= std::min<std::size_t>(drop, remaining_);
        if (count > remaining_ / 8) {
            return fail(Errc::truncated, "ZGFX unencoded run runs past the end of the bit stream", r_.offset());
        }
        remaining_ -= count * 8;
        for (; count > 0 && buffered_ >= 8; --count) {
            out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(acc_ >> 56U)));
            acc_ <<= 8U;
            buffered_ -= 8;
        }
        FARLAND_TRY(const auto run, r_.bytes(count));
        out.insert(out.end(), run.begin(), run.end());
        return {};
    }

    [[nodiscard]] std::size_t offset() const { return r_.offset(); }

private:
    Reader r_;
    std::uint64_t acc_ = 0;  // Buffered bits, left-aligned.
    unsigned buffered_ = 0;
    std::size_t remaining_ = 0;
};

// ---------------------------------------------------------------------------
// Match finding

// Hash chains over 3-byte prefixes. Greedy parsing with a bounded chain walk;
// after a run of positions without a match the search skips ahead faster
// (as LZ4 does), which keeps incompressible data (H.264, most RLGR) fast.
constexpr unsigned hash_bits_rdp8 = 16;
constexpr unsigned hash_bits_lite = 12;
constexpr unsigned max_chain = 16;
constexpr std::size_t nice_length = 128;
constexpr std::size_t max_insert_length = 256;
constexpr std::size_t tail_insert = 16;
constexpr unsigned skip_trigger = 5;

[[nodiscard]] std::uint32_t hash3(std::span<const std::byte> w, std::size_t k, unsigned shift)
{
    const auto t = w.subspan(k, 3);
    const std::uint32_t v = std::to_integer<std::uint32_t>(t[0]) | (std::to_integer<std::uint32_t>(t[1]) << 8U) |
                            (std::to_integer<std::uint32_t>(t[2]) << 16U);
    return (v * 2654435761U) >> shift;
}

[[nodiscard]] std::uint64_t load64(std::span<const std::byte> w, std::size_t k)
{
    std::array<std::byte, 8> bytes{};
    std::ranges::copy(w.subspan(k, 8), bytes.begin());
    return std::bit_cast<std::uint64_t>(bytes);
}

/// Length of the common prefix of w[a..] and w[b..], at most `limit`.
[[nodiscard]] std::size_t common_length(std::span<const std::byte> w, std::size_t a, std::size_t b, std::size_t limit)
{
    std::size_t n = 0;
    while (n + 8 <= limit) {
        if (const std::uint64_t x = load64(w, a + n) ^ load64(w, b + n); x != 0) {
            if constexpr (std::endian::native == std::endian::little) {
                return n + (static_cast<std::size_t>(std::countr_zero(x)) / 8);
            } else {
                return n + (static_cast<std::size_t>(std::countl_zero(x)) / 8);
            }
        }
        n += 8;
    }
    while (n < limit && w[a + n] == w[b + n]) {
        ++n;
    }
    return n;
}

[[nodiscard]] unsigned match_cost(std::uint32_t distance, std::size_t length)
{
    const auto d = distance_code(distance);
    return d.prefix.length + d.value.length + length_code(length).length;
}

[[nodiscard]] std::size_t literal_cost(std::span<const std::byte> bytes)
{
    std::size_t bits = 0;
    for (const std::byte b : bytes) {
        bits += literal_codes[std::to_integer<std::size_t>(b)].length;
    }
    return bits;
}

/// An unencoded run: distance token 0, a 15-bit count, then byte-aligned data.
constexpr std::size_t max_run = 32767;
constexpr unsigned run_header_bits = 5 + 5 + 15;

}  // namespace

// ---------------------------------------------------------------------------
// Compressor

struct ZgfxCompressor::State {
    ZgfxVariant variant;
    ZgfxMode mode;
    Params p;
    unsigned hash_shift = 0;

    // The history window: window[0] is the byte at absolute stream position
    // window_start. It keeps at least the last `p.history` bytes before the
    // segment being compressed, plus that segment. It grows with the data up
    // to max_window bytes, then slides.
    std::vector<std::byte> window;
    std::size_t max_window = 0;
    std::uint64_t window_start = 0;
    /// Absolute position of the first byte after construction or reset().
    std::uint64_t reset_pos = 0;

    /// Hash of 3 bytes -> most recent position with it, stored as the low 32
    /// bits of position + 1 (0 = none).
    std::vector<std::uint32_t> head;
    /// (position - reset_pos) & chain_mask -> the previous head entry of its
    /// hash. Grows with the data up to bit_ceil(p.history) entries.
    std::vector<std::uint32_t> chain;
    std::uint64_t chain_mask = 0;
    /// Every position before this one has been inserted or skipped.
    std::uint64_t insert_pos = 0;

    std::vector<std::byte> scratch;

    State(ZgfxVariant v, ZgfxMode m) : variant(v), mode(m), p(params(v))
    {
        if (mode == ZgfxMode::store) {
            return;
        }
        const unsigned hash_bits = variant == ZgfxVariant::rdp8 ? hash_bits_rdp8 : hash_bits_lite;
        hash_shift = 32 - hash_bits;
        max_window = (2 * p.history) + p.max_segment;
        head.assign(std::size_t{1} << hash_bits, 0);
        scratch.reserve(p.max_segment + 64);
    }

    [[nodiscard]] std::uint64_t window_end() const { return window_start + window.size(); }

    [[nodiscard]] std::size_t chain_index(std::uint64_t pos) const
    {
        return static_cast<std::size_t>((pos - reset_pos) & chain_mask);
    }

    void reset()
    {
        const std::uint64_t end = window_end();
        std::ranges::fill(head, 0);
        window.clear();
        window_start = end;
        reset_pos = end;
        insert_pos = end;
    }

    /// Appends one segment, sliding the window first if it would not fit.
    void append(std::span<const std::byte> data)
    {
        if (window.size() + data.size() > max_window) {
            const std::size_t keep = std::min(window.size(), p.history);
            const std::size_t shift = window.size() - keep;
            const auto w = std::span(window);
            std::ranges::copy(w.subspan(shift, keep), w.begin());
            window.resize(keep);
            window_start += shift;
        }
        if (window.capacity() < window.size() + data.size()) {
            window.reserve(std::min(max_window, std::max(window.size() + data.size(), 2 * window.capacity())));
        }
        window.insert(window.end(), data.begin(), data.end());

        // Until the history is full, chain entries are indexed by the plain
        // offset since reset_pos, so growing keeps every entry in place. From
        // then on positions chain_mask + 1 apart share a slot.
        const auto needed =
            static_cast<std::size_t>(std::bit_ceil(std::min<std::uint64_t>(p.history, window_end() - reset_pos)));
        if (chain.size() < needed) {
            chain.resize(needed);
            chain_mask = needed - 1;
        }
    }

    void insert(std::span<const std::byte> w, std::uint64_t pos)
    {
        const std::uint32_t h = hash3(w, static_cast<std::size_t>(pos - window_start), hash_shift);
        chain[chain_index(pos)] = head[h];
        head[h] = static_cast<std::uint32_t>(pos + 1);
    }

    /// The first position that does not yet have 3 bytes in the window.
    [[nodiscard]] std::uint64_t hashable_end() const
    {
        return window.size() >= min_match - 1 ? window_end() - (min_match - 1) : window_start;
    }

    /// Inserts the positions in [from, to) that have 3 bytes in the window.
    void insert_range(std::span<const std::byte> w, std::uint64_t from, std::uint64_t to)
    {
        for (std::uint64_t pos = from; pos < std::min(to, hashable_end()); ++pos) {
            insert(w, pos);
        }
    }

    struct Match {
        std::size_t length = 0;
        std::uint32_t distance = 0;
    };

    /// Inserts `pos` and returns the best match there, if any.
    ///
    /// Positions are kept modulo 2^32 and the chain slots are shared by
    /// positions chain_mask + 1 apart, so a stale entry can point anywhere.
    /// That never breaks the output: a candidate is used only if its distance
    /// is within the history and every byte compares equal. The walk ends when
    /// distances stop growing, so it terminates.
    [[nodiscard]] Match find_match(std::span<const std::byte> w, std::uint64_t pos, std::size_t limit)
    {
        const auto at = static_cast<std::size_t>(pos - window_start);
        const std::uint32_t h = hash3(w, at, hash_shift);
        std::uint32_t entry = head[h];
        chain[chain_index(pos)] = entry;
        head[h] = static_cast<std::uint32_t>(pos + 1);

        const auto max_distance = static_cast<std::uint32_t>(std::min<std::uint64_t>(p.history, pos - reset_pos));
        Match best;
        int best_score = 0;
        std::uint32_t last_distance = 0;
        for (unsigned depth = 0; entry != 0 && depth < max_chain; ++depth) {
            const std::uint32_t distance = static_cast<std::uint32_t>(pos) - (entry - 1U);
            if (distance <= last_distance || distance > max_distance) {
                break;
            }
            last_distance = distance;
            const std::size_t cand = at - distance;
            entry = chain[chain_index(pos - distance)];
            // Later candidates are farther away, so only a longer match can win.
            if (w[cand + best.length] != w[at + best.length]) {
                continue;
            }
            const std::size_t length = common_length(w, cand, at, limit);
            if (length < min_match) {
                continue;
            }
            const int score = static_cast<int>(9 * length) - static_cast<int>(match_cost(distance, length));
            if (score > best_score) {
                best_score = score;
                best = {.length = length, .distance = distance};
                if (length >= std::min(limit, nice_length)) {
                    break;
                }
            }
        }
        return best;
    }

    /// Literal bytes: as literal tokens, or as unencoded runs where cheaper.
    static void put_literals(BitWriter& bw, std::span<const std::byte> bytes)
    {
        while (!bytes.empty()) {
            const auto chunk = bytes.first(std::min(bytes.size(), max_run));
            bytes = bytes.subspan(chunk.size());
            const unsigned pad = (8 - ((bw.pending_bits() + run_header_bits) % 8)) % 8;
            const std::size_t run_bits = run_header_bits + pad + (8 * chunk.size());
            if (run_bits < literal_cost(chunk)) {
                const Token& zero = match_tokens[0];
                bw.put(zero.prefix, zero.prefix_length);
                bw.put(0, zero.value_bits);
                bw.put(static_cast<std::uint32_t>(chunk.size()), 15);
                bw.align();
                bw.bytes(chunk);
            } else {
                for (const std::byte b : chunk) {
                    bw.put(literal_codes[std::to_integer<std::size_t>(b)]);
                }
            }
        }
    }

    /// The bit stream for the segment at absolute positions [begin, end),
    /// which is already in the window.
    void compress_segment(std::uint64_t begin, std::uint64_t end, BitWriter& bw)
    {
        const auto w = std::span<const std::byte>(window);
        const auto bytes = [&](std::uint64_t from, std::uint64_t to) {
            return w.subspan(static_cast<std::size_t>(from - window_start), static_cast<std::size_t>(to - from));
        };

        // The last positions of the previous segment had no 3 bytes to hash;
        // a larger gap was skipped on purpose.
        if (begin - insert_pos < min_match) {
            insert_range(w, insert_pos, begin);
            insert_pos = std::max(insert_pos, std::min(begin, hashable_end()));
        } else {
            insert_pos = begin;
        }

        std::uint64_t pos = begin;
        std::uint64_t literals = begin;
        unsigned misses = 0;
        while (pos + min_match <= end) {
            const auto limit = static_cast<std::size_t>(std::min<std::uint64_t>(end - pos, max_match));
            const Match m = find_match(w, pos, limit);
            const bool take = m.length >= min_match && (m.length >= 8 || match_cost(m.distance, m.length) <
                                                                             literal_cost(bytes(pos, pos + m.length)));
            if (!take) {
                pos += 1 + (++misses >> skip_trigger);
                continue;
            }
            put_literals(bw, bytes(literals, pos));
            const auto d = distance_code(m.distance);
            bw.put(d.prefix);
            bw.put(d.value);
            bw.put(length_code(m.length));

            const std::uint64_t match_end = pos + m.length;
            const std::uint64_t first =
                m.length <= max_insert_length ? pos + 1 : std::max(pos + 1, match_end - tail_insert);
            insert_range(w, first, match_end);
            pos = match_end;
            literals = pos;
            misses = 0;
        }
        put_literals(bw, bytes(literals, end));
        // Positions without 3 bytes yet are inserted by the next segment.
        insert_pos = std::max(insert_pos, std::min(pos, hashable_end()));
    }

    void encode_segment(std::span<const std::byte> data, Writer& out)
    {
        FARLAND_ASSERT(data.size() <= p.max_segment);
        if (mode == ZgfxMode::store) {
            out.u8(p.type);
            out.bytes(data);
            return;
        }
        append(data);
        const std::uint64_t end = window_end();
        scratch.clear();
        BitWriter bw(scratch);
        compress_segment(end - data.size(), end, bw);
        bw.finish();
        if (scratch.size() < data.size()) {
            out.u8(p.type | zgfx::packet_compressed);
            out.bytes(scratch);
        } else {
            out.u8(p.type);
            out.bytes(data);
        }
    }

    void compress(std::span<const std::byte> data, Writer& out)
    {
        FARLAND_ASSERT(data.size() <= zgfx_max_input_size(variant));
        if (data.empty()) {
            // FreeRDP rejects a segment of fewer than 2 bytes, so an empty
            // message is an empty bit stream: header and a zero trailer.
            out.u8(zgfx::descriptor_single);
            out.u8(p.type | zgfx::packet_compressed);
            out.u8(0);
            return;
        }
        if (data.size() <= p.max_segment) {
            out.u8(zgfx::descriptor_single);
            encode_segment(data, out);
            return;
        }
        // [MS-RDPEGFX] 2.2.5.1: MULTIPART, segmentCount, uncompressedSize,
        // then RDP_DATA_SEGMENTs (2.2.5.2) of size + bulkData.
        const std::size_t count = (data.size() + p.max_segment - 1) / p.max_segment;
        out.u8(zgfx::descriptor_multipart);
        out.u16le(static_cast<std::uint16_t>(count));
        out.u32le(static_cast<std::uint32_t>(data.size()));
        while (!data.empty()) {
            const auto segment = data.first(std::min(data.size(), p.max_segment));
            data = data.subspan(segment.size());
            const std::size_t size_at = out.size();
            out.u32le(0);
            encode_segment(segment, out);
            out.patch_u32le(size_at, static_cast<std::uint32_t>(out.size() - size_at - 4));
        }
    }
};

ZgfxCompressor::ZgfxCompressor(ZgfxVariant variant, ZgfxMode mode) : state_(std::make_unique<State>(variant, mode)) {}
ZgfxCompressor::~ZgfxCompressor() = default;
ZgfxCompressor::ZgfxCompressor(ZgfxCompressor&&) noexcept = default;
ZgfxCompressor& ZgfxCompressor::operator=(ZgfxCompressor&&) noexcept = default;

std::vector<std::byte> ZgfxCompressor::compress(std::span<const std::byte> data)
{
    Writer out(data.size() + 16);
    compress(data, out);
    return std::move(out).take();
}

void ZgfxCompressor::compress(std::span<const std::byte> data, Writer& out)
{
    state_->compress(data, out);
}

void ZgfxCompressor::reset()
{
    state_->reset();
}

ZgfxVariant ZgfxCompressor::variant() const noexcept
{
    return state_->variant;
}

ZgfxMode ZgfxCompressor::mode() const noexcept
{
    return state_->mode;
}

// ---------------------------------------------------------------------------
// Decompressor

struct ZgfxDecompressor::State {
    ZgfxVariant variant;
    Params p;
    std::size_t max_output;

    /// The last ring.size() bytes of output. The ring grows until it holds
    /// p.history bytes; from then on the next byte overwrites ring[ring_pos].
    std::vector<std::byte> ring;
    std::size_t ring_pos = 0;

    State(ZgfxVariant v, std::size_t limit) : variant(v), p(params(v)), max_output(limit) {}

    void reset()
    {
        ring.clear();
        ring_pos = 0;
    }

    /// Appends output bytes to the history ([MS-RDPEGFX] 3.1.9.1.2: "All
    /// output bytes MUST be recorded in the history buffer").
    void record(std::span<const std::byte> bytes)
    {
        if (bytes.size() > p.history) {
            bytes = bytes.last(p.history);
        }
        if (ring.size() < p.history) {
            const auto grow = bytes.first(std::min(bytes.size(), p.history - ring.size()));
            if (ring.capacity() < ring.size() + grow.size()) {
                ring.reserve(std::min(p.history, std::max(ring.size() + grow.size(), 2 * ring.capacity())));
            }
            ring.insert(ring.end(), grow.begin(), grow.end());
            bytes = bytes.subspan(grow.size());
            ring_pos = ring.size() % p.history;
        }
        const auto r = std::span(ring);
        while (!bytes.empty()) {
            const std::size_t n = std::min(bytes.size(), r.size() - ring_pos);
            std::ranges::copy(bytes.first(n), r.subspan(ring_pos).begin());
            bytes = bytes.subspan(n);
            ring_pos = (ring_pos + n) % r.size();
        }
    }

    /// Appends `length` bytes starting `distance` bytes back in the history.
    /// The history must be up to date and hold at least `distance` bytes.
    /// While the ring is still growing, ring_pos equals its size, so the same
    /// arithmetic finds the source.
    void copy_match(std::size_t distance, std::size_t length, std::vector<std::byte>& out) const
    {
        const auto r = std::span(ring);
        const std::size_t start = out.size();
        out.resize(start + length);
        const auto dst = std::span(out).subspan(start);

        // The first min(length, distance) bytes come from the history...
        const std::size_t from_history = std::min(length, distance);
        const std::size_t src = (ring_pos + r.size() - distance) % r.size();
        const std::size_t before_wrap = std::min(from_history, r.size() - src);
        std::ranges::copy(r.subspan(src, before_wrap), dst.begin());
        std::ranges::copy(r.first(from_history - before_wrap), dst.subspan(before_wrap).begin());
        // ...and a match longer than its distance repeats them.
        for (std::size_t have = from_history; have < length;) {
            const std::size_t n = std::min(have, length - have);
            std::ranges::copy(dst.first(n), dst.subspan(have).begin());
            have += n;
        }
    }

    struct Bounds {
        std::size_t segment_start = 0;  // Output size when the segment began.
        std::size_t limit = 0;          // Largest total output.
        Errc limit_error = Errc::limit_exceeded;
    };

    [[nodiscard]] Result<void> check_room(const Bounds& b, std::size_t have, std::size_t more, std::size_t offset) const
    {
        if (more > p.max_segment - (have - b.segment_start)) {
            return fail(Errc::invalid_length, "ZGFX segment decodes to more than the segment limit", offset);
        }
        if (more > b.limit - have) {
            return fail(b.limit_error, "ZGFX output exceeds its limit", offset);
        }
        return {};
    }

    /// One RDP8_BULK_ENCODED_DATA ([MS-RDPEGFX] 2.2.5.3), appended to `out`.
    [[nodiscard]] Result<void> decode_segment(Reader seg, std::vector<std::byte>& out, std::size_t limit,
                                              Errc limit_error)
    {
        const Bounds bounds{.segment_start = out.size(), .limit = limit, .limit_error = limit_error};
        const std::size_t header_at = seg.offset();
        FARLAND_TRY(const auto header, seg.u8());
        // [MS-RDPEGFX] 3.1.9.1.2.2; the bits other than the type and
        // PACKET_COMPRESSED are reserved and ignored.
        if ((header & zgfx::compression_type_mask) != p.type) {
            return fail(Errc::unsupported, "RDP8_BULK_ENCODED_DATA has an unexpected compression type", header_at);
        }
        if ((header & zgfx::packet_compressed) == 0) {
            const auto data = seg.rest();
            FARLAND_TRY_VOID(check_room(bounds, out.size(), data.size(), seg.offset()));
            out.insert(out.end(), data.begin(), data.end());
            record(data);
            return {};
        }

        // [MS-RDPEGFX] 3.1.9.1.2.4: the last byte counts the unused bits of
        // the byte before it.
        if (seg.empty()) {
            return fail(Errc::truncated, "compressed ZGFX segment has no trailer byte", seg.offset());
        }
        FARLAND_TRY(const Reader stream, seg.sub(seg.remaining() - 1));
        const std::size_t trailer_at = seg.offset();
        FARLAND_TRY(const auto unused, seg.u8());
        if (unused > 7) {
            return fail(Errc::invalid_value, "ZGFX segment trailer is above 7", trailer_at);
        }
        if (unused > stream.size() * 8) {
            return fail(Errc::invalid_length, "ZGFX segment trailer exceeds the bit stream", trailer_at);
        }
        BitReader br(stream, (stream.size() * 8) - unused);

        std::size_t recorded = out.size();  // out[recorded..] is not yet in the history.
        const auto sync = [&] {
            record(std::span(out).subspan(recorded));
            recorded = out.size();
        };

        while (br.remaining() > 0) {
            const std::size_t token_at = br.offset();
            const std::uint8_t index = token_lookup[br.peek(lookup_bits)];
            if (index == reserved_token) {
                return fail(Errc::invalid_value, "reserved ZGFX token", token_at);
            }
            const Token& token = tokens[index];
            FARLAND_TRY_VOID(br.skip(token.prefix_length));
            FARLAND_TRY(const std::uint32_t value, br.read(token.value_bits));

            if (!token.match) {
                FARLAND_TRY_VOID(check_room(bounds, out.size(), 1, token_at));
                out.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(token.base + value)));
                continue;
            }

            const std::uint32_t distance = token.base + value;
            if (distance == 0) {
                // Unencoded run: a 15-bit count, then whole bytes.
                FARLAND_TRY(const std::uint32_t count, br.read(15));
                FARLAND_TRY_VOID(check_room(bounds, out.size(), count, token_at));
                FARLAND_TRY_VOID(br.read_aligned(count, out));
                continue;
            }

            // Match length: k ones and a zero, then k + 1 value bits.
            std::size_t length = min_match;
            FARLAND_TRY(const std::uint32_t first, br.read(1));
            if (first != 0) {
                unsigned ones = 1;
                while (true) {
                    FARLAND_TRY(const std::uint32_t bit, br.read(1));
                    if (bit == 0) {
                        break;
                    }
                    if (++ones > max_length_ones) {
                        return fail(Errc::invalid_value, "ZGFX match length prefix is too long", token_at);
                    }
                }
                FARLAND_TRY(const std::uint32_t extra, br.read(ones + 1));
                length = (std::size_t{1} << (ones + 1)) + extra;
            }

            sync();
            if (distance > ring.size()) {
                return fail(Errc::invalid_value, "ZGFX match distance reaches before the history", token_at);
            }
            FARLAND_TRY_VOID(check_room(bounds, out.size(), length, token_at));
            copy_match(distance, length, out);
        }
        sync();
        return {};
    }

    [[nodiscard]] Result<std::vector<std::byte>> decompress(std::span<const std::byte> data)
    {
        Reader r(data);
        FARLAND_TRY(const auto descriptor, r.u8());
        std::vector<std::byte> out;
        if (descriptor == zgfx::descriptor_single) {
            // [MS-RDPEGFX] 3.1.9.1.2.1: the rest of the input is the segment.
            FARLAND_TRY(const Reader segment, r.sub(r.remaining()));
            FARLAND_TRY_VOID(decode_segment(segment, out, max_output, Errc::limit_exceeded));
            return out;
        }
        if (descriptor != zgfx::descriptor_multipart) {
            return fail(Errc::invalid_value, "unknown RDP_SEGMENTED_DATA descriptor", 0);
        }

        const std::size_t header_at = r.offset();
        FARLAND_TRY(const std::uint16_t count, r.u16le());
        FARLAND_TRY(const std::uint32_t size, r.u32le());
        if (count == 0) {
            return fail(Errc::invalid_value, "MULTIPART RDP_SEGMENTED_DATA without segments", header_at);
        }
        if (size > max_output) {
            return fail(Errc::limit_exceeded, "ZGFX uncompressedSize exceeds the output limit", header_at);
        }
        if (size > std::size_t{count} * p.max_segment) {
            return fail(Errc::invalid_length, "ZGFX uncompressedSize exceeds what its segments can hold", header_at);
        }
        // Each RDP_DATA_SEGMENT (2.2.5.2) holds a 4-byte size and a header byte.
        if (count > r.remaining() / 5) {
            return fail(Errc::truncated, "ZGFX segmentCount exceeds the input", header_at);
        }
        out.reserve(size);
        for (std::size_t i = 0; i < count; ++i) {
            FARLAND_TRY(const std::uint32_t segment_size, r.u32le());
            FARLAND_TRY(const Reader segment, r.sub(segment_size));
            FARLAND_TRY_VOID(decode_segment(segment, out, size, Errc::invalid_length));
        }
        FARLAND_TRY_VOID(r.expect_end("trailing data after the last ZGFX segment"));
        if (out.size() != size) {
            return fail(Errc::invalid_length, "ZGFX segments do not add up to uncompressedSize", header_at);
        }
        return out;
    }
};

ZgfxDecompressor::ZgfxDecompressor(ZgfxVariant variant, std::size_t max_output_size)
    : state_(std::make_unique<State>(variant, max_output_size))
{
}
ZgfxDecompressor::~ZgfxDecompressor() = default;
ZgfxDecompressor::ZgfxDecompressor(ZgfxDecompressor&&) noexcept = default;
ZgfxDecompressor& ZgfxDecompressor::operator=(ZgfxDecompressor&&) noexcept = default;

Result<std::vector<std::byte>> ZgfxDecompressor::decompress(std::span<const std::byte> data)
{
    return state_->decompress(data);
}

void ZgfxDecompressor::reset()
{
    state_->reset();
}

ZgfxVariant ZgfxDecompressor::variant() const noexcept
{
    return state_->variant;
}

}  // namespace farland::codec
