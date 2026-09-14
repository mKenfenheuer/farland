// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/server/display_layout.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <utility>

namespace disp = farland::channels::disp;
namespace gcc = farland::proto::gcc;
using farland::Errc;
using farland::server::DisplayLayout;
using farland::server::DisplayLimits;
using farland::server::letterbox;
using farland::server::PixelRect;

namespace {

disp::MonitorLayout monitor(std::int32_t left, std::int32_t top, std::uint32_t width, std::uint32_t height,
                            bool primary = false)
{
    return disp::MonitorLayout{
        .flags = primary ? disp::monitor_primary : 0, .left = left, .top = top, .width = width, .height = height};
}

}  // namespace

TEST_CASE("Display layout: [MS-RDPEDISP] 2.2.2.2.1 layouts are normalised to a desktop starting at 0,0")
{
    disp::MonitorLayoutPdu pdu{{monitor(0, 0, 1920, 1080, true), monitor(-1080, -420, 1080, 1920)}};
    pdu.monitors[1].orientation = disp::orientation::portrait;
    pdu.monitors[1].desktop_scale_factor = 125;
    pdu.monitors[1].device_scale_factor = 141;  // not one of 100, 140, 180: ignored
    pdu.monitors[1].physical_width = 5;         // below 10 mm: both sizes ignored
    pdu.monitors[1].physical_height = 500;
    const auto layout = DisplayLayout::from_disp(pdu, {});
    REQUIRE(layout.has_value());
    CHECK(layout->width() == 3000);
    CHECK(layout->height() == 1920);
    CHECK(layout->area() == (1920U * 1080U) + (1080U * 1920U));
    const auto& monitors = layout->monitors();
    REQUIRE(monitors.size() == 2);
    CHECK(monitors[0].rect == PixelRect{1080, 420, 1920, 1080});
    CHECK(monitors[0].primary);
    CHECK(monitors[1].rect == PixelRect{0, 0, 1080, 1920});
    CHECK(monitors[1].orientation == 90);
    CHECK(monitors[1].desktop_scale_factor == 125);
    CHECK(monitors[1].device_scale_factor == 0);
    CHECK(monitors[1].physical_width == 0);
    CHECK(monitors[1].physical_height == 0);

    const auto defs = layout->gfx_monitors();
    REQUIRE(defs.size() == 2);
    CHECK(defs[0] == farland::channels::rdpgfx::MonitorDef{1080, 420, 2999, 1499, 1});
    CHECK(defs[1] == farland::channels::rdpgfx::MonitorDef{0, 0, 1079, 1919, 0});
}

TEST_CASE("Display layout: invalid layouts are refused")
{
    const DisplayLimits limits;
    const auto refused = [&](std::vector<disp::MonitorLayout> monitors) {
        return DisplayLayout::from_disp(disp::MonitorLayoutPdu{std::move(monitors)}, limits).error().code;
    };
    CHECK(refused({monitor(0, 0, 1921, 1080)}) == Errc::invalid_value);  // odd width
    CHECK(refused({monitor(0, 0, 198, 1080)}) == Errc::invalid_value);   // too narrow
    CHECK(refused({monitor(0, 0, 1920, 199)}) == Errc::invalid_value);
    CHECK(refused({monitor(0, 0, 8194, 1080)}) == Errc::invalid_value);
    CHECK(refused({monitor(0, 0, 1920, 1080, true), monitor(1920, 0, 1920, 1080, true)}) == Errc::invalid_value);
    CHECK(refused({monitor(0, 0, 1920, 1080), monitor(1900, 0, 1920, 1080)}) == Errc::invalid_value);  // overlap
    CHECK(refused({monitor(0, 0, 1920, 1080), monitor(20000, 0, 1920, 1080)}) == Errc::limit_exceeded);
    CHECK(refused({}) == Errc::invalid_value);

    DisplayLimits small = limits;
    small.max_monitors = 1;
    CHECK(DisplayLayout::from_disp({{monitor(0, 0, 1920, 1080), monitor(1920, 0, 1920, 1080)}}, small).error().code ==
          Errc::limit_exceeded);
    small = limits;
    small.max_monitors = 2;
    small.area_factor_a = 1920;
    small.area_factor_b = 1080;
    CHECK(DisplayLayout::from_disp({{monitor(0, 0, 3840, 2160)}}, small).error().code == Errc::limit_exceeded);
    CHECK(DisplayLayout::from_disp({{monitor(0, 0, 1920, 1080), monitor(1920, 0, 1920, 1080)}}, small).has_value());

    // Without a primary flag, the monitor at the origin is primary.
    const auto layout = DisplayLayout::from_disp({{monitor(-1920, 0, 1920, 1080), monitor(0, 0, 1920, 1080)}}, limits);
    REQUIRE(layout.has_value());
    CHECK_FALSE(layout->monitors()[0].primary);
    CHECK(layout->monitors()[1].primary);
}

TEST_CASE("Display layout: [MS-RDPBCGR] 2.2.1.3.6 CS_MONITOR at connect time, else the desktop size")
{
    gcc::ClientData data;
    data.core.desktop_scale_factor = 150;
    SECTION("no CS_MONITOR")
    {
        const auto layout = DisplayLayout::from_client_data(data, 1280, 800, {});
        REQUIRE(layout.monitors().size() == 1);
        CHECK(layout.monitors()[0].rect == PixelRect{0, 0, 1280, 800});
        CHECK(layout.monitors()[0].desktop_scale_factor == 150);
    }
    SECTION("two monitors, inclusive coordinates, with CS_MONITOR_EX attributes")
    {
        data.monitor = gcc::ClientMonitorData{0, {{0, 0, 1919, 1079, 1}, {1920, 0, 1920 + 1279, 1023, 0}}};
        data.monitor_ex = gcc::ClientMonitorExtendedData{0, {{531, 297, 0, 100, 100}, {376, 301, 0, 175, 180}}};
        const auto layout = DisplayLayout::from_client_data(data, 3200, 1080, {});
        REQUIRE(layout.monitors().size() == 2);
        CHECK(layout.width() == 3200);
        CHECK(layout.height() == 1080);
        CHECK(layout.monitors()[1].rect == PixelRect{1920, 0, 1280, 1024});
        CHECK(layout.monitors()[1].desktop_scale_factor == 175);
        CHECK(layout.monitors()[1].device_scale_factor == 180);
    }
    SECTION("odd widths are fine at connect time")
    {
        data.monitor = gcc::ClientMonitorData{0, {{0, 0, 1364, 767, 1}}};
        CHECK(DisplayLayout::from_client_data(data, 1365, 768, {}).monitors()[0].rect == PixelRect{0, 0, 1365, 768});
    }
    SECTION("overlapping monitors fall back to the desktop size")
    {
        data.monitor = gcc::ClientMonitorData{0, {{0, 0, 1919, 1079, 1}, {100, 0, 2019, 1079, 0}}};
        const auto layout = DisplayLayout::from_client_data(data, 2020, 1080, {});
        REQUIRE(layout.monitors().size() == 1);
        CHECK(layout.monitors()[0].rect == PixelRect{0, 0, 2020, 1080});
    }
}

TEST_CASE("Display layout: letterboxing centres and never enlarges")
{
    const PixelRect monitor{100, 50, 1920, 1200};
    auto box = letterbox(monitor, 1920, 1200);
    CHECK(box.picture == monitor);
    CHECK(box.bars.empty());

    box = letterbox(monitor, 1280, 720);  // smaller: centred at its own size
    CHECK(box.picture == PixelRect{100 + 320, 50 + 240, 1280, 720});
    REQUIRE(box.bars.size() == 4);
    CHECK(box.bars[0] == PixelRect{100, 50, 1920, 240});
    CHECK(box.bars[1] == PixelRect{100, 50 + 240 + 720, 1920, 240});
    CHECK(box.bars[2] == PixelRect{100, 290, 320, 720});
    CHECK(box.bars[3] == PixelRect{100 + 320 + 1280, 290, 320, 720});

    box = letterbox(monitor, 3840, 2160);  // wider: full width, bars above and below
    CHECK(box.picture == PixelRect{100, 50 + 60, 1920, 1080});
    CHECK(box.bars.size() == 2);

    box = letterbox({0, 0, 1080, 1920}, 1920, 1080);  // landscape on portrait
    CHECK(box.picture == PixelRect{0, 656, 1080, 608});

    box = letterbox({0, 0, 1000, 1000}, 3000, 2000);  // taller monitor: rounds the height
    CHECK(box.picture == PixelRect{0, 166, 1000, 667});
}

TEST_CASE("Display layout: screens go on the primary monitor, or left to right")
{
    const auto layout = DisplayLayout::from_disp(
        {{monitor(0, 0, 1920, 1080, true), monitor(-1280, 0, 1280, 1024), monitor(1920, 0, 1920, 1200)}}, {});
    REQUIRE(layout.has_value());
    CHECK(layout->screen_monitors(1) == std::vector<std::size_t>{0});
    CHECK(layout->screen_monitors(2) == std::vector<std::size_t>{1, 0});
    CHECK(layout->screen_monitors(5) == std::vector<std::size_t>{1, 0, 2});

    // One 2560x1440 screen: scaled onto the primary, the others black.
    const std::array one{std::pair<std::uint32_t, std::uint32_t>{2560, 1440}};
    auto out = layout->place(one);
    CHECK(out.width == 5120);
    CHECK(out.monitors.size() == 3);
    REQUIRE(out.screens.size() == 1);
    CHECK(out.screens[0].target == PixelRect{1280, 0, 1920, 1080});
    CHECK(out.screens[0].scaled());
    CHECK(out.fills == std::vector<PixelRect>{{0, 0, 1280, 1024}, {3200, 0, 1920, 1200}});

    // Three screens of the monitors' sizes: exact, no fills. A screen
    // without a picture yet leaves its monitor black.
    const std::array three{std::pair<std::uint32_t, std::uint32_t>{1280, 1024},
                           std::pair<std::uint32_t, std::uint32_t>{0, 0},
                           std::pair<std::uint32_t, std::uint32_t>{1920, 1200}};
    out = layout->place(three);
    REQUIRE(out.screens.size() == 2);
    CHECK(out.screens[0].screen == 0);
    CHECK(out.screens[0].target == PixelRect{0, 0, 1280, 1024});
    CHECK_FALSE(out.screens[0].scaled());
    CHECK(out.screens[1].screen == 2);
    CHECK(out.screens[1].target == PixelRect{3200, 0, 1920, 1200});
    CHECK(out.fills == std::vector<PixelRect>{{1280, 0, 1920, 1080}});
}
