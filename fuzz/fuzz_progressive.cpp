// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Input: a mode byte, then
//  - mode bit 0 clear: surface width - 1 and height - 1 (each mod 160), a
//    frame id byte, then an RFX_PROGRESSIVE_BITMAP_STREAM ([MS-RDPEGFX]
//    2.2.4.2) that is decoded twice (once per frame id), so tile state from
//    the first pass meets the second;
//  - mode bit 0 set: width - 1 and height - 1 (each mod 130), a quantization
//    byte and a budget byte, then pixels (repeated to fill the image). The
//    image is encoded and every stream must stay within the budget and decode.

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/codec/image.hpp>
#include <farland/codec/progressive.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace {

using namespace farland;
namespace progressive = codec::progressive;

void fuzz_decoder(Reader& r)
{
    const auto w = r.u8();
    const auto h = r.u8();
    const auto frame = r.u8();
    if (!w || !h || !frame) {
        return;
    }
    auto decoder = progressive::Decoder::create(1U + (*w % 160U), 1U + (*h % 160U));
    FARLAND_ASSERT(decoder.has_value());
    static_cast<void>(decoder->decode(r.rest(), *frame));
    static_cast<void>(decoder->decode(r.rest(), *frame + 1U));
}

void fuzz_encoder(Reader& r)
{
    const auto w = r.u8();
    const auto h = r.u8();
    const auto quant = r.u8();
    const auto budget = r.u8();
    if (!w || !h || !quant || !budget) {
        return;
    }
    const std::uint32_t width = 1U + (*w % 130U);
    const std::uint32_t height = 1U + (*h % 130U);
    std::vector<std::byte> pixels(std::size_t{width} * height * 4, std::byte{0x80});
    const auto rest = r.rest();
    if (!rest.empty()) {
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = rest[i % rest.size()];
        }
    }
    const codec::ImageView image{.data = pixels, .width = width, .height = height, .stride = std::size_t{width} * 4};
    const std::size_t max_bytes =
        progressive::min_max_bytes + ((progressive::max_max_bytes - progressive::min_max_bytes) * *budget / 255U);
    progressive::Encoder encoder(width, height,
                                 {.quant = codec::rfx::uniform_quant(static_cast<std::uint8_t>(6U + (*quant % 10U))),
                                  .max_bytes = max_bytes,
                                  .sync_every_stream = (*quant & 0x80U) != 0});
    auto decoder = progressive::Decoder::create(width, height);
    FARLAND_ASSERT(decoder.has_value());
    const std::array damage{progressive::Rect{.x = 0, .y = 0, .width = width, .height = height}};
    for (const auto& stream : encoder.encode(image, damage)) {
        FARLAND_ASSERT(stream.size() <= max_bytes);
        FARLAND_ASSERT(decoder->decode(stream, 1).has_value());
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
        fuzz_encoder(r);
    }
    return 0;
}
