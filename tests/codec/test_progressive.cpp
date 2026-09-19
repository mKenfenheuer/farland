// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/error.hpp>
#include <farland/base/hexdump.hpp>
#include <farland/codec/image.hpp>
#include <farland/codec/progressive.hpp>
#include <farland/codec/rfx_common.hpp>
#include <farland/server/test_pattern.hpp>

#include "codec/rfx_vectors.hpp"
#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rfx = farland::codec::rfx;
namespace progressive = farland::codec::progressive;
namespace vectors = farland::test::rfx_vectors;
using farland::Errc;
using farland::to_hex;
using farland::codec::ImageView;
using farland::test::hex;
using progressive::Rect;

namespace {

/// A tightly packed B, G, R, X image.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::byte> data;

    Image(std::uint32_t w, std::uint32_t h) : width(w), height(h), data(std::size_t{w} * h * 4, std::byte{0}) {}

    void set(std::uint32_t x, std::uint32_t y, int r, int g, int b)
    {
        const std::size_t at = ((std::size_t{y} * width) + x) * 4;
        data[at] = static_cast<std::byte>(std::clamp(b, 0, 255));
        data[at + 1] = static_cast<std::byte>(std::clamp(g, 0, 255));
        data[at + 2] = static_cast<std::byte>(std::clamp(r, 0, 255));
    }
    [[nodiscard]] ImageView view() const
    {
        return {.data = data, .width = width, .height = height, .stride = std::size_t{width} * 4};
    }
};

/// Smooth gradients, soft shapes and mild noise: a stand-in for a photo.
Image natural_image(std::uint32_t w, std::uint32_t h, std::uint32_t seed = 1)
{
    Image img(w, h);
    std::uint32_t state = seed;
    const auto noise = [&] {
        state = (state * 1664525U) + 1013904223U;
        return static_cast<int>((state >> 24U) % 9U) - 4;
    };
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const double fx = static_cast<double>(x) / w;
            const double fy = static_cast<double>(y) / h;
            const double wave = std::sin((fx * 9.0) + (fy * 4.0)) * 40.0;
            const double dx = fx - 0.6;
            const double dy = fy - 0.4;
            const double blob = std::exp(-((dx * dx) + (dy * dy)) * 18.0) * 90.0;
            img.set(x, y, static_cast<int>(60 + (fx * 150) + wave) + noise(),
                    static_cast<int>(90 + (fy * 100) + blob) + noise(),
                    static_cast<int>(140 - (fx * 60) + blob) + noise());
        }
    }
    return img;
}

/// PSNR of the R, G and B channels of `b` against `a`.
double psnr(const ImageView& a, const ImageView& b)
{
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    double sum = 0;
    for (std::uint32_t y = 0; y < a.height; ++y) {
        for (std::uint32_t x = 0; x < a.width; ++x) {
            for (std::size_t c = 0; c < 3; ++c) {
                const auto pa = std::to_integer<int>(a.data[(y * a.stride) + (x * 4) + c]);
                const auto pb = std::to_integer<int>(b.data[(y * b.stride) + (x * 4) + c]);
                sum += static_cast<double>((pa - pb) * (pa - pb));
            }
        }
    }
    const double mse = sum / (3.0 * a.width * a.height);
    return mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

/// Encodes `damage` of `image` and decodes the streams into `decoder`.
std::vector<std::vector<std::byte>> round_trip(progressive::Encoder& encoder, progressive::Decoder& decoder,
                                               const ImageView& image, std::span<const Rect> damage,
                                               std::uint32_t frame_id, std::size_t max_bytes,
                                               progressive::Encoder::Pass pass = progressive::Encoder::Pass::refined)
{
    auto streams = encoder.encode(image, damage, pass);
    for (const auto& stream : streams) {
        CHECK(stream.size() <= max_bytes);
        const auto result = decoder.decode(stream, frame_id);
        INFO((result ? std::string("ok") : result.error().message()));
        REQUIRE(result.has_value());
    }
    return streams;
}

// Offsets in a stream that starts with SYNC (12), CONTEXT (10) and
// FRAME_BEGIN (12): the REGION block, its rects (after its 18-byte header).
constexpr std::size_t region_at = 12 + 10 + 12;
constexpr std::size_t rects_at = region_at + 18;

/// The error of a result that must have failed.
template <class T>
Errc error_code(const farland::Result<T>& result)
{
    REQUIRE(!result.has_value());
    return result.error().code;
}

std::vector<std::int16_t> rlgr_decoded(std::span<const std::byte> data, std::size_t count)
{
    std::vector<std::int16_t> out(count, 0x7777);
    REQUIRE(rfx::rlgr_decode(rfx::RlgrMode::rlgr1, data, out).has_value());
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Primitives

TEST_CASE("RLGR1 matches the progressive entropy examples", "[codec][rfx]")
{
    // [MS-RDPEGFX] 4.1.2.1.1 (frame #1 at 25%) and 4.1.2.1.3 (frame #2 at 25%).
    const std::vector<std::int16_t> frame1{-2, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3, 1, -7, 6};
    const std::vector<std::int16_t> frame2{2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, -4, 8, -9};
    const auto bytes1 = hex("A8 62 6D FF F7 00");
    const auto bytes2 = hex("88 76 BD FF FE F2");

    std::vector<std::byte> out;
    rfx::rlgr_encode(rfx::RlgrMode::rlgr1, frame1, out);
    CHECK(to_hex(out) == to_hex(bytes1));
    out.clear();
    rfx::rlgr_encode(rfx::RlgrMode::rlgr1, frame2, out);
    CHECK(to_hex(out) == to_hex(bytes2));

    CHECK(rlgr_decoded(bytes1, frame1.size()) == frame1);
    CHECK(rlgr_decoded(bytes2, frame2.size()) == frame2);
}

TEST_CASE("RLGR round-trips in both modes", "[codec][rfx]")
{
    std::vector<std::int16_t> data(4096);
    std::uint32_t state = 7;
    for (std::size_t i = 0; i < data.size(); ++i) {
        state = (state * 1103515245U) + 12345U;
        // Mostly zeros with bursts of large values, as after quantization.
        // RLGR3 codes the sum of two 2 * |v| - sign values in FreeRDP's 16-bit
        // `code`, so magnitudes stay below 8192 (real coefficients are far
        // smaller).
        const auto r = static_cast<std::int32_t>((state >> 16U) % 1000U);
        data[i] = static_cast<std::int16_t>(r < 700 ? 0 : (r < 950 ? (r % 7) - 3 : ((r * 337) % 16000) - 8000));
    }
    data.back() = 5;  // see below for a trailing zero
    for (const auto mode : {rfx::RlgrMode::rlgr1, rfx::RlgrMode::rlgr3}) {
        INFO((mode == rfx::RlgrMode::rlgr1 ? "RLGR1" : "RLGR3"));
        std::vector<std::byte> encoded;
        rfx::rlgr_encode(mode, data, encoded);
        std::vector<std::int16_t> decoded(data.size());
        REQUIRE(rfx::rlgr_decode(mode, encoded, decoded).has_value());
        CHECK(decoded == data);
    }
}

TEST_CASE("RLGR1 sends a trailing zero after a run as magnitude 1, like FreeRDP", "[codec][rfx]")
{
    // FreeRDP's rfx_rlgr_encode (and macRDP, which mstsc accepts) ends a
    // zero run at the end of the input with a value of magnitude 1.
    std::vector<std::int16_t> data(64, 0);
    data[0] = 9;
    std::vector<std::byte> encoded;
    rfx::rlgr_encode(rfx::RlgrMode::rlgr1, data, encoded);
    std::vector<std::int16_t> decoded(data.size());
    REQUIRE(rfx::rlgr_decode(rfx::RlgrMode::rlgr1, encoded, decoded).has_value());
    CHECK(decoded[0] == 9);
    CHECK(std::all_of(decoded.begin() + 1, decoded.end() - 1, [](std::int16_t v) { return v == 0; }));
    CHECK(decoded.back() == 1);
}

TEST_CASE("RLGR decoding rejects empty input and zero-fills short input", "[codec][rfx]")
{
    std::vector<std::int16_t> out(16, 3);
    CHECK(rfx::rlgr_decode(rfx::RlgrMode::rlgr1, {}, out).error().code == Errc::invalid_length);
    const auto one = hex("A8");
    REQUIRE(rfx::rlgr_decode(rfx::RlgrMode::rlgr1, one, out).has_value());
    CHECK(out[0] == -2);
    CHECK(std::all_of(out.begin() + 1, out.end(), [](std::int16_t v) { return v == 0; }));
}

namespace {

struct ToyTile {
    std::array<std::int16_t, 14> current;
    std::array<std::int16_t, 14> sign;
};

/// One upgrade pass over the 14-coefficient tile of [MS-RDPEGFX] 4.1.2: HL
/// (5), LH (5) and LL (4) bands, each with its number of bits and PQF shift.
void toy_upgrade(ToyTile& t, std::span<const std::byte> srl, std::span<const std::byte> raw,
                 std::array<std::uint32_t, 3> num_bits, std::array<std::uint32_t, 3> shift)
{
    rfx::UpgradeState state;
    state.srl = rfx::BitReader(srl);
    state.raw = rfx::BitReader(raw);
    const std::span current(t.current);
    const std::span sign(t.sign);
    rfx::upgrade_band(state, current.subspan(0, 5), sign.subspan(0, 5), shift[0], num_bits[0], true);
    rfx::upgrade_band(state, current.subspan(5, 5), sign.subspan(5, 5), shift[1], num_bits[1], true);
    rfx::upgrade_band(state, current.subspan(10, 4), sign.subspan(10, 4), shift[2], num_bits[2], false);
}

}  // namespace

TEST_CASE("SRL and RAW upgrade passes match the progressive decode examples", "[codec][rfx]")
{
    SECTION("frame #1 from 25% to 50%, [MS-RDPEGFX] 4.1.2.2.2")
    {
        ToyTile t{.current = {-32, 0, 0, 0, 0, 0, 0, 0, 0, 8, 12, 16, -12, 12},
                  .sign = {-2, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0}};
        toy_upgrade(t, hex("A0 01 80 00 C9 49 E0"), hex("20"), {4, 2, 0}, {0, 1, 2});
        CHECK(t.current == std::array<std::int16_t, 14>{-34, -13, 15, -3, 0, 0, -6, 4, -2, 8, 12, 16, -12, 12});
        const std::array<int, 10> expected_sign{-1, -1, 1, -1, 0, 0, -1, 1, -1, 1};
        for (std::size_t i = 0; i < expected_sign.size(); ++i) {
            CHECK((t.sign.at(i) > 0) - (t.sign.at(i) < 0) == expected_sign.at(i));
        }
    }
    SECTION("frame #2 from 25% to 50%, [MS-RDPEGFX] 4.1.2.2.4 (spec erratum)")
    {
        // The SRL bytes of 4.1.2.1.4 do not follow the SRL rules of 3.1.8.1.5:
        // the zero before the value 1 is coded "01 | 101", one bit more than
        // the "0 | 1 | 01" those rules (and the encoder of 4.1.2.1.2) give.
        // FreeRDP's progressive_rfx_srl_read and ZeroVDI's independent port of
        // it both read the fourth value as -2 and lose sync from there; this
        // pins that shared behaviour instead of the spec's table.
        ToyTile t{.current = {-2, -13, 15, -3, 0, 0, -6, 4, -2, 8, 24, 12, 16, 4},
                  .sign = {2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
        toy_upgrade(t, hex("80 07 00 16 9C"), hex("00"), {4, 2, 0}, {0, 1, 2});
        CHECK(t.current == std::array<std::int16_t, 14>{-2, -2, 3, -3, -2, 0, -6, 4, -8, 8, 24, 12, 16, 4});
        // The first three SRL values match the spec: 11, -12 and 0.
        CHECK(t.current[1] - -13 == 11);
        CHECK(t.current[2] - 15 == -12);
        CHECK(t.current[3] == -3);
    }
    SECTION("frame #2 from 50% to 100%, [MS-RDPEGFX] 4.1.2.2.5")
    {
        ToyTile t{.current = {-2, -2, 3, -3, 1, 2, -6, 4, -2, 2, 24, 12, 16, 4},
                  .sign = {1, 1, -1, 0, 1, 1, 0, 0, 0, -1, 0, 0, 0, 0}};
        toy_upgrade(t, hex("B0"), hex("C6 40"), {0, 1, 2}, {0, 0, 0});
        CHECK(t.current == std::array<std::int16_t, 14>{-2, -2, 3, -3, 1, 3, -7, 5, -2, 1, 24, 13, 18, 5});
    }
}

TEST_CASE("SRL reading past the end terminates at the largest magnitude", "[codec][rfx]")
{
    // "1" (run end), "0" (k = 1 bit of run length), sign "0", then only zero
    // bits: the magnitude runs to (1 << 30) - 1 and is clamped, without
    // reading a billion bits one by one.
    rfx::UpgradeState state;
    const auto srl = hex("80");
    state.srl = rfx::BitReader(srl);
    CHECK(rfx::srl_read(state, 30) == INT16_MAX);
}

TEST_CASE("YCbCr to RGB matches FreeRDP's primitives test vectors", "[codec][rfx]")
{
    // libfreerdp/primitives/test/TestPrimitivesYCbCr.c, margin 1 per channel.
    std::size_t off = 0;
    for (std::size_t i = 0; i < vectors::ycbcr_image.size(); ++i) {
        const auto c = rfx::ycbcr_to_rgb(vectors::ycbcr_y[i], vectors::ycbcr_cb[i], vectors::ycbcr_cr[i]);
        const std::uint32_t expected = vectors::ycbcr_image[i];
        const auto near = [](int a, std::uint32_t b) { return std::abs(a - static_cast<int>(b & 0xFFU)) <= 1; };
        if (!near(c.r, expected >> 16U) || !near(c.g, expected >> 8U) || !near(c.b, expected)) {
            ++off;
        }
    }
    CHECK(off == 0);
}

TEST_CASE("RGB to YCbCr and back stays within one step", "[codec][rfx]")
{
    CHECK(rfx::rgb_to_ycbcr(255, 255, 255).y == 4064);
    CHECK(rfx::rgb_to_ycbcr(255, 255, 255).cb == 0);
    CHECK(rfx::rgb_to_ycbcr(0, 0, 0).y == -4096);
    int worst = 0;
    for (int r = 0; r < 256; r += 5) {
        for (int g = 0; g < 256; g += 3) {
            for (int b = 0; b < 256; b += 7) {
                const auto ycc = rfx::rgb_to_ycbcr(r, g, b);
                const auto back = rfx::ycbcr_to_rgb(ycc.y, ycc.cb, ycc.cr);
                worst = std::max({worst, std::abs(back.r - r), std::abs(back.g - g), std::abs(back.b - b)});
            }
        }
    }
    CHECK(worst <= 1);
}

TEST_CASE("The RemoteFX sample tile decodes to the reference image", "[codec][rfx]")
{
    // [MS-RDPRFX] 4.2.4: the TS_RFX_TILE of 4.2.4.1 (RLGR3, quantization
    // LL3 6, LH3 6, HL3 6, HH3 6, LH2 7, HL2 7, HH2 8, LH1 8, HL1 8, HH1 9)
    // decodes to the image of 4.2.4.4, within 1 per channel as in FreeRDP's
    // TestFreeRDPCodecRemoteFX. This covers RLGR3, differential decoding,
    // dequantization, the classic inverse DWT and the colour transform.
    const std::span messages(vectors::rfx_sample_messages);
    constexpr std::size_t tile_at = 64;
    REQUIRE(messages[tile_at] == 0xC3);  // CBT_TILE
    REQUIRE(messages[tile_at + 1] == 0xCA);
    const auto u16 = [&](std::size_t at) { return static_cast<std::size_t>(messages[at] | (messages[at + 1] << 8U)); };
    const std::array<std::size_t, 3> lengths{u16(tile_at + 13), u16(tile_at + 15), u16(tile_at + 17)};
    REQUIRE(lengths == std::array<std::size_t, 3>{0x3AE, 0x3CF, 0x393});

    const rfx::Quant quant = rfx::quant_from_rfx_order({6, 6, 6, 6, 7, 7, 8, 8, 8, 9});
    rfx::Quant shift;
    for (std::size_t b = 0; b < rfx::band_count; ++b) {
        shift.bands.at(b) = static_cast<std::uint8_t>(quant.bands.at(b) - 1);
    }
    rfx::Planes planes;
    rfx::Coefficients scratch{};
    std::size_t offset = tile_at + 19;
    for (auto* component : {&planes.y, &planes.cb, &planes.cr}) {
        const std::size_t length =
            lengths.at(static_cast<std::size_t>(component == &planes.y ? 0 : (component == &planes.cb ? 1 : 2)));
        const auto data = std::as_bytes(messages.subspan(offset, length));
        offset += length;
        REQUIRE(rfx::rlgr_decode(rfx::RlgrMode::rlgr3, data, *component).has_value());
        rfx::differential_decode(std::span(*component).subspan(4032, 64));
        REQUIRE(rfx::dequantize(*component, rfx::standard_layout, shift));
        rfx::dwt_decode(*component, scratch);
    }
    std::vector<std::byte> pixels(rfx::tile_coefficients * 4);
    rfx::store_tile(planes, pixels);

    std::size_t off = 0;
    for (std::size_t i = 0; i < vectors::rfx_sample_image.size(); ++i) {
        const std::uint32_t expected = vectors::rfx_sample_image[i];
        for (std::size_t c = 0; c < 3; ++c) {
            const auto got = std::to_integer<int>(pixels[(i * 4) + c]);
            const auto want = static_cast<int>((expected >> (8U * c)) & 0xFFU);
            off += std::abs(got - want) > 1 ? 1 : 0;
        }
    }
    CHECK(off == 0);
}

TEST_CASE("Forward and inverse classic DWT round-trip closely", "[codec][rfx]")
{
    rfx::Coefficients data{};
    rfx::Coefficients original{};
    for (std::size_t i = 0; i < data.size(); ++i) {
        original[i] = static_cast<std::int16_t>((((i * 37) % 251) * 32) - 4000);
    }
    data = original;
    rfx::Coefficients scratch{};
    rfx::dwt_encode(data, scratch);
    rfx::dwt_decode(data, scratch);
    int worst = 0;
    for (std::size_t i = 0; i < data.size(); ++i) {
        worst = std::max(worst, std::abs(data[i] - original[i]));
    }
    // FreeRDP's forward and inverse lifting steps round differently, so the
    // pair is not exactly reversible; 16 is half a pixel level in 11.5 fixed
    // point.
    CHECK(worst <= 16);
}

TEST_CASE("Component quantization tables pack as in [MS-RDPEGFX] 2.2.4.2.1.5.2", "[codec][rfx]")
{
    farland::Writer w;
    rfx::write_component_quant(w, progressive::quant_default);
    // LL3 6 | HL3 6, LH3 6 | HH3 6, HL2 7 | LH2 7, HH2 8 | HL1 8, LH1 8 | HH1 9.
    CHECK(to_hex(w.view()) == "66 66 77 88 98");
    farland::Reader r(w.view());
    const auto q = rfx::read_component_quant(r);
    REQUIRE(q.has_value());
    CHECK(*q == progressive::quant_default);
}

// ---------------------------------------------------------------------------
// Encoder and decoder

TEST_CASE("Progressive streams have the macRDP block layout", "[codec][progressive]")
{
    const Image img = natural_image(64, 64);
    progressive::Encoder encoder(64, 64);
    const std::array damage{Rect{.x = 0, .y = 0, .width = 64, .height = 64}};
    const auto streams = encoder.encode(img.view(), damage);
    REQUIRE(streams.size() == 1);
    const auto& s = streams[0];
    // SYNC, then CONTEXT (ctxId 0, tileSize 64, flags 0), then FRAME_BEGIN.
    CHECK(to_hex(std::span(s).first(12)) == "c0 cc 0c 00 00 00 ca ac cc ca 00 01");
    CHECK(to_hex(std::span(s).subspan(12, 10)) == "c3 cc 0a 00 00 00 00 40 00 00");
    CHECK(to_hex(std::span(s).subspan(22, 12)) == "c1 cc 0c 00 00 00 01 00 00 00 01 00");
    // REGION: tileSize 64, one rect, one quant table, one progressive table,
    // classic DWT, one tile.
    CHECK(to_hex(std::span(s).subspan(34, 2)) == "c4 cc");
    CHECK(to_hex(std::span(s).subspan(40, 7)) == "40 01 00 01 01 00 01");
    CHECK(to_hex(std::span(s).subspan(52, 8)) == "00 00 00 00 40 00 40 00");  // the rect
    CHECK(to_hex(std::span(s).subspan(60, 5)) == "66 66 77 88 98");           // quant_default
    CHECK(to_hex(std::span(s).subspan(65, 16)) == "64 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00");
    CHECK(to_hex(std::span(s).subspan(81, 2)) == "c6 cc");  // TILE_FIRST
    CHECK(to_hex(std::span(s).last(6)) == "c2 cc 06 00 00 00");
}

TEST_CASE("Round trip at the highest quality exceeds 40 dB", "[codec][progressive]")
{
    constexpr std::uint32_t w = 1920;
    constexpr std::uint32_t h = 1080;
    const progressive::EncoderOptions options{.quant = progressive::quant_highest};
    const std::array full{Rect{.x = 0, .y = 0, .width = w, .height = h}};

    SECTION("natural image")
    {
        const Image img = natural_image(w, h);
        progressive::Encoder encoder(w, h, options);
        auto decoder = progressive::Decoder::create(w, h);
        REQUIRE(decoder.has_value());
        const auto streams = round_trip(encoder, *decoder, img.view(), full, 1, progressive::default_max_bytes);
        CHECK(streams.size() > 1);
        const double quality = psnr(img.view(), decoder->image());
        INFO("PSNR " << quality << " dB, " << streams.size() << " streams");
        CHECK(quality >= 40.0);
    }
    SECTION("test pattern")
    {
        farland::server::TestPattern pattern(w, h);
        const ImageView view = pattern.render(3);
        progressive::Encoder encoder(w, h, options);
        auto decoder = progressive::Decoder::create(w, h);
        REQUIRE(decoder.has_value());
        round_trip(encoder, *decoder, view, full, 1, progressive::default_max_bytes);
        const double quality = psnr(view, decoder->image());
        INFO("PSNR " << quality << " dB");
        CHECK(quality >= 40.0);
    }
}

TEST_CASE("The default quantization still gives a good picture", "[codec][progressive]")
{
    const Image img = natural_image(320, 200);
    progressive::Encoder encoder(320, 200);
    auto decoder = progressive::Decoder::create(320, 200);
    REQUIRE(decoder.has_value());
    const std::array full{Rect{.x = 0, .y = 0, .width = 320, .height = 200}};
    round_trip(encoder, *decoder, img.view(), full, 1, progressive::default_max_bytes);
    const double quality = psnr(img.view(), decoder->image());
    INFO("PSNR " << quality << " dB");
    CHECK(quality >= 34.0);
}

TEST_CASE("Only damaged tiles are sent", "[codec][progressive]")
{
    Image img = natural_image(300, 200);
    progressive::Encoder encoder(300, 200, {.quant = progressive::quant_highest});
    auto decoder = progressive::Decoder::create(300, 200);
    REQUIRE(decoder.has_value());
    const std::array full{Rect{.x = 0, .y = 0, .width = 300, .height = 200}};
    round_trip(encoder, *decoder, img.view(), full, 1, progressive::default_max_bytes);

    CHECK(encoder.encode(img.view(), {}).empty());
    const std::array outside{Rect{.x = 400, .y = 0, .width = 10, .height = 10}};
    CHECK(encoder.encode(img.view(), outside).empty());

    // Change a 10 x 10 square straddling four tiles.
    for (std::uint32_t y = 60; y < 70; ++y) {
        for (std::uint32_t x = 60; x < 70; ++x) {
            img.set(x, y, 255, 0, 0);
        }
    }
    const std::array damage{Rect{.x = 60, .y = 60, .width = 10, .height = 10}};
    const auto streams = round_trip(encoder, *decoder, img.view(), damage, 2, progressive::default_max_bytes);
    REQUIRE(streams.size() == 1);
    // SYNC 12 + CONTEXT 10 + FRAME_BEGIN 12, then the REGION's numTiles.
    CHECK(streams[0][region_at + 12] == std::byte{4});
    CHECK(psnr(img.view(), decoder->image()) >= 40.0);
}

TEST_CASE("Streams respect the byte budget, even for noise", "[codec][progressive]")
{
    constexpr std::uint32_t w = 256;
    constexpr std::uint32_t h = 130;
    Image img(w, h);
    std::uint32_t state = 99;
    for (auto& b : img.data) {
        state = (state * 1664525U) + 1013904223U;
        b = static_cast<std::byte>(state >> 24U);
    }
    const std::array full{Rect{.x = 0, .y = 0, .width = w, .height = h}};
    for (const std::size_t max_bytes :
         {progressive::min_max_bytes, std::size_t{4096}, progressive::default_max_bytes, progressive::max_max_bytes}) {
        progressive::Encoder encoder(w, h, {.quant = progressive::quant_highest, .max_bytes = max_bytes});
        auto decoder = progressive::Decoder::create(w, h);
        REQUIRE(decoder.has_value());
        const auto streams = round_trip(encoder, *decoder, img.view(), full, 1, max_bytes);
        CHECK(!streams.empty());
    }
}

TEST_CASE("Odd surface sizes encode and decode", "[codec][progressive]")
{
    for (const auto& [w, h] : {std::pair{1U, 1U}, std::pair{65U, 33U}, std::pair{8192U, 64U}, std::pair{63U, 8192U}}) {
        const Image img = natural_image(w, h, w + h);
        progressive::Encoder encoder(w, h, {.quant = progressive::quant_highest});
        auto decoder = progressive::Decoder::create(w, h);
        REQUIRE(decoder.has_value());
        const std::array full{Rect{.x = 0, .y = 0, .width = w, .height = h}};
        round_trip(encoder, *decoder, img.view(), full, 1, progressive::default_max_bytes);
        INFO(w << " x " << h);
        CHECK(psnr(img.view(), decoder->image()) >= 40.0);
    }
}

TEST_CASE("SYNC can be limited to the first stream", "[codec][progressive]")
{
    const Image img = natural_image(640, 480);
    progressive::Encoder encoder(640, 480, {.sync_every_stream = false});
    const std::array full{Rect{.x = 0, .y = 0, .width = 640, .height = 480}};
    const auto first = encoder.encode(img.view(), full);
    REQUIRE(first.size() > 1);
    CHECK(first[0][0] == std::byte{0xC0});
    CHECK(first[1][0] == std::byte{0xC3});
    const auto later = encoder.encode(img.view(), full);
    CHECK(later[0][0] == std::byte{0xC3});
    encoder.reset();
    CHECK(encoder.encode(img.view(), full)[0][0] == std::byte{0xC0});
}

// ---------------------------------------------------------------------------
// Decoder input checks

TEST_CASE("Progressive decoder rejects bad surfaces and malformed streams", "[codec][progressive]")
{
    CHECK(error_code(progressive::Decoder::create(0, 10)) == Errc::invalid_value);
    CHECK(error_code(progressive::Decoder::create(10, 8193)) == Errc::limit_exceeded);

    const Image img = natural_image(128, 64);
    progressive::Encoder encoder(128, 64);
    const std::array full{Rect{.x = 0, .y = 0, .width = 128, .height = 64}};
    const auto streams = encoder.encode(img.view(), full);
    REQUIRE(streams.size() == 1);
    const auto& good = streams[0];

    auto decoder = progressive::Decoder::create(128, 64);
    REQUIRE(decoder.has_value());
    REQUIRE(decoder->decode(good, 1).has_value());

    SECTION("truncation inside a block fails")
    {
        // A cut at a block boundary leaves a valid stream.
        const std::array boundaries{std::size_t{12}, std::size_t{22}, region_at, good.size() - 6};
        for (std::size_t n = 1; n < good.size(); ++n) {
            if (std::ranges::find(boundaries, n) != boundaries.end()) {
                continue;
            }
            auto d = progressive::Decoder::create(128, 64);
            INFO("cut at " << n);
            CHECK(!d->decode(std::span(good).first(n), 1).has_value());
        }
    }
    SECTION("unknown block type")
    {
        auto bad = good;
        bad[0] = std::byte{0xC8};
        CHECK(error_code(decoder->decode(bad, 2)) == Errc::invalid_value);
    }
    SECTION("SYNC with a bad blockLen")
    {
        auto bad = good;
        bad[2] = std::byte{13};
        CHECK(!decoder->decode(bad, 2).has_value());
    }
    // Two rects (16 bytes), one quantization table (5), one progressive
    // table (16), then the first TILE_FIRST block.
    constexpr std::size_t quant_at = rects_at + 16;
    constexpr std::size_t tile_at = quant_at + 5 + 16;
    REQUIRE(good[tile_at] == std::byte{0xC6});

    SECTION("tile outside a smaller surface")
    {
        auto small = progressive::Decoder::create(64, 64);
        CHECK(error_code(small->decode(good, 1)) == Errc::invalid_value);
    }
    SECTION("quantization factor below 6")
    {
        auto bad = good;
        bad[quant_at] = std::byte{0x65};  // LL3 5
        CHECK(error_code(decoder->decode(bad, 2)) == Errc::invalid_value);
    }
    SECTION("quantIdx without a table")
    {
        auto bad = good;
        bad[tile_at + 6] = std::byte{1};
        CHECK(error_code(decoder->decode(bad, 2)) == Errc::invalid_value);
    }
    SECTION("progressiveQuality without a table")
    {
        auto bad = good;
        bad[tile_at + 14] = std::byte{1};
        CHECK(error_code(decoder->decode(bad, 2)) == Errc::invalid_value);
    }
    SECTION("numTiles disagrees")
    {
        auto bad = good;
        bad[region_at + 12] = std::byte{3};
        CHECK(error_code(decoder->decode(bad, 2)) == Errc::invalid_length);
    }
    SECTION("region rect past the surface")
    {
        // FreeRDP checks rect-and-tile intersections against the surface, so
        // this needs a surface narrower than its last tile column.
        const Image narrow = natural_image(120, 64);
        progressive::Encoder narrow_encoder(120, 64);
        const std::array narrow_full{Rect{.x = 0, .y = 0, .width = 120, .height = 64}};
        auto bad = narrow_encoder.encode(narrow.view(), narrow_full)[0];
        auto narrow_decoder = progressive::Decoder::create(120, 64);
        REQUIRE(narrow_decoder->decode(bad, 1).has_value());
        REQUIRE(bad[rects_at + 12] == std::byte{56});  // second rect: x 64, width 56
        bad[rects_at + 12] = std::byte{57};
        CHECK(error_code(narrow_decoder->decode(bad, 2)) == Errc::invalid_value);
        // Wider than the surface but inside the tile grid is fine.
        auto wide = good;
        wide[rects_at + 12] = std::byte{0x41};  // x 64, width 65 on a 128-wide surface
        CHECK(decoder->decode(wide, 2).has_value());
    }
    SECTION("trailing garbage")
    {
        auto bad = good;
        bad.push_back(std::byte{0});
        CHECK(!decoder->decode(bad, 2).has_value());
    }
}

namespace {

/// The RLGR1 data of each component of the only tile in a one-tile stream.
using TileData = std::array<std::vector<std::byte>, 3>;
TileData tile_data(const std::vector<std::byte>& stream)
{
    constexpr std::size_t tile = rects_at + 8 + 5 + 16;  // one rect, quant table, progressive table
    REQUIRE(stream[tile] == std::byte{0xC6});
    TileData out;
    std::size_t at = tile + 23;
    for (std::size_t c = 0; c < 3; ++c) {
        const std::size_t length = std::to_integer<std::size_t>(stream[tile + 15 + (2 * c)]) |
                                   (std::to_integer<std::size_t>(stream[tile + 16 + (2 * c)]) << 8U);
        out.at(c).assign(stream.begin() + static_cast<std::ptrdiff_t>(at),
                         stream.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
    }
    return out;
}

/// RFX_PROGRESSIVE_TILE_FIRST for tile (0, 0), quantIdx 0.
std::vector<std::byte> tile_first_block(std::uint8_t flags, std::uint8_t quality, const TileData& data)
{
    farland::Writer w;
    w.u16le(0xCCC6);
    w.u32le(static_cast<std::uint32_t>(23 + data[0].size() + data[1].size() + data[2].size()));
    w.zeros(3 + 4);  // quantIdx Y, Cb, Cr; xIdx, yIdx
    w.u8(flags);
    w.u8(quality);
    for (const auto& component : data) {
        w.u16le(static_cast<std::uint16_t>(component.size()));
    }
    w.u16le(0);
    for (const auto& component : data) {
        w.bytes(component);
    }
    return std::move(w).take();
}

/// RFX_PROGRESSIVE_TILE_UPGRADE for tile (0, 0), quantIdx 0, the same SRL and
/// RAW data for every component.
std::vector<std::byte> tile_upgrade_block(std::uint8_t quality, std::span<const std::byte> srl,
                                          std::span<const std::byte> raw)
{
    farland::Writer w;
    w.u16le(0xCCC7);
    w.u32le(static_cast<std::uint32_t>(26 + (3 * (srl.size() + raw.size()))));  // 6 + 20 header bytes
    w.zeros(3 + 4);
    w.u8(quality);
    for (int c = 0; c < 3; ++c) {
        w.u16le(static_cast<std::uint16_t>(srl.size()));
        w.u16le(static_cast<std::uint16_t>(raw.size()));
    }
    for (int c = 0; c < 3; ++c) {
        w.bytes(srl);
        w.bytes(raw);
    }
    return std::move(w).take();
}

/// A stream with one REGION (flags `region_flags`, one 64 x 64 rect,
/// quant_default, one progressive table per entry of `prog_shifts`) holding
/// `tile`.
std::vector<std::byte> region_stream(std::uint8_t region_flags, std::span<const std::uint8_t> prog_shifts,
                                     std::span<const std::byte> tile)
{
    farland::Writer w;
    w.u16le(0xCCC0);
    w.u32le(12);
    w.u32le(0xCACCACCA);
    w.u16le(0x0100);
    w.u16le(0xCCC3);
    w.u32le(10);
    w.u8(0);
    w.u16le(64);
    w.u8(1);  // RFX_SUBBAND_DIFFING
    w.u16le(0xCCC1);
    w.u32le(12);
    w.u32le(1);
    w.u16le(1);
    w.u16le(0xCCC4);
    w.u32le(static_cast<std::uint32_t>(18 + 8 + 5 + (16 * prog_shifts.size()) + tile.size()));
    w.u8(64);
    w.u16le(1);
    w.u8(1);
    w.u8(static_cast<std::uint8_t>(prog_shifts.size()));
    w.u8(region_flags);
    w.u16le(1);
    w.u32le(static_cast<std::uint32_t>(tile.size()));
    w.u16le(0);
    w.u16le(0);
    w.u16le(64);
    w.u16le(64);
    rfx::write_component_quant(w, progressive::quant_default);
    for (const std::uint8_t shift : prog_shifts) {
        w.u8(100);
        for (int c = 0; c < 3; ++c) {
            rfx::write_component_quant(w, rfx::uniform_quant(shift));
        }
    }
    w.bytes(tile);
    w.u16le(0xCCC2);
    w.u32le(6);
    return std::move(w).take();
}

std::vector<std::byte> pixels_of(const progressive::Decoder& decoder)
{
    const auto data = decoder.image().data;
    return {data.begin(), data.end()};
}

}  // namespace

TEST_CASE("TILE_UPGRADE passes refine a TILE_FIRST tile", "[codec][progressive]")
{
    const Image img = natural_image(64, 64);
    progressive::Encoder encoder(64, 64);
    const std::array full{Rect{.x = 0, .y = 0, .width = 64, .height = 64}};
    const TileData data = tile_data(encoder.encode(img.view(), full)[0]);
    const std::array<std::uint8_t, 2> prog{2, 0};  // quality 0: 2 extra bits, quality 1: none
    const auto srl = hex("A0 01 80 00 C9 49 E0 5A 3C");
    const auto raw = hex("20 C6 40 FF 13");

    for (const std::uint8_t region_flags : {std::uint8_t{0}, std::uint8_t{1}}) {
        INFO("region flags " << int{region_flags});
        auto decoder = progressive::Decoder::create(64, 64);
        // Without a first pass there are no bits to upgrade.
        CHECK(error_code(decoder->decode(region_stream(region_flags, prog, tile_upgrade_block(1, {}, {})), 1)) ==
              Errc::invalid_value);
        REQUIRE(decoder->decode(region_stream(region_flags, prog, tile_first_block(0, 0, data)), 2).has_value());
        const auto first = pixels_of(*decoder);
        // Empty SRL and RAW data reads as zeros and adds nothing; neither does
        // a second upgrade to the same quality.
        REQUIRE(decoder->decode(region_stream(region_flags, prog, tile_upgrade_block(1, {}, {})), 3).has_value());
        CHECK(pixels_of(*decoder) == first);
        REQUIRE(decoder->decode(region_stream(region_flags, prog, tile_upgrade_block(1, srl, raw)), 4).has_value());
        CHECK(pixels_of(*decoder) == first);
        // Back to a coarser quality is not an upgrade.
        CHECK(error_code(decoder->decode(region_stream(region_flags, prog, tile_upgrade_block(0, {}, {})), 5)) ==
              Errc::invalid_value);

        auto refined = progressive::Decoder::create(64, 64);
        REQUIRE(refined->decode(region_stream(region_flags, prog, tile_first_block(0, 0, data)), 1).has_value());
        REQUIRE(refined->decode(region_stream(region_flags, prog, tile_upgrade_block(1, srl, raw)), 2).has_value());
        CHECK(pixels_of(*refined) != first);
    }
}

TEST_CASE("Difference tiles add to the tile's coefficients", "[codec][progressive]")
{
    const Image img = natural_image(64, 64);
    progressive::Encoder encoder(64, 64);
    const std::array full{Rect{.x = 0, .y = 0, .width = 64, .height = 64}};
    const TileData data = tile_data(encoder.encode(img.view(), full)[0]);
    const std::array<std::uint8_t, 1> prog{0};

    auto decoder = progressive::Decoder::create(64, 64);
    REQUIRE(decoder->decode(region_stream(0, prog, tile_first_block(0, 0, data)), 1).has_value());
    const auto once = pixels_of(*decoder);
    CHECK(psnr(img.view(), decoder->image()) >= 34.0);
    // An original tile replaces the coefficients ...
    REQUIRE(decoder->decode(region_stream(0, prog, tile_first_block(0, 0xFF, data)), 2).has_value());
    CHECK(pixels_of(*decoder) == once);
    // ... a difference tile adds to them.
    REQUIRE(decoder->decode(region_stream(0, prog, tile_first_block(1, 0xFF, data)), 3).has_value());
    CHECK(pixels_of(*decoder) != once);
}

TEST_CASE("A region outside FRAME_BEGIN and FRAME_END is ignored", "[codec][progressive]")
{
    const Image img = natural_image(64, 64);
    progressive::Encoder encoder(64, 64);
    const std::array full{Rect{.x = 0, .y = 0, .width = 64, .height = 64}};
    auto stream = encoder.encode(img.view(), full)[0];
    // Drop FRAME_BEGIN (bytes 22..33): the region is then skipped.
    stream.erase(stream.begin() + 22, stream.begin() + 34);
    auto decoder = progressive::Decoder::create(64, 64);
    REQUIRE(decoder->decode(stream, 1).has_value());
    CHECK(std::to_integer<int>(decoder->image().data[0]) == 0);
}

TEST_CASE("Tiles of one frame are re-copied for later regions of the frame", "[codec][progressive]")
{
    // FreeRDP update_tiles: every tile updated under the current frame id is
    // copied again, clipped to the rects of each later stream's region.
    Image img = natural_image(128, 64);
    progressive::Encoder encoder(128, 64, {.quant = progressive::quant_highest});
    const std::array left{Rect{.x = 0, .y = 0, .width = 64, .height = 64}};
    const std::array right{Rect{.x = 64, .y = 0, .width = 64, .height = 64}};
    const auto a = encoder.encode(img.view(), left)[0];
    auto b = encoder.encode(img.view(), right)[0];
    // Widen the second stream's rect to the whole surface.
    b[rects_at] = std::byte{0};
    b[rects_at + 4] = std::byte{128};

    auto same_frame = progressive::Decoder::create(128, 64);
    auto new_frame = progressive::Decoder::create(128, 64);
    // A region's rects clip its own tiles: with only the left tile decoded
    // and rect 0..64, both decoders agree.
    REQUIRE(same_frame->decode(a, 7).has_value());
    REQUIRE(new_frame->decode(a, 7).has_value());
    // Overwrite the left half of the surface the decoders show by decoding the
    // left tile again under another frame id is not possible from outside, so
    // compare instead: the same-frame decoder copies both tiles for `b`.
    REQUIRE(same_frame->decode(b, 7).has_value());
    REQUIRE(new_frame->decode(b, 8).has_value());
    CHECK(psnr(img.view(), same_frame->image()) >= 40.0);
    CHECK(psnr(img.view(), new_frame->image()) >= 40.0);
}

// ---------------------------------------------------------------------------
// Reduce-extrapolate and progressive refinement

namespace {

const progressive::EncoderOptions refine_options{
    .quant = progressive::quant_highest, .reduce_extrapolate = true, .refine = true};

/// Calls upgrade() with `budget` until no tile is pending, decoding every
/// stream; returns the number of calls that produced streams.
std::size_t refine_fully(progressive::Encoder& encoder, progressive::Decoder& decoder, std::uint32_t& frame_id,
                         std::size_t budget, const ImageView* image = nullptr, std::vector<double>* quality = nullptr)
{
    std::size_t calls = 0;
    while (encoder.pending_tiles() > 0) {
        const auto streams = encoder.upgrade(budget);
        REQUIRE(!streams.empty());
        ++calls;
        REQUIRE(calls < 10000);
        std::size_t total = 0;
        ++frame_id;
        for (const auto& stream : streams) {
            CHECK(stream.size() <= progressive::default_max_bytes);
            total += stream.size();
            const auto result = decoder.decode(stream, frame_id);
            INFO((result ? std::string("ok") : result.error().message()));
            REQUIRE(result.has_value());
        }
        CHECK(total <= budget);
        if (image != nullptr && quality != nullptr) {
            quality->push_back(psnr(*image, decoder.image()));
        }
    }
    return calls;
}

std::vector<std::byte> single_pass_pixels(const ImageView& image, const progressive::EncoderOptions& options)
{
    progressive::Encoder encoder(image.width, image.height, options);
    auto decoder = progressive::Decoder::create(image.width, image.height);
    REQUIRE(decoder.has_value());
    const std::array full{Rect{.x = 0, .y = 0, .width = image.width, .height = image.height}};
    round_trip(encoder, *decoder, image, full, 1, options.max_bytes);
    return pixels_of(*decoder);
}

}  // namespace

TEST_CASE("Reduce-extrapolate streams round-trip", "[codec][progressive]")
{
    const Image img = natural_image(320, 200);
    progressive::Encoder encoder(320, 200, {.quant = progressive::quant_highest, .reduce_extrapolate = true});
    auto decoder = progressive::Decoder::create(320, 200);
    REQUIRE(decoder.has_value());
    const std::array full{Rect{.x = 0, .y = 0, .width = 320, .height = 200}};
    const auto streams = round_trip(encoder, *decoder, img.view(), full, 1, progressive::default_max_bytes);
    CHECK(streams[0][region_at + 11] == std::byte{1});  // RFX_DWT_REDUCE_EXTRAPOLATE
    CHECK(streams[0][12 + 9] == std::byte{0});          // no RFX_SUBBAND_DIFFING
    const double quality = psnr(img.view(), decoder->image());
    INFO("PSNR " << quality << " dB");
    CHECK(quality >= 40.0);
}

TEST_CASE("Refinement converges to the single-pass picture bit for bit", "[codec][progressive]")
{
    const Image img = natural_image(320, 200);
    const auto target =
        single_pass_pixels(img.view(), {.quant = progressive::quant_highest, .reduce_extrapolate = true});

    progressive::Encoder encoder(320, 200, refine_options);
    auto decoder = progressive::Decoder::create(320, 200);
    REQUIRE(decoder.has_value());
    const std::array full{Rect{.x = 0, .y = 0, .width = 320, .height = 200}};
    const auto first = round_trip(encoder, *decoder, img.view(), full, 1, progressive::default_max_bytes);
    // The first pass: RFX_SUBBAND_DIFFING, four quality tables, TILE_FIRST at
    // quality stage 0.
    CHECK(first[0][12 + 9] == std::byte{1});
    CHECK(first[0][region_at + 10] == std::byte{4});
    CHECK(encoder.pending_tiles() == 20);
    CHECK(encoder.tile_stage(0, 0) == 0);
    CHECK(encoder.tile_stage(4, 3) == 0);
    std::vector<double> quality{psnr(img.view(), decoder->image())};
    CHECK(quality[0] >= 25.0);
    CHECK(pixels_of(*decoder) != target);

    std::uint32_t frame_id = 1;
    const auto view = img.view();
    const std::size_t calls = refine_fully(encoder, *decoder, frame_id, std::size_t{1} << 20, &view, &quality);
    CHECK(calls == 1);  // a large budget takes every tile to full quality at once
    CHECK(encoder.tile_stage(0, 0) == progressive::full_quality_stage);
    CHECK(pixels_of(*decoder) == target);
    INFO("PSNR first pass " << quality.front() << " dB, final " << quality.back() << " dB");
    CHECK(quality.back() > quality.front());
    CHECK(quality.back() >= 40.0);
    CHECK(encoder.upgrade(std::size_t{1} << 20).empty());
}

TEST_CASE("A direct pass is the single-pass picture at once and owes no upgrade", "[codec][progressive]")
{
    const Image img = natural_image(320, 200);
    const auto target =
        single_pass_pixels(img.view(), {.quant = progressive::quant_highest, .reduce_extrapolate = true});
    const std::array full{Rect{.x = 0, .y = 0, .width = 320, .height = 200}};

    progressive::Encoder encoder(320, 200, refine_options);
    auto decoder = progressive::Decoder::create(320, 200);
    REQUIRE(decoder.has_value());
    const auto streams = round_trip(encoder, *decoder, img.view(), full, 1, progressive::default_max_bytes,
                                    progressive::Encoder::Pass::direct);
    REQUIRE(!streams.empty());
    // Nothing is left to refine, and the client already has what a refined
    // encoder would have reached only after every upgrade.
    CHECK(encoder.pending_tiles() == 0);
    CHECK(encoder.tile_stage(0, 0) == progressive::full_quality_stage);
    CHECK(encoder.upgrade(std::size_t{1} << 20).empty());
    CHECK(pixels_of(*decoder) == target);

    // A refined first pass of the same picture is coarser and leaves work behind.
    progressive::Encoder coarse_encoder(320, 200, refine_options);
    auto coarse_decoder = progressive::Decoder::create(320, 200);
    REQUIRE(coarse_decoder.has_value());
    static_cast<void>(round_trip(coarse_encoder, *coarse_decoder, img.view(), full, 1, progressive::default_max_bytes));
    CHECK(coarse_encoder.pending_tiles() == 20);
    CHECK(psnr(img.view(), decoder->image()) > psnr(img.view(), coarse_decoder->image()));

    // A tile that changes again after a direct pass starts over normally.
    Image moved = natural_image(320, 200, 9);
    const std::array corner{Rect{.x = 0, .y = 0, .width = 64, .height = 64}};
    static_cast<void>(encoder.encode(moved.view(), corner));
    CHECK(encoder.pending_tiles() == 1);
    CHECK(encoder.tile_stage(0, 0) == 0);
}

TEST_CASE("Upgrades stay within the budget and raise quality stage by stage", "[codec][progressive]")
{
    const Image img = natural_image(640, 360, 7);
    const auto target =
        single_pass_pixels(img.view(), {.quant = progressive::quant_highest, .reduce_extrapolate = true});
    progressive::Encoder encoder(640, 360, refine_options);
    auto decoder = progressive::Decoder::create(640, 360);
    const std::array full{Rect{.x = 0, .y = 0, .width = 640, .height = 360}};
    const auto first = round_trip(encoder, *decoder, img.view(), full, 1, progressive::default_max_bytes);
    std::size_t first_size = 0;
    for (const auto& s : first) {
        first_size += s.size();
    }
    std::vector<double> quality{psnr(img.view(), decoder->image())};
    std::uint32_t frame_id = 1;
    const auto view = img.view();
    const std::size_t calls = refine_fully(encoder, *decoder, frame_id, 6000, &view, &quality);
    INFO("first pass " << first_size << " bytes, " << calls << " upgrade calls");
    CHECK(calls > 3);
    for (std::size_t i = 1; i < quality.size(); ++i) {
        CHECK(quality[i] >= quality[i - 1] - 0.05);
    }
    CHECK(pixels_of(*decoder) == target);
}

TEST_CASE("Upgrades can be limited to an area", "[codec][progressive]")
{
    const Image img = natural_image(256, 128);
    progressive::Encoder encoder(256, 128, refine_options);
    auto decoder = progressive::Decoder::create(256, 128);
    const std::array full{Rect{.x = 0, .y = 0, .width = 256, .height = 128}};
    round_trip(encoder, *decoder, img.view(), full, 1, progressive::default_max_bytes);
    REQUIRE(encoder.pending_tiles() == 8);
    const std::array left{Rect{.x = 0, .y = 0, .width = 100, .height = 128}};
    const std::array right{Rect{.x = 128, .y = 0, .width = 128, .height = 128}};
    CHECK(encoder.pending(left));
    for (const auto& s : encoder.upgrade(left, std::size_t{1} << 20)) {
        REQUIRE(decoder->decode(s, 2).has_value());
    }
    // Tiles 0 and 1 of both rows are done, the others untouched.
    CHECK(!encoder.pending(left));
    CHECK(encoder.pending(right));
    CHECK(encoder.pending_tiles() == 4);
    CHECK(encoder.tile_stage(1, 1) == progressive::full_quality_stage);
    CHECK(encoder.tile_stage(2, 0) == 0);
    // Another codec paints the right half: its refinement is forgotten.
    encoder.discard(right);
    CHECK(encoder.pending_tiles() == 0);
    CHECK(encoder.upgrade(std::size_t{1} << 20).empty());
}

TEST_CASE("A tile that changes again starts over with TILE_FIRST", "[codec][progressive]")
{
    Image img = natural_image(192, 128);
    progressive::Encoder encoder(192, 128, refine_options);
    auto decoder = progressive::Decoder::create(192, 128);
    const std::array full{Rect{.x = 0, .y = 0, .width = 192, .height = 128}};
    round_trip(encoder, *decoder, img.view(), full, 1, progressive::default_max_bytes);
    std::uint32_t frame_id = 1;
    // Half way: every tile at stage 1 or 2.
    for (const auto& s : encoder.upgrade(std::size_t{1} << 20)) {
        REQUIRE(decoder->decode(s, ++frame_id).has_value());
    }
    REQUIRE(encoder.pending_tiles() == 0);

    for (std::uint32_t y = 10; y < 40; ++y) {
        for (std::uint32_t x = 50; x < 120; ++x) {
            img.set(x, y, 250, static_cast<int>(x), 20);
        }
    }
    const std::array damage{Rect{.x = 50, .y = 10, .width = 70, .height = 30}};
    const auto streams = round_trip(encoder, *decoder, img.view(), damage, ++frame_id, progressive::default_max_bytes);
    REQUIRE(streams.size() == 1);
    CHECK(streams[0][region_at + 12] == std::byte{2});  // tiles (0, 0) and (1, 0)
    CHECK(encoder.pending_tiles() == 2);
    CHECK(encoder.tile_stage(1, 0) == 0);
    CHECK(encoder.tile_stage(2, 0) == progressive::full_quality_stage);
    refine_fully(encoder, *decoder, frame_id, 3000);
    CHECK(pixels_of(*decoder) ==
          single_pass_pixels(img.view(), {.quant = progressive::quant_highest, .reduce_extrapolate = true}));
}

TEST_CASE("Refinement copes with the smallest stream budget", "[codec][progressive]")
{
    constexpr std::uint32_t w = 200;
    constexpr std::uint32_t h = 70;
    Image img(w, h);
    std::uint32_t state = 5;
    for (auto& b : img.data) {
        state = (state * 1664525U) + 1013904223U;
        b = static_cast<std::byte>(state >> 24U);
    }
    for (const std::size_t max_bytes : {progressive::min_max_bytes, std::size_t{3000}}) {
        progressive::Encoder encoder(
            w, h,
            {.quant = progressive::quant_highest, .max_bytes = max_bytes, .reduce_extrapolate = true, .refine = true});
        auto decoder = progressive::Decoder::create(w, h);
        const std::array full{Rect{.x = 0, .y = 0, .width = w, .height = h}};
        round_trip(encoder, *decoder, img.view(), full, 1, max_bytes);
        std::uint32_t frame_id = 1;
        for (int calls = 0; encoder.pending_tiles() > 0; ++calls) {
            REQUIRE(calls < 1000);
            const auto streams = encoder.upgrade(max_bytes * 3);
            ++frame_id;
            for (const auto& s : streams) {
                CHECK(s.size() <= max_bytes);
                REQUIRE(decoder->decode(s, frame_id).has_value());
            }
            if (streams.empty()) {
                break;  // nothing left that fits
            }
        }
        INFO("max_bytes " << max_bytes);
        CHECK(psnr(img.view(), decoder->image()) >= 10.0);
    }
}

TEST_CASE("reset() drops pending refinement", "[codec][progressive]")
{
    const Image img = natural_image(128, 64);
    progressive::Encoder encoder(128, 64, refine_options);
    const std::array full{Rect{.x = 0, .y = 0, .width = 128, .height = 64}};
    CHECK(!encoder.encode(img.view(), full).empty());
    CHECK(encoder.pending_tiles() == 2);
    encoder.reset();
    CHECK(encoder.pending_tiles() == 0);
    CHECK(encoder.upgrade(std::size_t{1} << 20).empty());
    // A flat picture needs no refinement at all.
    const Image flat(128, 64);
    CHECK(!encoder.encode(flat.view(), full).empty());
    CHECK(encoder.pending_tiles() == 0);
}

TEST_CASE("Single-pass encoders ignore upgrade()", "[codec][progressive]")
{
    const Image img = natural_image(64, 64);
    progressive::Encoder encoder(64, 64);
    const std::array full{Rect{.x = 0, .y = 0, .width = 64, .height = 64}};
    CHECK(!encoder.encode(img.view(), full).empty());
    CHECK(encoder.pending_tiles() == 0);
    CHECK(encoder.upgrade(std::size_t{1} << 20).empty());
    CHECK(encoder.tile_stage(0, 0) == progressive::full_quality_stage);
}

TEST_CASE("Refinement keeps BitPos within 15 for coarse quantization", "[codec][progressive]")
{
    // Base factors plus the stages' extra quantization would exceed 15 (a
    // dequantization shift of 16 or more, which decoders reject); the extra
    // quantization is capped per table instead.
    const Image img = natural_image(200, 130, 3);
    for (const std::uint8_t q : {std::uint8_t{12}, std::uint8_t{14}, std::uint8_t{15}}) {
        INFO("quant " << int{q});
        const progressive::EncoderOptions options{
            .quant = rfx::uniform_quant(q), .max_bytes = progressive::min_max_bytes, .reduce_extrapolate = true};
        progressive::EncoderOptions refined = options;
        refined.refine = true;
        progressive::Encoder encoder(200, 130, refined);
        auto decoder = progressive::Decoder::create(200, 130);
        const std::array full{Rect{.x = 0, .y = 0, .width = 200, .height = 130}};
        round_trip(encoder, *decoder, img.view(), full, 1, options.max_bytes);
        std::uint32_t frame_id = 1;
        while (encoder.pending_tiles() > 0) {
            const auto streams = encoder.upgrade(std::size_t{1} << 20);
            REQUIRE(!streams.empty());
            ++frame_id;
            for (const auto& s : streams) {
                CHECK(s.size() <= options.max_bytes);
                const auto result = decoder->decode(s, frame_id);
                INFO((result ? std::string("ok") : result.error().message()));
                REQUIRE(result.has_value());
            }
        }
        CHECK(pixels_of(*decoder) == single_pass_pixels(img.view(), options));
    }
}

TEST_CASE("Tiles refine with the quantization they started with", "[codec][progressive]")
{
    const Image img = natural_image(192, 128, 5);
    progressive::Encoder encoder(192, 128, refine_options);
    auto decoder = progressive::Decoder::create(192, 128);
    const std::array full{Rect{.x = 0, .y = 0, .width = 192, .height = 128}};
    round_trip(encoder, *decoder, img.view(), full, 1, progressive::default_max_bytes);
    encoder.set_quant(progressive::quant_default);
    // A changed tile goes out with the new table, in the same streams as
    // upgrades of tiles that keep the old one.
    const std::array one{Rect{.x = 0, .y = 0, .width = 64, .height = 64}};
    round_trip(encoder, *decoder, img.view(), one, 2, progressive::default_max_bytes);
    std::uint32_t frame_id = 2;
    refine_fully(encoder, *decoder, frame_id, std::size_t{1} << 20);
    // Every tile but (0, 0) matches a single pass at quant_highest.
    const auto target =
        single_pass_pixels(img.view(), {.quant = progressive::quant_highest, .reduce_extrapolate = true});
    const auto got = pixels_of(*decoder);
    for (std::size_t y = 0; y < 128; ++y) {
        const std::size_t from = y < 64 ? 64 : 0;
        const std::size_t at = ((y * 192) + from) * 4;
        CHECK(std::equal(got.begin() + static_cast<std::ptrdiff_t>(at),
                         got.begin() + static_cast<std::ptrdiff_t>(y * 192 * 4 + 192 * 4),
                         target.begin() + static_cast<std::ptrdiff_t>(at)));
    }
    CHECK(psnr(img.view(), decoder->image()) >= 34.0);
}
