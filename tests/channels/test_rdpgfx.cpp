// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// RDPGFX PDU codecs. [MS-RDPEGFX] 4 has examples only for ClearCodec,
// Progressive and ZGFX, none for the graphics messages, so the vectors here
// are assembled by hand from the layouts in [MS-RDPEGFX] 2.2, and the Caps
// Advertise from what FreeRDP 3's client sends (rdpgfx_send_supported_caps).

#include <farland/base/hexdump.hpp>
#include <farland/channels/rdpgfx.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

namespace gfx = farland::channels::rdpgfx;
using farland::Errc;
using farland::to_hex;
using farland::test::hex;

namespace {

constexpr std::array payload{std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}};

std::string zeros(std::size_t count)
{
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        out += " 00";
    }
    return out;
}

/// FreeRDP 3 client without H.264, built with cairo/swscale: 8.0 and 8.1
/// without flags, AVC_DISABLED on every 10.x set, 10.1 with its 16 bytes.
const std::string freerdp_advertise = "12 00 00 00 9a 00 00 00 0b 00"
                                      " 04 00 08 00 04 00 00 00 00 00 00 00"  // 8.0
                                      " 05 01 08 00 04 00 00 00 00 00 00 00"  // 8.1
                                      " 02 00 0a 00 04 00 00 00 20 00 00 00"  // 10.0
                                      " 00 01 0a 00 10 00 00 00" +
                                      zeros(16) +                              // 10.1
                                      " 00 02 0a 00 04 00 00 00 20 00 00 00"   // 10.2
                                      " 01 03 0a 00 04 00 00 00 20 00 00 00"   // 10.3
                                      " 00 04 0a 00 04 00 00 00 20 00 00 00"   // 10.4
                                      " 02 05 0a 00 04 00 00 00 20 00 00 00"   // 10.5
                                      " 00 06 0a 00 04 00 00 00 20 00 00 00"   // 10.6
                                      " 01 06 0a 00 04 00 00 00 20 00 00 00"   // 10.6, errata value
                                      " 01 07 0a 00 04 00 00 00 20 00 00 00";  // 10.7

struct Vector {
    std::string name;
    std::string bytes;
    gfx::Pdu pdu;
};

std::vector<Vector> vectors()
{
    using namespace gfx;
    std::vector<Vector> v;
    v.push_back({"WireToSurface1 AVC420 (2.2.2.1)",
                 "01 00 00 00 1c 00 00 00 00 00 0b 00 20 00 00 00 00 40 00 40 00 03 00 00 00 aa bb cc",
                 WireToSurface1{.surface_id = 0,
                                .codec_id = codec::avc420,
                                .pixel_format = pixel_format::xrgb_8888,
                                .dest_rect = {0, 0, 64, 64},
                                .bitmap_data = payload}});
    v.push_back({"WireToSurface2 Progressive (2.2.2.2)",
                 "02 00 00 00 18 00 00 00 00 00 09 00 07 00 00 00 20 03 00 00 00 aa bb cc",
                 WireToSurface2{.surface_id = 0,
                                .codec_id = codec::progressive,
                                .codec_context_id = 7,
                                .pixel_format = pixel_format::xrgb_8888,
                                .bitmap_data = payload}});
    v.push_back({"DeleteEncodingContext (2.2.2.3)", "03 00 00 00 0e 00 00 00 00 00 07 00 00 00",
                 DeleteEncodingContext{.surface_id = 0, .codec_context_id = 7}});
    v.push_back({"SolidFill (2.2.2.4)",
                 "04 00 00 00 20 00 00 00 00 00 ff 00 00 ff 02 00"
                 " 00 00 00 00 10 00 10 00 10 00 10 00 20 00 20 00",
                 SolidFill{.surface_id = 0,
                           .fill_pixel = {.b = 0xFF, .g = 0, .r = 0, .xa = 0xFF},
                           .fill_rects = {{0, 0, 16, 16}, {16, 16, 32, 32}}}});
    v.push_back({"SurfaceToSurface (2.2.2.5)",
                 "05 00 00 00 1a 00 00 00 00 00 01 00 00 00 00 00 08 00 08 00 01 00 10 00 20 00",
                 SurfaceToSurface{
                     .surface_id_src = 0, .surface_id_dest = 1, .rect_src = {0, 0, 8, 8}, .dest_pts = {{16, 32}}}});
    v.push_back(
        {"SurfaceToCache (2.2.2.6)",
         "06 00 00 00 1c 00 00 00 00 00 08 07 06 05 04 03 02 01 01 00 00 00 00 00 08 00 08 00",
         SurfaceToCache{.surface_id = 0, .cache_key = 0x0102030405060708, .cache_slot = 1, .rect_src = {0, 0, 8, 8}}});
    v.push_back({"CacheToSurface (2.2.2.7)", "07 00 00 00 16 00 00 00 01 00 00 00 02 00 00 00 00 00 f8 ff 10 00",
                 CacheToSurface{.cache_slot = 1, .surface_id = 0, .dest_pts = {{0, 0}, {-8, 16}}}});
    v.push_back({"EvictCacheEntry (2.2.2.8)", "08 00 00 00 0a 00 00 00 01 00", EvictCacheEntry{1}});
    v.push_back(
        {"CreateSurface (2.2.2.9)", "09 00 00 00 0f 00 00 00 00 00 80 07 38 04 20",
         CreateSurface{.surface_id = 0, .width = 1920, .height = 1080, .pixel_format = pixel_format::xrgb_8888}});
    v.push_back({"DeleteSurface (2.2.2.10)", "0a 00 00 00 0a 00 00 00 00 00", DeleteSurface{0}});
    v.push_back({"StartFrame (2.2.2.11)", "0b 00 00 00 10 00 00 00 15 e3 22 03 01 00 00 00",
                 StartFrame{.timestamp = make_timestamp(12, 34, 56, 789), .frame_id = 1}});
    v.push_back({"EndFrame (2.2.2.12)", "0c 00 00 00 0c 00 00 00 01 00 00 00", EndFrame{1}});
    v.push_back({"FrameAcknowledge (2.2.2.13)", "0d 00 00 00 14 00 00 00 00 00 00 00 05 00 00 00 05 00 00 00",
                 FrameAcknowledge{.queue_depth = 0, .frame_id = 5, .total_frames_decoded = 5}});
    v.push_back({"ResetGraphics, one monitor, padded to 340 bytes (2.2.2.14)",
                 "0e 00 00 00 54 01 00 00 80 07 00 00 38 04 00 00 01 00 00 00"
                 " 00 00 00 00 00 00 00 00 7f 07 00 00 37 04 00 00 01 00 00 00" +
                     zeros(300),
                 ResetGraphics{.width = 1920, .height = 1080, .monitors = {{0, 0, 1919, 1079, monitor_primary}}}});
    v.push_back({"MapSurfaceToOutput (2.2.2.15)", "0f 00 00 00 14 00 00 00 00 00 00 00 80 07 00 00 00 00 00 00",
                 MapSurfaceToOutput{.surface_id = 0, .output_origin_x = 1920, .output_origin_y = 0}});
    v.push_back({"CacheImportOffer (2.2.2.16)",
                 "10 00 00 00 22 00 00 00 02 00"
                 " 01 00 00 00 00 00 00 00 00 10 00 00 02 00 00 00 00 00 00 80 00 40 00 00",
                 CacheImportOffer{{{1, 0x1000}, {0x8000000000000002, 0x4000}}}});
    v.push_back({"CacheImportReply, nothing imported (2.2.2.17)", "11 00 00 00 0a 00 00 00 00 00", CacheImportReply{}});
    v.push_back({"CacheImportReply (2.2.2.17)", "11 00 00 00 10 00 00 00 03 00 06 00 09 00 02 00",
                 CacheImportReply{{6, 9, 2}}});
    v.push_back({"CapsConfirm 10.7 (2.2.2.19)", "13 00 00 00 14 00 00 00 01 07 0a 00 04 00 00 00 20 00 00 00",
                 CapsConfirm{make_capability_set(cap_version::v10_7, caps_flag::avc_disabled)}});
    v.push_back({"CapsConfirm 10.1 has capsDataLength 16 (2.2.3.4)",
                 "13 00 00 00 20 00 00 00 00 01 0a 00 10 00 00 00" + zeros(16),
                 CapsConfirm{make_capability_set(cap_version::v10_1, 0)}});
    v.push_back(
        {"MapSurfaceToWindow (2.2.2.20)",
         "15 00 00 00 1a 00 00 00 03 00 44 33 22 11 00 00 00 00 00 04 00 00 00 03 00 00",
         MapSurfaceToWindow{.surface_id = 3, .window_id = 0x11223344, .mapped_width = 1024, .mapped_height = 768}});
    v.push_back({"QoeFrameAcknowledge (2.2.2.21)", "16 00 00 00 14 00 00 00 05 00 00 00 e8 03 00 00 10 00 20 00",
                 QoeFrameAcknowledge{.frame_id = 5, .timestamp = 1000, .time_diff_se = 16, .time_diff_edr = 32}});
    v.push_back({"MapSurfaceToScaledOutput (2.2.2.22)",
                 "17 00 00 00 1c 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0f 00 00 70 08 00 00",
                 MapSurfaceToScaledOutput{.surface_id = 0,
                                          .output_origin_x = 0,
                                          .output_origin_y = 0,
                                          .target_width = 3840,
                                          .target_height = 2160}});
    v.push_back({"MapSurfaceToScaledWindow (2.2.2.23)",
                 "18 00 00 00 22 00 00 00 01 00 05 00 00 00 00 00 00 00"
                 " 00 01 00 00 00 01 00 00 00 02 00 00 00 02 00 00",
                 MapSurfaceToScaledWindow{.surface_id = 1,
                                          .window_id = 5,
                                          .mapped_width = 256,
                                          .mapped_height = 256,
                                          .target_width = 512,
                                          .target_height = 512}});
    return v;
}

Errc decode_error(std::string_view text)
{
    const auto bytes = hex(text);
    const auto pdu = gfx::decode_pdu(bytes);
    REQUIRE_FALSE(pdu.has_value());
    return pdu.error().code;
}

}  // namespace

TEST_CASE("Every RDPGFX PDU decodes and re-encodes byte-exact ([MS-RDPEGFX] 2.2.2)")
{
    for (const auto& vector : vectors()) {
        CAPTURE(vector.name);
        const auto bytes = hex(vector.bytes);
        const auto decoded = gfx::decode_pdu(bytes);
        REQUIRE(decoded.has_value());
        CHECK(*decoded == vector.pdu);
        CHECK(gfx::cmd_id(vector.pdu) == std::to_integer<std::uint16_t>(bytes.front()));
        CHECK(to_hex(gfx::encode(vector.pdu)) == to_hex(bytes));
        const auto framed = gfx::frame_pdu(bytes, bytes.size());
        REQUIRE(framed.has_value());
        CHECK(*framed == bytes.size());
    }
}

TEST_CASE("FreeRDP 3's Caps Advertise decodes with every capsDataLength ([MS-RDPEGFX] 2.2.2.18)")
{
    const auto bytes = hex(freerdp_advertise);
    const auto pdu = gfx::decode_pdu(bytes);
    REQUIRE(pdu.has_value());
    const auto& advertise = std::get<gfx::CapsAdvertise>(*pdu);
    REQUIRE(advertise.caps_sets.size() == 11);
    CHECK(advertise.caps_sets[0] == gfx::make_capability_set(gfx::cap_version::v8, 0));
    CHECK(advertise.caps_sets[3] == gfx::make_capability_set(gfx::cap_version::v10_1, 0));
    CHECK(advertise.caps_sets[9].version == gfx::cap_version::v10_6_err);
    CHECK(advertise.caps_sets[10].flags == gfx::caps_flag::avc_disabled);
    CHECK(to_hex(gfx::encode(*pdu)) == to_hex(bytes));
}

TEST_CASE("capsDataLength and flags per capability set version ([MS-RDPEGFX] 2.2.3)")
{
    using namespace gfx::cap_version;
    using namespace gfx::caps_flag;
    for (const std::uint32_t version : {v8, v8_1, v10, v10_2, v10_3, v10_4, v10_5, v10_6, v10_6_err, v10_7}) {
        CAPTURE(gfx::version_name(version));
        CHECK(gfx::caps_data_length(version) == 4U);
    }
    CHECK(gfx::caps_data_length(v10_1) == 16U);
    CHECK_FALSE(gfx::caps_data_length(v11_1).has_value());
    CHECK_FALSE(gfx::caps_data_length(0x00010000).has_value());

    CHECK(gfx::defined_caps_flags(v8) == (thin_client | small_cache));
    CHECK(gfx::defined_caps_flags(v8_1) == (thin_client | small_cache | avc420_enabled));
    CHECK(gfx::defined_caps_flags(v10) == (small_cache | avc_disabled));
    CHECK(gfx::defined_caps_flags(v10_1) == 0);
    CHECK(gfx::defined_caps_flags(v10_2) == (small_cache | avc_disabled));
    CHECK(gfx::defined_caps_flags(v10_3) == (avc_disabled | avc_thin_client));
    CHECK(gfx::defined_caps_flags(v10_6) == (small_cache | avc_disabled | avc_thin_client));
    CHECK(gfx::defined_caps_flags(v10_7) == (small_cache | avc_disabled | avc_thin_client | scaledmap_disable));
    CHECK(gfx::defined_caps_flags(v11_3) == 0);
}

TEST_CASE("Unknown capability set versions are kept raw")
{
    // FreeRDP's WITH_GFX_AZURE 11.1 set: four flag bytes, SCALEDMAP_DISABLE | AVC_DISABLED.
    const auto bytes = hex("12 00 00 00 16 00 00 00 01 00 01 01 0b 00 04 00 00 00 a0 00 00 00");
    const auto pdu = gfx::decode_pdu(bytes);
    REQUIRE(pdu.has_value());
    const auto& set = std::get<gfx::CapsAdvertise>(*pdu).caps_sets.at(0);
    CHECK(set.version == gfx::cap_version::v11_1);
    CHECK(set.flags == 0xA0);
    CHECK(to_hex(set.unknown_data) == "a0 00 00 00");
    CHECK(set == gfx::make_capability_set(gfx::cap_version::v11_1, 0xA0));
    CHECK(to_hex(gfx::encode(*pdu)) == to_hex(bytes));

    // A short unknown set has no flags.
    const auto empty = hex("12 00 00 00 12 00 00 00 01 00 00 00 01 00 00 00 00 00");
    const auto decoded = gfx::decode_pdu(empty);
    REQUIRE(decoded.has_value());
    CHECK(std::get<gfx::CapsAdvertise>(*decoded).caps_sets.at(0).flags == 0);
    CHECK(to_hex(gfx::encode(*decoded)) == to_hex(empty));
}

TEST_CASE("Malformed RDPGFX PDUs are rejected")
{
    // Header.
    CHECK(decode_error("0c 00 00 00 0d 00 00 00 01 00 00 00") == Errc::invalid_length);     // pduLength too big
    CHECK(decode_error("0c 00 00 00 0c 00 00 00 01 00 00 00 00") == Errc::invalid_length);  // pduLength too small
    CHECK(decode_error("0c 00 00 00 0d 00 00 00 01 00 00 00 00") == Errc::trailing_data);   // body too long
    CHECK(decode_error("0c 00 00 00 0b 00 00 00 01 00 00") == Errc::truncated);             // body too short
    CHECK(decode_error("19 00 00 00 0a 00 00 00 00 00") == Errc::unsupported);              // PROTECT_SURFACE
    CHECK(decode_error("0c 00 00") == Errc::truncated);

    // Caps Advertise.
    CHECK(decode_error("12 00 00 00 0a 00 00 00 00 00") == Errc::invalid_value);  // no sets
    CHECK(decode_error("12 00 00 00 1a 00 00 00 01 00 04 00 08 00 08 00 00 00 00 00 00 00 00 00 00 00") ==
          Errc::invalid_length);  // 8.0 with capsDataLength 8
    CHECK(decode_error("12 00 00 00 16 00 00 00 01 00 00 01 0a 00 04 00 00 00 00 00 00 00") ==
          Errc::invalid_length);  // 10.1 with capsDataLength 4, the ZeroVDI bug
    CHECK(decode_error("12 00 00 00 22 00 00 00 02 00 01 07 0a 00 04 00 00 00 00 00 00 00"
                       " 01 07 0a 00 04 00 00 00 00 00 00 00") == Errc::invalid_value);  // 10.7 twice
    CHECK(decode_error("12 00 00 00 0e 00 00 00 01 00 01 00 00 00") == Errc::truncated);
    CHECK(decode_error("12 00 00 00 12 00 00 00 01 00 01 00 00 00 2d 01 00 00") ==
          Errc::limit_exceeded);  // unknown set with 301 bytes
    {
        gfx::CapsAdvertise many;
        for (std::uint32_t i = 0; i <= gfx::max_caps_sets; ++i) {
            many.caps_sets.push_back(gfx::make_capability_set(0x00100000 + i, 0));
        }
        const auto bytes = gfx::encode(many);
        CHECK(gfx::decode_pdu(bytes).error().code == Errc::limit_exceeded);
    }

    // Cache Import Offer: count must be below 5462 and match the data.
    CHECK(decode_error("10 00 00 00 0a 00 00 00 56 15") == Errc::invalid_value);
    CHECK(decode_error("10 00 00 00 16 00 00 00 02 00 01 00 00 00 00 00 00 00 00 10 00 00") == Errc::truncated);

    // Reset Graphics: exactly 340 bytes, at most 16 monitors, size 1..32766.
    CHECK(decode_error("0e 00 00 00 14 00 00 00 80 07 00 00 38 04 00 00 00 00 00 00") == Errc::invalid_length);
    CHECK(decode_error("0e 00 00 00 54 01 00 00 80 07 00 00 38 04 00 00 11 00 00 00" + zeros(320)) ==
          Errc::invalid_value);
    CHECK(decode_error("0e 00 00 00 54 01 00 00 ff 7f 00 00 38 04 00 00 00 00 00 00" + zeros(320)) ==
          Errc::invalid_value);

    // Rectangles, pixel formats, cache slots.
    CHECK(decode_error("04 00 00 00 18 00 00 00 00 00 00 00 00 00 01 00 10 00 10 00 10 00 20 00") ==
          Errc::invalid_value);  // empty fill rectangle
    CHECK(decode_error("09 00 00 00 0f 00 00 00 00 00 80 07 38 04 22") == Errc::invalid_value);
    CHECK(decode_error("09 00 00 00 0f 00 00 00 00 00 00 00 38 04 20") == Errc::invalid_value);
    CHECK(decode_error("08 00 00 00 0a 00 00 00 00 00") == Errc::invalid_value);  // slot 0
    CHECK(decode_error("08 00 00 00 0a 00 00 00 01 64") == Errc::invalid_value);  // slot 25601
    CHECK(decode_error("11 00 00 00 0c 00 00 00 01 00 00 00") == Errc::invalid_value);

    // bitmapDataLength beyond the PDU.
    CHECK(decode_error("01 00 00 00 1c 00 00 00 00 00 0b 00 20 00 00 00 00 40 00 40 00 04 00 00 00 aa bb cc") ==
          Errc::truncated);
}

TEST_CASE("frame_pdu finds PDU boundaries in a stream")
{
    const auto two = hex("0c 00 00 00 0c 00 00 00 01 00 00 00 0c 00 00 00 0c 00 00 00 02 00");
    auto framed = gfx::frame_pdu(two);
    REQUIRE(framed.has_value());
    CHECK(*framed == 12U);
    framed = gfx::frame_pdu(std::span(two).subspan(12));
    REQUIRE(framed.has_value());
    CHECK_FALSE(framed->has_value());  // incomplete
    CHECK_FALSE(gfx::frame_pdu(std::span(two).first(7)).value().has_value());

    const auto below = hex("0c 00 00 00 07 00 00 00");
    CHECK(gfx::frame_pdu(below).error().code == Errc::invalid_length);
    const auto above = hex("10 00 00 00 00 00 02 00");
    CHECK(gfx::frame_pdu(above).error().code == Errc::limit_exceeded);
    CHECK(gfx::frame_pdu(above, 0x20000).value() == std::nullopt);
}

TEST_CASE("Start Frame timestamps ([MS-RDPEGFX] 2.2.2.11)")
{
    CHECK(gfx::make_timestamp(12, 34, 56, 789) == 0x0322E315U);
    CHECK(gfx::make_timestamp(23, 59, 59, 999) == ((23U << 22U) | (59U << 16U) | (59U << 10U) | 999U));
    CHECK(gfx::valid_timestamp(0));
    CHECK(gfx::valid_timestamp(gfx::make_timestamp(23, 59, 59, 999)));
    CHECK_FALSE(gfx::valid_timestamp(1000));        // milliseconds
    CHECK_FALSE(gfx::valid_timestamp(60U << 10U));  // seconds
    CHECK_FALSE(gfx::valid_timestamp(60U << 16U));  // minutes
    CHECK_FALSE(gfx::valid_timestamp(24U << 22U));  // hours
}
