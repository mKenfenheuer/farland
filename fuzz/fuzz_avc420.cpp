// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Input: a selector byte, then (selector mod 3)
//   0: an RFX_AVC420_BITMAP_STREAM ([MS-RDPEGFX] 2.2.4.4),
//   1: an RFX_AVC444(V2)_BITMAP_STREAM (2.2.4.5, 2.2.4.6),
//   2: an H.264 Annex B byte stream.
// Every stream that parses must re-encode to a stream that parses to the same
// values, and the H.264 bitstreams inside are split into NAL units as well.
// AVC444 region rectangles that fit a 64x64 picture are also applied to one
// the way a client combines the views (codec/yuv444.hpp), in both versions.

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/codec/avc420.hpp>
#include <farland/codec/h264_nal.hpp>
#include <farland/codec/yuv.hpp>
#include <farland/codec/yuv444.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <optional>
#include <span>

namespace {

using namespace farland;
using namespace farland::codec;

void split(std::span<const std::byte> bitstream)
{
    const auto units = h264::split_annex_b(bitstream);
    if (!units.has_value()) {
        return;
    }
    FARLAND_ASSERT(!units->empty() && units->size() <= h264::max_nal_units);
    for (const auto& unit : *units) {
        FARLAND_ASSERT(!unit.data.empty() && unit.data.back() != std::byte{0});
        FARLAND_ASSERT(unit.data.data() >= bitstream.data() &&
                       unit.data.data() + unit.data.size() <= bitstream.data() + bitstream.size());
    }
}

void check_same(const avc::Avc420Stream& a, const avc::Avc420Stream& b)
{
    FARLAND_ASSERT(a.regions == b.regions);
    FARLAND_ASSERT(std::ranges::equal(a.bitstream, b.bitstream));
}

void check_avc420(std::span<const std::byte> input)
{
    const auto stream = avc::decode_avc420(input);
    if (!stream.has_value()) {
        return;
    }
    split(stream->bitstream);
    const auto bytes = avc::encode_avc420(stream->regions, stream->bitstream);
    const auto again = avc::decode_avc420(bytes);
    FARLAND_ASSERT(again.has_value());
    check_same(*stream, *again);
}

/// Combines two constant 64x64 views over the regions that fit, as a client
/// would, and checks that the samples land where they belong.
void combine_regions(const avc::Avc444Stream& stream)
{
    constexpr std::uint32_t side = 64;
    Yuv420Frame main(side, side);
    Yuv420Frame aux(side, side);
    std::ranges::fill(main.y(), std::byte{1});
    std::ranges::fill(main.u(), std::byte{2});
    std::ranges::fill(main.v(), std::byte{3});
    std::ranges::fill(aux.y(), std::byte{4});
    std::ranges::fill(aux.u(), std::byte{5});
    std::ranges::fill(aux.v(), std::byte{6});
    const auto usable = [](const avc::Region& region) {
        const auto& r = region.rect;
        return r.right <= side && r.bottom <= side && r.left % 2 == 0 && r.top % 2 == 0;
    };
    for (const auto version : {Avc444Version::v1, Avc444Version::v2}) {
        Yuv444Frame out(side, side);
        const auto* luma = stream.layout == avc::Avc444Layout::chroma ? nullptr : &stream.first;
        const auto* chroma = stream.layout == avc::Avc444Layout::luma_and_chroma ? &*stream.second
                             : stream.layout == avc::Avc444Layout::chroma        ? &stream.first
                                                                                 : nullptr;
        if (luma != nullptr) {
            for (const auto& region : luma->regions) {
                if (usable(region)) {
                    apply_main_view(main.view(), region.rect, out);
                }
            }
        }
        if (chroma != nullptr) {
            for (const auto& region : chroma->regions) {
                if (usable(region)) {
                    apply_aux_view(aux.view(), version, region.rect, out);
                }
            }
        }
        const auto y = out.view().y;
        FARLAND_ASSERT(std::ranges::all_of(y, [](std::byte b) { return b == std::byte{0} || b == std::byte{1}; }));
    }
}

void check_avc444(std::span<const std::byte> input)
{
    const auto stream = avc::decode_avc444(input);
    if (!stream.has_value()) {
        return;
    }
    FARLAND_ASSERT(stream->second.has_value() == (stream->layout == avc::Avc444Layout::luma_and_chroma));
    split(stream->first.bitstream);
    std::optional<avc::Avc420Part> second;
    if (stream->second.has_value()) {
        split(stream->second->bitstream);
        second = avc::Avc420Part{.regions = stream->second->regions, .bitstream = stream->second->bitstream};
    }
    const auto bytes = avc::encode_avc444(
        stream->layout, {.regions = stream->first.regions, .bitstream = stream->first.bitstream}, second);
    const auto again = avc::decode_avc444(bytes);
    FARLAND_ASSERT(again.has_value() && again->layout == stream->layout);
    check_same(stream->first, again->first);
    FARLAND_ASSERT(again->second.has_value() == stream->second.has_value());
    if (stream->second.has_value()) {
        check_same(*stream->second, *again->second);
    }
    combine_regions(*stream);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    Reader r(std::as_bytes(std::span(data, size)));
    const auto selector = r.u8();
    if (!selector.has_value()) {
        return 0;
    }
    switch (*selector % 3) {
    case 0:
        check_avc420(r.rest());
        break;
    case 1:
        check_avc444(r.rest());
        break;
    default:
        split(r.rest());
        break;
    }
    return 0;
}
