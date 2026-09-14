// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Every RemoteFX kernel, under every instruction set this CPU runs, against
// the straightforward scalar code in rfx_reference.hpp, bit for bit.

#include <farland/codec/image.hpp>
#include <farland/codec/rfx_common.hpp>

#include "codec/rfx_reference.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

namespace rfx = farland::codec::rfx;
namespace ref = farland::test::rfx_reference;
using farland::codec::ImageView;

namespace {

struct Rng {
    std::uint32_t state;
    std::uint32_t next()
    {
        state = (state * 1664525U) + 1013904223U;
        return state;
    }
    /// Full 16-bit range, or small values (as after quantization), or zero.
    std::int16_t coefficient(int kind)
    {
        const std::uint32_t r = next() >> 8U;
        switch (kind) {
        case 0:
            return static_cast<std::int16_t>(static_cast<std::uint16_t>(r));
        case 1:
            return static_cast<std::int16_t>(static_cast<int>(r % 8192U) - 4096);
        default:
            return r % 10U < 7 ? 0 : static_cast<std::int16_t>(static_cast<int>(r % 41U) - 20);
        }
    }
};

rfx::Coefficients random_coefficients(Rng& rng, int kind)
{
    rfx::Coefficients c{};
    for (auto& v : c) {
        v = rng.coefficient(kind);
    }
    return c;
}

/// Runs `body` once per available instruction set, then restores the best.
template <class F>
void for_each_isa(F&& body)
{
    const auto isas = rfx::available_isas();
    REQUIRE(!isas.empty());
    CHECK(isas.front() == rfx::Isa::scalar);
    for (const rfx::Isa isa : isas) {
        rfx::set_isa(isa);
        INFO("instruction set " << rfx::isa_name(isa));
        body();
    }
    rfx::set_isa(isas.back());
}

}  // namespace

TEST_CASE("The best instruction set is the default", "[codec][rfx][simd]")
{
    CHECK(rfx::active_isa() == rfx::available_isas().back());
#if defined(__aarch64__)
    CHECK(rfx::active_isa() == rfx::Isa::neon);
#endif
}

TEST_CASE("RGB to YCbCr kernels match the scalar conversion", "[codec][rfx][simd]")
{
    // Random pixels, on a surface whose right and bottom tiles are partial.
    constexpr std::uint32_t w = 150;
    constexpr std::uint32_t h = 70;
    std::vector<std::byte> pixels(std::size_t{w} * h * 4 + 12);  // stride w * 4 + 12 on the last row too
    Rng rng{5};
    for (auto& b : pixels) {
        b = static_cast<std::byte>(rng.next() >> 24U);
    }
    // Extremes in the first tile.
    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint8_t v = i % 2 == 0 ? 0 : 255;
        pixels[i * 4] = std::byte{v};
        pixels[(i * 4) + 1] = std::byte{static_cast<std::uint8_t>(255 - v)};
        pixels[(i * 4) + 2] = std::byte{v};
    }
    const ImageView image{.data = pixels, .width = w, .height = h - 1, .stride = (std::size_t{w} * 4) + 4};
    for_each_isa([&] {
        for (std::uint32_t ty = 0; ty * 64 < image.height; ++ty) {
            for (std::uint32_t tx = 0; tx * 64 < image.width; ++tx) {
                rfx::Planes got;
                rfx::Planes want;
                rfx::load_tile(image, tx * 64, ty * 64, got);
                ref::load_tile(image, tx * 64, ty * 64, want);
                INFO("tile " << tx << ", " << ty);
                CHECK(got.y == want.y);
                CHECK(got.cb == want.cb);
                CHECK(got.cr == want.cr);
            }
        }
    });
}

TEST_CASE("DWT kernels match the scalar transforms", "[codec][rfx][simd]")
{
    for_each_isa([] {
        Rng rng{11};
        rfx::Coefficients scratch{};
        for (int round = 0; round < 12; ++round) {
            const int kind = round % 3;
            INFO("round " << round);
            const rfx::Coefficients input = random_coefficients(rng, kind);

            auto got = input;
            auto want = input;
            rfx::dwt_encode(got, scratch);
            ref::dwt_encode(want);
            CHECK(got == want);

            got = input;
            want = input;
            rfx::dwt_decode(got, scratch);
            ref::dwt_decode(want);
            CHECK(got == want);

            got = input;
            want = input;
            rfx::dwt_decode_extrapolate(got, scratch);
            ref::dwt_decode_extrapolate(want);
            CHECK(got == want);

            got = input;
            want = input;
            rfx::dwt_encode_extrapolate(got, scratch);
            ref::dwt_encode_extrapolate(want);
            CHECK(got == want);
        }
    });
}

TEST_CASE("Quantization kernels match the scalar code for both layouts", "[codec][rfx][simd]")
{
    for_each_isa([] {
        Rng rng{3};
        for (int round = 0; round < 8; ++round) {
            rfx::Quant quant;
            for (auto& q : quant.bands) {
                q = static_cast<std::uint8_t>(6 + (rng.next() >> 28U) % 10U);
            }
            const rfx::Coefficients input = random_coefficients(rng, round % 2);
            for (const auto* layout : {&rfx::standard_layout, &rfx::extrapolate_layout}) {
                auto got = input;
                auto want = input;
                rfx::quantize(got, quant, *layout);
                ref::quantize(want, quant, *layout);
                CHECK(got == want);
            }
        }
    });
}

TEST_CASE("RLGR encoding matches the scalar encoder", "[codec][rfx][simd]")
{
    Rng rng{17};
    for (int round = 0; round < 24; ++round) {
        std::vector<std::int16_t> data(round % 4 == 3 ? 4096 : 1 + (rng.next() >> 20U));
        const int kind = round % 3;
        for (auto& v : data) {
            v = kind == 0 ? static_cast<std::int16_t>(static_cast<int>(rng.next() >> 18U) - 8192) : rng.coefficient(2);
        }
        if (round % 5 == 0) {
            std::fill(data.begin() + static_cast<std::ptrdiff_t>(data.size() / 2), data.end(), std::int16_t{0});
        }
        for (const auto mode : {rfx::RlgrMode::rlgr1, rfx::RlgrMode::rlgr3}) {
            std::vector<std::byte> got;
            std::vector<std::byte> want;
            rfx::rlgr_encode(mode, data, got);
            ref::rlgr_encode(mode, data, want);
            INFO("round " << round << (mode == rfx::RlgrMode::rlgr1 ? " RLGR1" : " RLGR3"));
            CHECK(got == want);
        }
    }
}

TEST_CASE("The reduce-extrapolate DWT round-trips within a step", "[codec][rfx]")
{
    // Pixels in the 11.5 fixed point of the colour transform: the inverse of
    // FreeRDP's lifting recovers them up to the rounding of the high bands,
    // which adds up over three levels to a fifth of a pixel step (32) for
    // noise and almost nothing for smooth content.
    Rng rng{23};
    rfx::Coefficients scratch{};
    for (int round = 0; round < 6; ++round) {
        rfx::Coefficients original{};
        for (std::size_t i = 0; i < original.size(); ++i) {
            const std::size_t x = i % 64;
            const std::size_t y = i / 64;
            const int smooth = static_cast<int>((x * 37) + (y * 21)) % 4000;
            original[i] = static_cast<std::int16_t>(round % 2 == 0 ? smooth - 2000 : rng.coefficient(1));
        }
        auto data = original;
        rfx::dwt_encode_extrapolate(data, scratch);
        rfx::dwt_decode_extrapolate(data, scratch);
        int worst = 0;
        for (std::size_t i = 0; i < data.size(); ++i) {
            worst = std::max(worst, std::abs(data[i] - original[i]));
        }
        INFO("round " << round);
        CHECK(worst <= (round % 2 == 0 ? 2 : 8));
    }
}
