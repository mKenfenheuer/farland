// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/error.hpp>
#include <farland/base/hexdump.hpp>
#include <farland/codec/image.hpp>
#include <farland/codec/planar.hpp>
#include <farland/codec/zgfx.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using farland::Errc;
using farland::to_hex;
using farland::codec::ZgfxCompressor;
using farland::codec::ZgfxDecompressor;
using farland::codec::ZgfxMode;
using farland::codec::ZgfxVariant;
using farland::test::ascii;
using farland::test::hex;
namespace zgfx = farland::codec::zgfx;

namespace {

using Bytes = std::vector<std::byte>;

Bytes to_bytes(std::string_view text)
{
    const auto view = ascii(text);
    return {view.begin(), view.end()};
}

Bytes filled(std::size_t size, std::uint8_t value)
{
    return Bytes(size, std::byte{value});
}

Bytes random_bytes(std::size_t size, std::uint32_t seed)
{
    std::mt19937 rng(seed);
    Bytes out(size);
    for (auto& b : out) {
        b = static_cast<std::byte>(rng() & 0xFFU);
    }
    return out;
}

void append(Bytes& to, const Bytes& from)
{
    to.insert(to.end(), from.begin(), from.end());
}

/// Builds a compressed bit stream by hand, most significant bit first.
class Bits {
public:
    Bits& add(std::string_view pattern)
    {
        for (const char c : pattern) {
            if (c == '0' || c == '1') {
                bits_.push_back(c == '1');
            }
        }
        return *this;
    }

    Bits& add(std::uint32_t value, unsigned count)
    {
        for (unsigned i = count; i-- > 0;) {
            bits_.push_back(((value >> i) & 1U) != 0);
        }
        return *this;
    }

    Bits& align()
    {
        while (bits_.size() % 8 != 0) {
            bits_.push_back(false);
        }
        return *this;
    }

    Bits& byte(std::uint8_t value) { return add(value, 8); }

    /// The data field of a compressed RDP8_BULK_ENCODED_DATA: bytes and trailer.
    [[nodiscard]] Bytes data() const
    {
        Bytes out((bits_.size() + 7) / 8);
        for (std::size_t i = 0; i < bits_.size(); ++i) {
            if (bits_[i]) {
                out[i / 8] |= std::byte{static_cast<std::uint8_t>(0x80U >> (i % 8))};
            }
        }
        out.push_back(std::byte{static_cast<std::uint8_t>((8 - (bits_.size() % 8)) % 8)});
        return out;
    }

    /// A SINGLE RDP_SEGMENTED_DATA with this bit stream.
    [[nodiscard]] Bytes single(std::uint8_t header = 0x24) const
    {
        Bytes out{std::byte{0xE0}, std::byte{header}};
        append(out, data());
        return out;
    }

private:
    std::vector<bool> bits_;
};

Bytes single_raw(const Bytes& payload, std::uint8_t header = 0x04)
{
    Bytes out{std::byte{0xE0}, std::byte{header}};
    append(out, payload);
    return out;
}

void check_round_trip(ZgfxCompressor& c, ZgfxDecompressor& d, const Bytes& data)
{
    const Bytes z = c.compress(data);
    const auto back = d.decompress(z);
    REQUIRE(back.has_value());
    REQUIRE(back->size() == data.size());
    REQUIRE(*back == data);
}

Errc decompress_error(const Bytes& input, ZgfxVariant variant = ZgfxVariant::rdp8,
                      std::size_t limit = zgfx::default_max_output_size)
{
    ZgfxDecompressor d(variant, limit);
    const auto result = d.decompress(input);
    REQUIRE_FALSE(result.has_value());
    return result.error().code;
}

// [MS-RDPEGFX] sample used by FreeRDP's TestFreeRDPCodecZGfx.c.
constexpr std::string_view fox = "The quick brown fox jumps over the lazy dog";

const Bytes fox_single = hex("E0 04 54 68 65 20 71 75 69 63 6B 20 62 72 6F 77"
                             "6E 20 66 6F 78 20 6A 75 6D 70 73 20 6F 76 65 72"
                             "20 74 68 65 20 6C 61 7A 79 20 64 6F 67");

const Bytes fox_multipart = hex("E1 03 00 2B 00 00 00 11 00 00 00 04 54 68 65 20"
                                "71 75 69 63 6B 20 62 72 6F 77 6E 20 0E 00 00 00"
                                "04 66 6F 78 20 6A 75 6D 70 73 20 6F 76 65 10 00"
                                "00 00 24 39 08 0E 91 F8 D8 61 3D 1E 44 06 43 79"
                                "9C 02");

}  // namespace

TEST_CASE("ZGFX decodes FreeRDP's test vectors", "[codec][zgfx]")
{
    SECTION("single, raw")
    {
        ZgfxDecompressor d;
        const auto out = d.decompress(fox_single);
        REQUIRE(out.has_value());
        CHECK(*out == to_bytes(fox));
    }
    SECTION("multipart: two raw segments, then one that matches into them")
    {
        ZgfxDecompressor d;
        const auto out = d.decompress(fox_multipart);
        REQUIRE(out.has_value());
        CHECK(*out == to_bytes(fox));
    }
}

TEST_CASE("ZGFX store mode matches FreeRDP's compressor", "[codec][zgfx]")
{
    ZgfxCompressor store(ZgfxVariant::rdp8, ZgfxMode::store);
    CHECK(to_hex(store.compress(to_bytes(fox))) == to_hex(fox_single));

    // The text has too few repeats to shrink, so compression stores it too.
    ZgfxCompressor c;
    CHECK(to_hex(c.compress(to_bytes(fox))) == to_hex(fox_single));
}

TEST_CASE("ZGFX round-trips FreeRDP's 64 KiB consistency buffer", "[codec][zgfx]")
{
    // test_ZGfxCompressConsistent: the fox followed by 0xAA up to 65,536
    // bytes, one more than a segment holds.
    Bytes big = to_bytes(fox);
    big.resize(65536, std::byte{0xAA});
    ZgfxCompressor c;
    ZgfxDecompressor d;
    const Bytes z = c.compress(big);
    CHECK(z.front() == std::byte{0xE1});
    CHECK(z.size() < 200);
    const auto back = d.decompress(z);
    REQUIRE(back.has_value());
    CHECK(*back == big);
}

TEST_CASE("ZGFX bit stream example of [MS-RDPEGFX] 3.1.9.1.2.5", "[codec][zgfx]")
{
    // 0 0100 1001 10001 00001 110 001: 0x49, then distance 1, length 9.
    const Bytes stream = Bits().add("0 01001001 10001 00001 110 001").single();
    CHECK(to_hex(stream) == to_hex(hex("E0 24 24 C4 38 80 07")));

    ZgfxDecompressor d;
    const auto out = d.decompress(stream);
    REQUIRE(out.has_value());
    CHECK(*out == filled(10, 0x49));

    ZgfxCompressor c;
    CHECK(to_hex(c.compress(filled(10, 0x49))) == to_hex(stream));
}

TEST_CASE("ZGFX Lite examples of [MS-RDPEDYC] 4.3.3 and 4.3.4", "[codec][zgfx]")
{
    // The Data fields of the DYNVC_DATA_FIRST_COMPRESSED and the following
    // DYNVC_DATA_COMPRESSED PDU; the second matches into the first.
    const Bytes first = hex("e0 26 38 c4 3f f4 74 01");
    const Bytes second = hex("e0 26 88 7f e8 f4 02");

    ZgfxDecompressor d(ZgfxVariant::rdp8_lite);
    const auto one = d.decompress(first);
    REQUIRE(one.has_value());
    CHECK(*one == filled(1595, 'q'));
    const auto two = d.decompress(second);
    REQUIRE(two.has_value());
    CHECK(*two == filled(1597, 'q'));

    ZgfxCompressor c(ZgfxVariant::rdp8_lite);
    CHECK(to_hex(c.compress(filled(1595, 'q'))) == to_hex(first));
    CHECK(to_hex(c.compress(filled(1597, 'q'))) == to_hex(second));

    // Without the first message's history the second one is invalid.
    ZgfxDecompressor fresh(ZgfxVariant::rdp8_lite);
    CHECK_FALSE(fresh.decompress(second).has_value());
}

TEST_CASE("ZGFX empty messages", "[codec][zgfx]")
{
    for (const ZgfxMode mode : {ZgfxMode::compress, ZgfxMode::store}) {
        ZgfxCompressor c(ZgfxVariant::rdp8, mode);
        // FreeRDP rejects segments shorter than 2 bytes: use an empty bit stream.
        CHECK(to_hex(c.compress({})) == "e0 24 00");
    }
    ZgfxDecompressor d;
    const auto from_bits = d.decompress(hex("E0 24 00"));
    REQUIRE(from_bits.has_value());
    CHECK(from_bits->empty());
    const auto raw = d.decompress(hex("E0 04"));
    REQUIRE(raw.has_value());
    CHECK(raw->empty());
}

TEST_CASE("ZGFX decodes every token kind", "[codec][zgfx]")
{
    // Every short literal, one long literal, and an unencoded run that starts
    // mid-byte (the rest of that byte is skipped).
    const Bytes shorts = hex("00 01 02 03 FF 04 05 06 07 08 09 0A 0B 3A 3B 3C 3D 3E 3F 40 80 0C 38 39 66");
    Bits bits;
    bits.add("11000 11001 110100 110101 110110 1101110 1101111 1110000 1110001 1110010 1110011 1110100 1110101"
             " 1110110 1110111 1111000 1111001 1111010 1111011 1111100 1111101 11111100 11111101 11111110 11111111");
    bits.add("0 01000001");                                                    // 'A'
    bits.add("10001 00000").add(3, 15).align().byte('x').byte('y').byte('z');  // unencoded run "xyz"
    bits.add("10001 10100 0");                                                 // distance 20, length 3
    Bytes expected = shorts;
    append(expected, to_bytes("Axyz"));
    const Bytes copied(expected.end() - 20, expected.end() - 17);
    append(expected, copied);

    ZgfxDecompressor d;
    const auto out = d.decompress(bits.single());
    REQUIRE(out.has_value());
    CHECK(to_hex(*out) == to_hex(expected));
}

TEST_CASE("ZGFX match lengths and distances at their limits", "[codec][zgfx]")
{
    SECTION("longest match, 65,535 bytes, fills a segment")
    {
        // A literal, then distance 1 with lengths 32768 + 32766 = 65534.
        const Bytes stream = Bits().add("0 00100001 10001 00001").add("111111111111110").add(32766, 15).single();
        ZgfxDecompressor d;
        const auto out = d.decompress(stream);
        REQUIRE(out.has_value());
        CHECK(*out == filled(65535, 0x21));

        // One byte more is over the segment limit.
        const Bytes over =
            Bits().add("0 00100001 0 00100001 10001 00001").add("111111111111110").add(32766, 15).single();
        CHECK(decompress_error(over) == Errc::invalid_length);
    }
    SECTION("distance 2,500,000 reaches the oldest byte of the history")
    {
        ZgfxDecompressor d;
        Bytes history = random_bytes(2'500'000, 7);
        // 39 raw segments of up to 65,535 bytes each.
        for (std::size_t at = 0; at < history.size(); at += zgfx::max_segment_size) {
            const std::size_t n = std::min(zgfx::max_segment_size, history.size() - at);
            const Bytes part(history.begin() + static_cast<std::ptrdiff_t>(at),
                             history.begin() + static_cast<std::ptrdiff_t>(at + n));
            REQUIRE(d.decompress(single_raw(part)).has_value());
        }
        // 10111101: base 2,414,240 plus 21 bits.
        const Bytes stream = Bits().add("10111101").add(2'500'000 - 2'414'240, 21).add("0").single();
        const auto out = d.decompress(stream);
        if (!out.has_value()) {
            FAIL(out.error().message());
        }
        CHECK(*out == Bytes(history.begin(), history.begin() + 3));

        // With 2,500,003 bytes now recorded, 2,500,001 is out of reach.
        const Bytes too_far = Bits().add("10111101").add(2'500'001 - 2'414'240, 21).add("0").single();
        CHECK(d.decompress(too_far).error().code == Errc::invalid_value);
    }
}

TEST_CASE("ZGFX round trips", "[codec][zgfx]")
{
    SECTION("random data is stored raw")
    {
        ZgfxCompressor c;
        ZgfxDecompressor d;
        const Bytes data = random_bytes(10000, 1);
        const Bytes z = c.compress(data);
        CHECK(z.size() == data.size() + 2);
        CHECK(z[1] == std::byte{0x04});
        const auto back = d.decompress(z);
        REQUIRE(back.has_value());
        CHECK(*back == data);
    }
    SECTION("repetitive data compresses")
    {
        ZgfxCompressor c;
        ZgfxDecompressor d;
        Bytes data;
        for (int i = 0; i < 500; ++i) {
            append(data, to_bytes("farland: repetitive text with numbers "));
            append(data, to_bytes(std::to_string(i % 17)));
        }
        const Bytes z = c.compress(data);
        CHECK(z.size() * 10 < data.size());
        const auto back = d.decompress(z);
        REQUIRE(back.has_value());
        CHECK(*back == data);
    }
    SECTION("sizes around the segment limit")
    {
        ZgfxCompressor c;
        ZgfxDecompressor d;
        for (const std::size_t size :
             {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{65534}, std::size_t{65535},
              std::size_t{65536}, std::size_t{131070}, std::size_t{131071}, std::size_t{200000}}) {
            Bytes data = random_bytes(size / 2, static_cast<std::uint32_t>(size));
            append(data, filled(size - data.size(), 0x5A));
            check_round_trip(c, d, data);
        }
    }
    SECTION("many messages share the history, beyond 2.5 MB")
    {
        // Each message: fresh random data, a copy of a block from about 2.36 MB
        // back, runs of zeros and a copy from the same message. 70 messages of
        // 131,000+ bytes each cycle the history three times.
        ZgfxCompressor c;
        ZgfxDecompressor d;
        std::vector<Bytes> sent;
        std::size_t raw_total = 0;
        std::size_t compressed_total = 0;
        std::size_t far_compressed = 0;
        std::size_t far_raw = 0;
        for (std::uint32_t k = 0; k < 70; ++k) {
            Bytes message = random_bytes(40000 + (k * 37), k + 100);
            if (k >= 18) {
                const Bytes& old = sent[k - 18];
                message.insert(message.end(), old.begin(), old.begin() + 40000);
            } else {
                append(message, random_bytes(40000, k + 1000));
            }
            append(message, filled(30000 + k, 0));
            message.insert(message.end(), message.begin() + 100, message.begin() + 20100);
            const Bytes z = c.compress(message);
            const auto back = d.decompress(z);
            REQUIRE(back.has_value());
            REQUIRE(*back == message);
            raw_total += message.size();
            compressed_total += z.size();
            if (k >= 18) {
                far_raw += message.size();
                far_compressed += z.size();
            }
            sent.push_back(std::move(message));
        }
        CHECK(raw_total > 3 * zgfx::history_size);
        // Only the fresh random part is left once the far copies are found.
        CHECK(far_compressed * 100 < far_raw * 40);
        CHECK(compressed_total < raw_total);
    }
    SECTION("store mode")
    {
        ZgfxCompressor c(ZgfxVariant::rdp8, ZgfxMode::store);
        ZgfxDecompressor d;
        const Bytes data = filled(150000, 0);
        const Bytes z = c.compress(data);
        CHECK(z.size() == data.size() + 7 + (3 * 5));
        const auto back = d.decompress(z);
        REQUIRE(back.has_value());
        CHECK(*back == data);
        // The history holds raw segments as well.
        check_round_trip(c, d, to_bytes(fox));
    }
    SECTION("compressed and stored streams interleave on one decompressor")
    {
        ZgfxCompressor c;
        ZgfxDecompressor d;
        const Bytes a = random_bytes(3000, 5);
        check_round_trip(c, d, a);
        Bytes again = a;
        append(again, a);
        check_round_trip(c, d, again);
        CHECK(c.compress(a).size() < 20);
    }
    SECTION("reset")
    {
        ZgfxCompressor c;
        ZgfxDecompressor d;
        const Bytes a = random_bytes(3000, 6);
        check_round_trip(c, d, a);
        c.reset();
        d.reset();
        check_round_trip(c, d, a);
        const Bytes z = c.compress(a);
        ZgfxDecompressor fresh;
        CHECK_FALSE(fresh.decompress(z).has_value());
    }
    SECTION("Lite, many small messages")
    {
        ZgfxCompressor c(ZgfxVariant::rdp8_lite);
        ZgfxDecompressor d(ZgfxVariant::rdp8_lite);
        std::mt19937 rng(9);
        for (int k = 0; k < 400; ++k) {
            Bytes message;
            const std::size_t size = rng() % 1600;
            while (message.size() < size) {
                append(message, to_bytes(k % 3 == 0 ? "{\"x\":12,\"y\":34}" : "ABCDEFG"));
                message.push_back(static_cast<std::byte>(rng() & 0xFFU));
            }
            message.resize(size);
            const Bytes z = c.compress(message);
            CHECK(z.size() <= std::max<std::size_t>(message.size() + 2, 3));
            CHECK(z[0] == std::byte{0xE0});
            const auto back = d.decompress(z);
            REQUIRE(back.has_value());
            REQUIRE(*back == message);
        }
        check_round_trip(c, d, random_bytes(zgfx::lite_max_segment_size, 3));
    }
}

TEST_CASE("ZGFX rejects malformed input", "[codec][zgfx]")
{
    SECTION("framing")
    {
        CHECK(decompress_error({}) == Errc::truncated);
        CHECK(decompress_error(hex("E2 04 00")) == Errc::invalid_value);
        CHECK(decompress_error(hex("E0")) == Errc::truncated);
        CHECK(decompress_error(hex("E1 01")) == Errc::truncated);
        CHECK(decompress_error(hex("E1 00 00 00 00 00 00")) == Errc::invalid_value);  // no segments
        CHECK(decompress_error(hex("E1 01 00 00 00 01 00 05 00 00 00 04 01 02 03 04")) ==
              Errc::invalid_length);                                                             // 65,536 > 1 * 65,535
        CHECK(decompress_error(hex("E1 02 00 02 00 00 00 01 00 00 00 04")) == Errc::truncated);  // count > input
        CHECK(decompress_error(hex("E1 01 00 02 00 00 00 05 00 00 00 04 41 42")) == Errc::truncated);
        CHECK(decompress_error(hex("E1 01 00 02 00 00 00 03 00 00 00 04 41 42 99")) == Errc::trailing_data);
        CHECK(decompress_error(hex("E1 01 00 03 00 00 00 03 00 00 00 04 41 42")) == Errc::invalid_length);
        CHECK(decompress_error(hex("E1 01 00 01 00 00 00 03 00 00 00 04 41 42")) == Errc::invalid_length);
    }
    SECTION("segment header")
    {
        CHECK(decompress_error(hex("E0 05 41")) == Errc::unsupported);
        CHECK(decompress_error(hex("E0 26 38 c4 3f f4 74 01")) == Errc::unsupported);  // Lite on the rdp8 variant
        CHECK(decompress_error(hex("E0 04 41"), ZgfxVariant::rdp8_lite) == Errc::unsupported);
        CHECK(decompress_error(hex("E0 24")) == Errc::truncated);            // no trailer
        CHECK(decompress_error(hex("E0 24 41 08")) == Errc::invalid_value);  // trailer above 7
        CHECK(decompress_error(hex("E0 24 01")) == Errc::invalid_length);    // 1 unused bit of nothing
    }
    SECTION("segment limits")
    {
        CHECK(decompress_error(single_raw(filled(65536, 1))) == Errc::invalid_length);
        CHECK(decompress_error(single_raw(filled(8193, 1), 0x06), ZgfxVariant::rdp8_lite) == Errc::invalid_length);
        CHECK(decompress_error(single_raw(filled(100, 1)), ZgfxVariant::rdp8, 99) == Errc::limit_exceeded);
        CHECK(decompress_error(hex("E1 01 00 64 00 00 00 65 00 00 00 04"), ZgfxVariant::rdp8, 99) ==
              Errc::limit_exceeded);
        // A match that ends past the caller's limit.
        const Bytes match = Bits().add("0 00100001 10001 00001 1110 1111").single();  // 1 + 31 bytes
        CHECK(decompress_error(match, ZgfxVariant::rdp8, 31) == Errc::limit_exceeded);
        ZgfxDecompressor d(ZgfxVariant::rdp8, 32);
        CHECK(d.decompress(match).has_value());
    }
    SECTION("bit stream")
    {
        CHECK(decompress_error(Bits().add("10000 00000").single()) == Errc::invalid_value);     // reserved token
        CHECK(decompress_error(Bits().add("101111100 0000").single()) == Errc::invalid_value);  // FreeRDP-only token
        CHECK(decompress_error(Bits().add("10001 00001 0").single()) == Errc::invalid_value);   // nothing to match
        CHECK(decompress_error(Bits().add("0 00100001 10001 00010 0").single()) ==
              Errc::invalid_value);                                                 // distance 2 of 1
        CHECK(decompress_error(Bits().add("0 0010").single()) == Errc::truncated);  // cut literal
        CHECK(decompress_error(Bits().add("10001").single()) == Errc::truncated);   // cut distance
        CHECK(decompress_error(Bits().add("0 00100001 10001 00001 1111").single()) == Errc::truncated);  // cut length
        CHECK(decompress_error(Bits().add("0 00100001 10001 00001 1111111111111110").add(0, 16).single()) ==
              Errc::invalid_value);  // 15 ones: length 65,536 and up
        CHECK(decompress_error(Bits().add("10001 00000").add(2, 15).align().byte('a').single()) ==
              Errc::truncated);  // unencoded run of 2 with 1 byte
        CHECK(decompress_error(Bits().add("10001 00000").add(1, 14).single()) == Errc::truncated);  // cut count
    }
    SECTION("Lite history is 8,192 bytes")
    {
        ZgfxDecompressor d(ZgfxVariant::rdp8_lite);
        REQUIRE(d.decompress(single_raw(random_bytes(8192, 4), 0x06)).has_value());
        REQUIRE(d.decompress(single_raw(random_bytes(100, 5), 0x06)).has_value());
        // 101100 (base 5,792, 14 bits): distance 8,192 is fine, 8,193 is not.
        CHECK(d.decompress(Bits().add("101100").add(8192 - 5792, 14).add("0").single(0x26)).has_value());
        const auto too_far = d.decompress(Bits().add("101100").add(8193 - 5792, 14).add("0").single(0x26));
        REQUIRE_FALSE(too_far.has_value());
        CHECK(too_far.error().code == Errc::invalid_value);
    }
}

// Throughput and ratio; run with `farland-unit-tests "[.benchmark]"`. Needs an
// optimised build to mean anything.
TEST_CASE("ZGFX compressor benchmark", "[.benchmark]")
{
    using Clock = std::chrono::steady_clock;
    namespace planar = farland::codec::planar;

    const auto run = [](std::string_view name, const std::vector<Bytes>& messages) {
        for (const ZgfxMode mode : {ZgfxMode::compress, ZgfxMode::store}) {
            ZgfxCompressor c(ZgfxVariant::rdp8, mode);
            std::size_t in = 0;
            std::size_t out = 0;
            std::vector<Bytes> compressed;
            const auto start = Clock::now();
            for (const Bytes& m : messages) {
                in += m.size();
                compressed.push_back(c.compress(m));
                out += compressed.back().size();
            }
            const std::chrono::duration<double> took = Clock::now() - start;
            ZgfxDecompressor d;
            const auto dstart = Clock::now();
            for (const Bytes& z : compressed) {
                REQUIRE(d.decompress(z).has_value());
            }
            const std::chrono::duration<double> dtook = Clock::now() - dstart;
            std::cout << name << (mode == ZgfxMode::store ? " (store)" : "") << ": " << in << " -> " << out
                      << " bytes, ratio " << (static_cast<double>(out) / static_cast<double>(in)) << ", compress "
                      << (static_cast<double>(in) / took.count() / 1e6) << " MB/s, decompress "
                      << (static_cast<double>(in) / dtook.count() / 1e6) << " MB/s\n";
        }
    };

    std::vector<Bytes> random;
    for (std::uint32_t i = 0; i < 64; ++i) {
        random.push_back(random_bytes(256 * 1024, i));
    }
    run("random 16 MiB", random);

    run("zeros 16 MiB", std::vector<Bytes>(64, filled(256 * 1024, 0)));

    // Desktop-like frames: flat window backgrounds, gradients, text-like
    // glyph strips that recur, and a cursor-sized noisy patch that moves.
    // Each frame is a few damaged 256x64 tiles, sent raw (BGRA, as in an
    // uncompressed GFX surface command) and planar-encoded.
    std::vector<Bytes> raw_tiles;
    std::vector<Bytes> planar_tiles;
    std::mt19937 rng(42);
    constexpr std::uint32_t w = 256;
    constexpr std::uint32_t h = 64;
    std::vector<std::uint8_t> glyphs(64 * 8);
    for (auto& g : glyphs) {
        g = static_cast<std::uint8_t>(rng() & 0xFFU);
    }
    for (int frame = 0; frame < 400; ++frame) {
        Bytes px(std::size_t{w} * h * 4);
        const int kind = frame % 4;
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                std::uint8_t r = 0xF0;
                std::uint8_t g = 0xF0;
                std::uint8_t b = 0xF0;
                if (kind == 1) {
                    r = static_cast<std::uint8_t>(x);
                    g = static_cast<std::uint8_t>(y * 4);
                    b = 0x80;
                } else if (kind != 3) {
                    // Text: 8x8 glyph cells from a small alphabet.
                    const std::size_t glyph = ((x / 8) * 7 + (y / 8) * 13 + static_cast<std::size_t>(frame)) % 64;
                    const bool ink = ((glyphs[(glyph * 8) + (y % 8)] >> (x % 8)) & 1U) != 0;
                    r = g = b = ink ? 0x20 : 0xFF;
                } else if (x < 32 && y < 32) {
                    r = static_cast<std::uint8_t>(rng());
                    g = static_cast<std::uint8_t>(rng());
                    b = static_cast<std::uint8_t>(rng());
                }
                const std::size_t at = ((std::size_t{y} * w) + x) * 4;
                px[at] = std::byte{b};
                px[at + 1] = std::byte{g};
                px[at + 2] = std::byte{r};
                px[at + 3] = std::byte{0xFF};
            }
        }
        const farland::codec::ImageView view{.data = px, .width = w, .height = h, .stride = std::size_t{w} * 4};
        planar_tiles.push_back(planar::encode(view, {.orientation = planar::Orientation::top_down}));
        raw_tiles.push_back(std::move(px));
    }
    run("desktop tiles, raw BGRA", raw_tiles);
    run("desktop tiles, planar", planar_tiles);
}
