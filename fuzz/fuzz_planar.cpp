// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Input: width - 1, height - 1 (each mod 64), a flags byte whose bit 0 selects
// bottom-up rows, then an RDP6_BITMAP_STREAM ([MS-RDPEGDI] 2.2.2.5.1). Every
// stream that decodes must re-encode, in every mode and orientation, to a
// stream that decodes to the same colours.

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/codec/image.hpp>
#include <farland/codec/planar.hpp>

#include "fuzz.hpp"

#include <array>
#include <span>
#include <vector>

namespace {

using namespace farland;
using codec::planar::Mode;
using codec::planar::Orientation;

constexpr std::uint32_t max_side = 64;

void check_round_trip(std::span<const std::byte> pixels, std::uint32_t width, std::uint32_t height)
{
    const codec::ImageView view{.data = pixels, .width = width, .height = height, .stride = std::size_t{width} * 4};
    std::vector<std::byte> back(pixels.size());
    for (const Mode mode : {Mode::raw, Mode::rle, Mode::automatic}) {
        for (const Orientation orientation : {Orientation::top_down, Orientation::bottom_up}) {
            const auto stream = codec::planar::encode(view, {.mode = mode, .orientation = orientation});
            FARLAND_ASSERT(codec::planar::decode(stream, width, height, orientation, back).has_value());
            for (std::size_t i = 0; i < pixels.size(); ++i) {
                // Alpha is dropped by the encoder and comes back as 0xFF.
                FARLAND_ASSERT(back[i] == (i % 4 == 3 ? std::byte{0xFF} : pixels[i]));
            }
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    Reader r(std::as_bytes(std::span(data, size)));
    const auto width_byte = r.u8();
    const auto height_byte = r.u8();
    const auto flags = r.u8();
    if (!width_byte.has_value() || !height_byte.has_value() || !flags.has_value()) {
        return 0;
    }
    const std::uint32_t width = 1U + (*width_byte % max_side);
    const std::uint32_t height = 1U + (*height_byte % max_side);
    const auto orientation = (*flags & 1U) != 0 ? Orientation::bottom_up : Orientation::top_down;

    std::vector<std::byte> pixels(std::size_t{width} * height * 4);
    if (codec::planar::decode(r.rest(), width, height, orientation, pixels).has_value()) {
        check_round_trip(pixels, width, height);
    }
    return 0;
}
