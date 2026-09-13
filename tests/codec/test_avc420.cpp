// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/error.hpp>
#include <farland/base/hexdump.hpp>
#include <farland/codec/avc420.hpp>
#include <farland/codec/h264_nal.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace avc = farland::codec::avc;
namespace h264 = farland::codec::h264;
using farland::Errc;
using farland::to_hex;
using farland::test::hex;

namespace {

const std::vector<avc::Region> two_regions{
    {.rect = {.left = 0, .top = 0, .right = 64, .bottom = 32},
     .quant = {.qp = 22, .progressive = false, .quality = 78}},
    {.rect = {.left = 64, .top = 0, .right = 128, .bottom = 16},
     .quant = {.qp = 51, .progressive = true, .quality = 49}},
};

// An access unit delimiter, enough bitstream for the container.
const auto aud = hex("00 00 00 01 09 10");

// [MS-RDPEGFX] 2.2.4.4: numRegionRects, regionRects (RDPGFX_RECT16, 2.2.1.2),
// quantQualityVals (2.2.4.4.2: qpVal = qp | r << 6 | p << 7, qualityVal),
// then the Annex B bitstream.
const auto two_region_stream = hex("02 00 00 00"
                                   "00 00 00 00 40 00 20 00"
                                   "40 00 00 00 80 00 10 00"
                                   "16 4e"
                                   "b3 31"
                                   "00 00 00 01 09 10");

Errc error_of(std::span<const std::byte> bytes)
{
    const auto parsed = avc::decode_avc420(bytes);
    REQUIRE_FALSE(parsed.has_value());
    return parsed.error().code;
}

Errc error_of_444(std::span<const std::byte> bytes)
{
    const auto parsed = avc::decode_avc444(bytes);
    REQUIRE_FALSE(parsed.has_value());
    return parsed.error().code;
}

bool same_bytes(std::span<const std::byte> a, std::span<const std::byte> b)
{
    return std::ranges::equal(a, b);
}

}  // namespace

TEST_CASE("RFX_AVC420_BITMAP_STREAM layout")
{
    const auto encoded = avc::encode_avc420(two_regions, aud);
    CHECK(to_hex(encoded) == to_hex(two_region_stream));
    CHECK(encoded.size() == avc::avc420_size(2, aud.size()));

    const auto decoded = avc::decode_avc420(two_region_stream);
    REQUIRE(decoded.has_value());
    CHECK(decoded->regions == two_regions);
    CHECK(same_bytes(decoded->bitstream, aud));
}

TEST_CASE("RFX_AVC420_BITMAP_STREAM without regions")
{
    const auto encoded = avc::encode_avc420({}, aud);
    CHECK(to_hex(encoded) == to_hex(hex("00 00 00 00 00 00 00 01 09 10")));
    const auto decoded = avc::decode_avc420(encoded);
    REQUIRE(decoded.has_value());
    CHECK(decoded->regions.empty());
}

TEST_CASE("RFX_AVC420_BITMAP_STREAM ignores the reserved r bit")
{
    const auto decoded = avc::decode_avc420(hex("01 00 00 00 00 00 00 00 10 00 10 00 56 4e 00 00 01 09 10"));
    REQUIRE(decoded.has_value());
    CHECK(decoded->regions.at(0).quant.qp == 22);
    CHECK_FALSE(decoded->regions.at(0).quant.progressive);
}

TEST_CASE("RFX_AVC420_BITMAP_STREAM rejects malformed input")
{
    CHECK(error_of(hex("01 00 00")) == Errc::truncated);
    // Two regions announced, room for one.
    CHECK(error_of(hex("02 00 00 00 00 00 00 00 10 00 10 00 16 4e 00 00 01 09")) == Errc::invalid_length);
    CHECK(error_of(hex("01 40 00 00 00 00 00 00")) == Errc::limit_exceeded);
    // Empty rectangles: left == right, then top > bottom.
    CHECK(error_of(hex("01 00 00 00 10 00 00 00 10 00 10 00 16 4e 00 00 01 09")) == Errc::invalid_value);
    CHECK(error_of(hex("01 00 00 00 00 00 20 00 10 00 10 00 16 4e 00 00 01 09")) == Errc::invalid_value);
    // qp 52, qualityVal 101.
    CHECK(error_of(hex("01 00 00 00 00 00 00 00 10 00 10 00 34 4e 00 00 01 09")) == Errc::invalid_value);
    CHECK(error_of(hex("01 00 00 00 00 00 00 00 10 00 10 00 16 65 00 00 01 09")) == Errc::invalid_value);
    // No bitstream.
    CHECK(error_of(hex("01 00 00 00 00 00 00 00 10 00 10 00 16 4e")) == Errc::invalid_length);
}

TEST_CASE("RFX_AVC444_BITMAP_STREAM with luma and chroma")
{
    const std::vector<avc::Region> one{two_regions.at(0)};
    const auto chroma_au = hex("00 00 00 01 09 30");
    const auto encoded =
        avc::encode_avc444(avc::Avc444Layout::luma_and_chroma, {.regions = two_regions, .bitstream = aud},
                           avc::Avc420Part{.regions = one, .bitstream = chroma_au});
    // avc420EncodedBitstreamInfo: cbAvc420EncodedBitstream1 = 30 (the whole
    // first stream, metablock included, as FreeRDP reads it), LC = 0.
    CHECK(to_hex(std::span(encoded).first(4)) == to_hex(hex("1e 00 00 00")));
    CHECK(to_hex(std::span(encoded).subspan(4, 30)) == to_hex(two_region_stream));

    const auto decoded = avc::decode_avc444(encoded);
    REQUIRE(decoded.has_value());
    CHECK(decoded->layout == avc::Avc444Layout::luma_and_chroma);
    CHECK(decoded->first.regions == two_regions);
    CHECK(same_bytes(decoded->first.bitstream, aud));
    REQUIRE(decoded->second.has_value());
    CHECK(decoded->second->regions == one);
    CHECK(same_bytes(decoded->second->bitstream, chroma_au));
}

TEST_CASE("RFX_AVC444_BITMAP_STREAM with one picture")
{
    SECTION("luma only (LC 1)")
    {
        const auto encoded = avc::encode_avc444(avc::Avc444Layout::luma, {.regions = two_regions, .bitstream = aud});
        CHECK(to_hex(std::span(encoded).first(4)) == to_hex(hex("1e 00 00 40")));
        const auto decoded = avc::decode_avc444(encoded);
        REQUIRE(decoded.has_value());
        CHECK(decoded->layout == avc::Avc444Layout::luma);
        CHECK(decoded->first.regions == two_regions);
        CHECK_FALSE(decoded->second.has_value());
    }
    SECTION("chroma only (LC 2) carries size 0, as the spec requires")
    {
        const auto encoded = avc::encode_avc444(avc::Avc444Layout::chroma, {.regions = two_regions, .bitstream = aud});
        CHECK(to_hex(std::span(encoded).first(4)) == to_hex(hex("00 00 00 80")));
        const auto decoded = avc::decode_avc444(encoded);
        REQUIRE(decoded.has_value());
        CHECK(decoded->layout == avc::Avc444Layout::chroma);
        CHECK(decoded->first.regions == two_regions);
    }
    SECTION("chroma only with the size, as FreeRDP's server writes it")
    {
        auto bytes = hex("1e 00 00 80");
        bytes.insert(bytes.end(), two_region_stream.begin(), two_region_stream.end());
        const auto decoded = avc::decode_avc444(bytes);
        REQUIRE(decoded.has_value());
        CHECK(decoded->layout == avc::Avc444Layout::chroma);
    }
}

TEST_CASE("RFX_AVC444_BITMAP_STREAM rejects malformed input")
{
    auto with_info = [](std::string_view info) {
        auto bytes = hex(info);
        bytes.insert(bytes.end(), two_region_stream.begin(), two_region_stream.end());
        return bytes;
    };
    CHECK(error_of_444(hex("1e 00 00")) == Errc::truncated);
    CHECK(error_of_444(with_info("1e 00 00 c0")) == Errc::invalid_value);   // LC 3
    CHECK(error_of_444(with_info("1f 00 00 00")) == Errc::invalid_length);  // LC 0, longer than the input
    CHECK(error_of_444(with_info("1d 00 00 40")) == Errc::invalid_length);  // LC 1, wrong size
    CHECK(error_of_444(with_info("05 00 00 80")) == Errc::invalid_length);  // LC 2, neither 0 nor the size
    CHECK(error_of_444(with_info("1e 00 00 00")) == Errc::truncated);       // LC 0 without a second stream
}

TEST_CASE("H.264 coded sizes and quality values")
{
    CHECK(avc::coded_size(1920) == 1920);
    CHECK(avc::coded_size(1080) == 1088);
    CHECK(avc::coded_size(1) == 16);
    CHECK(avc::coded_size(0) == 0);
    CHECK(avc::quality_from_qp(26) == 74);
    CHECK(avc::quality_from_qp(51) == 49);
}

TEST_CASE("Annex B byte streams split into NAL units")
{
    // AUD, SPS, PPS with a 3-byte start code, and an IDR slice followed by a trailing zero byte.
    const auto stream = hex("00 00 00 01 09 10"
                            "00 00 00 01 67 42 c0 1f"
                            "00 00 01 68 ce 3c 80"
                            "00 00 00 01 65 88 84 00 00 03 01 00");
    const auto units = h264::split_annex_b(stream);
    REQUIRE(units.has_value());
    REQUIRE(units->size() == 4);
    CHECK(units->at(0).type == h264::nal_type::aud);
    CHECK(units->at(0).ref_idc == 0);
    CHECK(to_hex(units->at(0).data) == to_hex(hex("09 10")));
    CHECK(units->at(1).type == h264::nal_type::sps);
    CHECK(units->at(1).ref_idc == 3);
    CHECK(units->at(2).type == h264::nal_type::pps);
    CHECK(to_hex(units->at(2).data) == to_hex(hex("68 ce 3c 80")));
    CHECK(units->at(3).type == h264::nal_type::idr);
    CHECK(to_hex(units->at(3).data) == to_hex(hex("65 88 84 00 00 03 01")));
    CHECK(h264::contains_idr(*units));

    const auto p_frame = h264::split_annex_b(hex("00 00 01 41 9a 02"));
    REQUIRE(p_frame.has_value());
    CHECK_FALSE(h264::contains_idr(*p_frame));
}

TEST_CASE("Annex B splitting rejects malformed streams")
{
    auto code = [](std::string_view text) {
        const auto units = h264::split_annex_b(hex(text));
        REQUIRE_FALSE(units.has_value());
        return units.error().code;
    };
    CHECK(code("") == Errc::truncated);
    CHECK(code("00 00 00") == Errc::truncated);
    CHECK(code("09 10") == Errc::invalid_value);
    CHECK(code("00 01 09 10") == Errc::invalid_value);
    CHECK(code("00 00 01") == Errc::invalid_length);
    CHECK(code("00 00 01 00 00 01 09") == Errc::invalid_length);
    CHECK(code("00 00 01 89 10") == Errc::invalid_value);
}
