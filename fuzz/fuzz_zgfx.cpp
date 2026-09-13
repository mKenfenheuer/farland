// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Input: a flags byte, then messages, each a 2-byte little-endian length and
// that many bytes.
//
// flags bit 0: RDP 8.0 Lite instead of RDP 8.0.
// flags bit 1: round trip. Otherwise every message is an RDP_SEGMENTED_DATA
//              ([MS-RDPEGFX] 2.2.5.1) for one decompressor, which keeps its
//              history from message to message until one fails.
//
// In round trip mode each message is compressed by a compressing and a
// storing compressor and must decompress to itself. Bits 2..7 of the flags
// repeat each message up to 63 more times (RDP 8.0: up to the 65,535 * 64
// bytes that reach MULTIPART and the history wrap in a few runs).

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/codec/zgfx.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <span>
#include <vector>

namespace {

using namespace farland;
using codec::ZgfxCompressor;
using codec::ZgfxDecompressor;
using codec::ZgfxMode;
using codec::ZgfxVariant;

constexpr std::size_t max_output = std::size_t{4} << 20U;

void decompress_all(Reader& r, ZgfxVariant variant)
{
    ZgfxDecompressor d(variant, max_output);
    while (!r.empty()) {
        const auto size = r.u16le();
        if (!size.has_value()) {
            return;
        }
        const auto message = r.bytes(std::min<std::size_t>(*size, r.remaining()));
        const auto out = d.decompress(*message);
        if (!out.has_value()) {
            return;
        }
        FARLAND_ASSERT(out->size() <= max_output);
    }
}

void round_trip(Reader& r, ZgfxVariant variant, unsigned repeat)
{
    ZgfxCompressor compressing(variant, ZgfxMode::compress);
    ZgfxCompressor storing(variant, ZgfxMode::store);
    ZgfxDecompressor from_compressed(variant, max_output * 16);
    ZgfxDecompressor from_stored(variant, max_output * 16);
    const std::size_t max_input = codec::zgfx_max_input_size(variant);
    while (!r.empty()) {
        const auto size = r.u16le();
        if (!size.has_value()) {
            return;
        }
        const auto chunk = *r.bytes(std::min<std::size_t>(*size, r.remaining()));
        std::vector<std::byte> message;
        for (unsigned i = 0; i <= repeat && message.size() + chunk.size() <= max_input; ++i) {
            message.insert(message.end(), chunk.begin(), chunk.end());
        }
        const auto a = compressing.compress(message);
        const auto b = storing.compress(message);
        const auto back_a = from_compressed.decompress(a);
        const auto back_b = from_stored.decompress(b);
        FARLAND_ASSERT(back_a.has_value() && *back_a == message);
        FARLAND_ASSERT(back_b.has_value() && *back_b == message);
        FARLAND_ASSERT(a.size() <= b.size());
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    Reader r(std::as_bytes(std::span(data, size)));
    const auto flags = r.u8();
    if (!flags.has_value()) {
        return 0;
    }
    const auto variant = (*flags & 1U) != 0 ? ZgfxVariant::rdp8_lite : ZgfxVariant::rdp8;
    if ((*flags & 2U) != 0) {
        round_trip(r, variant, *flags >> 2U);
    } else {
        decompress_all(r, variant);
    }
    return 0;
}
