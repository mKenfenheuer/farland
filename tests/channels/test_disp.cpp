// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/channels/disp.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using farland::Errc;
using farland::test::hex;
namespace disp = farland::channels::disp;

namespace {

/// Two monitors as FreeRDP 3 sends them with /multimon: a primary 1920x1080
/// (the physical size, orientation and scale of a 24" screen) and a portrait
/// 1080x1920 to its left.
const auto two_monitors = hex("02000000 60000000"                             // Type MONITOR_LAYOUT, Length 96
                              "28000000 02000000"                             // MonitorLayoutSize 40, NumMonitors 2
                              "01000000 00000000 00000000 80070000 38040000"  // primary, 0,0, 1920x1080
                              "13020000 29010000 00000000 64000000 64000000"  // 531x297 mm, 0 deg, 100%, 100%
                              "00000000 C8FBFFFF 00000000 38040000 80070000"  // -1080,0, 1080x1920
                              "29010000 13020000 5A000000 7D000000 8C000000"  // 297x531 mm, 90 deg, 125%, 140%
);

}  // namespace

TEST_CASE("[MS-RDPEDISP] 2.2.2.1 DISPLAYCONTROL_CAPS_PDU encodes and decodes")
{
    const disp::CapsPdu caps{
        .max_num_monitors = 8, .max_monitor_area_factor_a = 3840, .max_monitor_area_factor_b = 2160};
    const auto bytes = disp::encode(caps);
    CHECK(bytes == hex("05000000 14000000 08000000 000F0000 70080000"));
    const auto decoded = disp::decode(bytes);
    REQUIRE(decoded.has_value());
    CHECK(std::get<disp::CapsPdu>(*decoded) == caps);
}

TEST_CASE("[MS-RDPEDISP] 2.2.2.2 DISPLAYCONTROL_MONITOR_LAYOUT_PDU with every field")
{
    const auto decoded = disp::decode(two_monitors);
    REQUIRE(decoded.has_value());
    const auto& layout = std::get<disp::MonitorLayoutPdu>(*decoded);
    REQUIRE(layout.monitors.size() == 2);
    const auto& primary = layout.monitors[0];
    CHECK(primary.primary());
    CHECK(primary.left == 0);
    CHECK(primary.top == 0);
    CHECK(primary.width == 1920);
    CHECK(primary.height == 1080);
    CHECK(primary.physical_width == 531);
    CHECK(primary.physical_height == 297);
    CHECK(primary.orientation == disp::orientation::landscape);
    CHECK(primary.desktop_scale_factor == 100);
    CHECK(primary.device_scale_factor == 100);
    const auto& side = layout.monitors[1];
    CHECK_FALSE(side.primary());
    CHECK(side.left == -1080);
    CHECK(side.width == 1080);
    CHECK(side.height == 1920);
    CHECK(side.orientation == disp::orientation::portrait);
    CHECK(side.desktop_scale_factor == 125);
    CHECK(side.device_scale_factor == 140);
    CHECK(disp::encode(layout) == two_monitors);
}

TEST_CASE("[MS-RDPEDISP] 2.2.2.2 decoding is strict about lengths and counts")
{
    SECTION("the header length must be the message size")
    {
        auto bytes = two_monitors;
        bytes.push_back(std::byte{0});
        CHECK(disp::decode(bytes).error().code == Errc::invalid_length);
        bytes.pop_back();
        bytes.pop_back();
        CHECK(disp::decode(bytes).error().code == Errc::invalid_length);
    }
    SECTION("MonitorLayoutSize must be 40")
    {
        auto bytes = two_monitors;
        bytes[8] = std::byte{0x2C};
        CHECK(disp::decode(bytes).error().code == Errc::invalid_value);
    }
    SECTION("NumMonitors must fill the PDU")
    {
        auto bytes = two_monitors;
        bytes[12] = std::byte{1};
        CHECK(disp::decode(bytes).error().code == Errc::invalid_length);
        bytes[12] = std::byte{3};
        CHECK(disp::decode(bytes).error().code == Errc::invalid_length);
    }
    SECTION("no monitors, or more than the server offered")
    {
        CHECK(disp::decode(hex("02000000 10000000 28000000 00000000")).error().code == Errc::invalid_value);
        CHECK(disp::decode(two_monitors, 1).error().code == Errc::limit_exceeded);
        CHECK(disp::decode(two_monitors, 2).has_value());
        auto many = hex("02000000 00000000 28000000 11000000");  // 17 monitors
        many.resize(16 + (17 * 40));
        many[4] = static_cast<std::byte>(many.size() & 0xFFU);
        many[5] = static_cast<std::byte>(many.size() >> 8U);
        CHECK(disp::decode(many, 100).error().code == Errc::limit_exceeded);
    }
    SECTION("truncated and unknown PDUs")
    {
        CHECK(disp::decode(hex("05000000")).error().code == Errc::truncated);
        CHECK(disp::decode(hex("05000000 10000000 08000000 000F0000")).error().code == Errc::truncated);
        CHECK(disp::decode(hex("03000000 08000000")).error().code == Errc::unsupported);
    }
}
