// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Input: a mode byte, then
//  - mode bit 0 clear: width - 1 and height - 1 (each mod 128), then
//    CLEARCODEC_BITMAP_STREAMs ([MS-RDPEGFX] 2.2.4.1), each behind a 16-bit
//    little-endian length (the last one takes whatever is left). All of them
//    go to one decoder, so the V-bar and glyph storage carry over;
//  - mode bit 0 set: width - 1 and height - 1 (each mod 80), a palette size
//    byte, then pixel data. With a palette size of 0 the data are BGR pixels,
//    otherwise indices into that many colours taken from the data (text-like
//    content). The image, then the image with its left half replaced by
//    another part of the data, are encoded (mode bit 1: without the glyph
//    cache) and every stream must decode to exactly the image.

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/codec/clear.hpp>
#include <farland/codec/image.hpp>

#include "fuzz.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace {

using namespace farland;
namespace clear = codec::clear;

void fuzz_decoder(Reader& r)
{
    const auto w = r.u8();
    const auto h = r.u8();
    if (!w || !h) {
        return;
    }
    const std::uint32_t width = 1U + (*w % 128U);
    const std::uint32_t height = 1U + (*h % 128U);
    std::vector<std::byte> out(std::size_t{width} * height * 4);
    clear::Decoder decoder;
    while (!r.empty()) {
        const auto length = r.u16le();
        if (!length) {
            return;
        }
        const auto stream = r.bytes(std::min<std::size_t>(*length, r.remaining()));
        FARLAND_ASSERT(stream.has_value());
        static_cast<void>(decoder.decode(*stream, width, height, out, std::size_t{width} * 4));
    }
}

void fill_image(std::vector<std::byte>& pixels, std::span<const std::byte> data, std::size_t palette_size,
                std::size_t offset)
{
    if (data.empty()) {
        return;
    }
    for (std::size_t i = 0; i < pixels.size() / 4; ++i) {
        for (std::size_t c = 0; c < 3; ++c) {
            std::byte value{};
            if (palette_size == 0) {
                value = data[((i * 3) + c + offset) % data.size()];
            } else {
                const auto index = std::to_integer<std::size_t>(data[(i + offset) % data.size()]) % palette_size;
                value = data[((index * 3) + c) % data.size()];
            }
            pixels[(i * 4) + c] = value;
        }
    }
}

void fuzz_encoder(Reader& r, bool glyphs)
{
    const auto w = r.u8();
    const auto h = r.u8();
    const auto palette = r.u8();
    if (!w || !h || !palette) {
        return;
    }
    const std::uint32_t width = 1U + (*w % 80U);
    const std::uint32_t height = 1U + (*h % 80U);
    const std::size_t stride = std::size_t{width} * 4;
    std::vector<std::byte> pixels(stride * height, std::byte{0x80});
    fill_image(pixels, r.rest(), *palette, 0);

    clear::Encoder encoder;
    clear::Decoder decoder;
    std::vector<std::byte> out(pixels.size());
    for (int pass = 0; pass < 3; ++pass) {
        if (pass == 1) {
            // Change the left half, keep the rest: a partial cache hit.
            std::vector<std::byte> other(pixels.size());
            fill_image(other, r.rest(), *palette, 7);
            for (std::size_t y = 0; y < height; ++y) {
                for (std::size_t x = 0; x < (width + 1) / 2; ++x) {
                    for (std::size_t c = 0; c < 3; ++c) {
                        pixels[(y * stride) + (x * 4) + c] = other[(y * stride) + (x * 4) + c];
                    }
                }
            }
        }
        const codec::ImageView image{.data = pixels, .width = width, .height = height, .stride = stride};
        const auto stream = encoder.encode(image, {.glyph_cache = glyphs});
        FARLAND_ASSERT(decoder.decode(stream, width, height, out, stride).has_value());
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            FARLAND_ASSERT(out[i] == (i % 4 == 3 ? std::byte{0xFF} : pixels[i]));
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    Reader r(std::as_bytes(std::span(data, size)));
    const auto mode = r.u8();
    if (!mode) {
        return 0;
    }
    if ((*mode & 1U) == 0) {
        fuzz_decoder(r);
    } else {
        fuzz_encoder(r, (*mode & 2U) == 0);
    }
    return 0;
}
