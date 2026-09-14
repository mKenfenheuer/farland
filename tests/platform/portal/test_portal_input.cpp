// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/platform/portal/portal_input.hpp>

#include "portal_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

using farland::platform::portal::default_layout;
using farland::platform::portal::map_to_stream;
using farland::platform::portal::PortalNotifyInput;
using farland::platform::portal::PortalSession;
using farland::platform::portal::PortalStream;
using farland::platform::portal::StreamRegion;
using farland::test::start_mock_portal;

namespace {

PortalStream stream(std::uint32_t node, std::optional<std::pair<std::int32_t, std::int32_t>> position,
                    std::optional<std::pair<std::int32_t, std::int32_t>> size)
{
    PortalStream s;
    s.node_id = node;
    s.position = position;
    s.size = size;
    return s;
}

void check_point(const std::vector<StreamRegion>& layout, double x, double y, std::uint32_t node, double lx, double ly)
{
    CAPTURE(x, y);
    const auto point = map_to_stream(layout, x, y);
    REQUIRE(point.has_value());
    CHECK(point->node_id == node);
    CHECK(point->x == lx);
    CHECK(point->y == ly);
}

}  // namespace

TEST_CASE("Portal streams map to desktop regions")
{
    const std::vector<PortalStream> streams{stream(42, std::pair(0, 0), std::pair(1920, 1080)),
                                            stream(43, std::pair(1920, 0), std::pair(1280, 1024)),
                                            stream(44, std::nullopt, std::pair(1280, 720))};
    const auto layout = default_layout(streams);
    REQUIRE(layout.size() == 3);
    CHECK(layout[0].desktop == farland::platform::Rect{0, 0, 1920, 1080});
    CHECK(layout[1].desktop == farland::platform::Rect{1920, 0, 1280, 1024});
    CHECK(layout[2].desktop == farland::platform::Rect{3200, 0, 1280, 720});

    check_point(layout, 100, 200, 42, 100, 200);
    check_point(layout, 2000, 100, 43, 80, 100);
    check_point(layout, 3300, 10, 44, 100, 10);
    check_point(layout, 1919.5, 1079, 42, 1919, 1079);  // clamped to the last pixel
    check_point(layout, 100, 1500, 42, 100, 1079);      // below the first monitor
    check_point(layout, 5000, 2000, 44, 1279, 719);     // nearest is the virtual monitor
    check_point(layout, -50, -50, 42, 0, 0);
}

TEST_CASE("Portal layout shifts negative positions and scales logical sizes")
{
    const std::vector<PortalStream> streams{stream(1, std::pair(-1280, 100), std::pair(1280, 1024)),
                                            stream(2, std::pair(0, 0), std::pair(1920, 1080))};
    const auto layout = default_layout(streams);
    CHECK(layout[0].desktop == farland::platform::Rect{0, 100, 1280, 1024});
    CHECK(layout[1].desktop == farland::platform::Rect{1280, 0, 1920, 1080});

    // A HiDPI monitor: 3840x2160 desktop pixels, 1920x1080 logical.
    const std::vector<StreamRegion> scaled{
        {.node_id = 7, .desktop = {0, 0, 3840, 2160}, .logical_width = 1920, .logical_height = 1080}};
    check_point(scaled, 1920, 1080, 7, 960, 540);

    const std::vector<StreamRegion> unsized{
        {.node_id = 9, .desktop = {10, 20, 0, 0}, .logical_width = 0, .logical_height = 0}};
    check_point(unsized, 110, 220, 9, 100, 200);
    CHECK_FALSE(map_to_stream({}, 1, 1).has_value());
}

TEST_CASE("Portal Notify* input")
{
    const auto mock = start_mock_portal();
    PortalSession session;
    REQUIRE(session.start(mock->options()).has_value());
    const auto before = mock->wait_for_calls(4).size();

    {
        PortalNotifyInput input(session);
        input.key(30, true);
        input.key(30, false);
        input.pointer_motion_absolute(2000, 100);
        input.pointer_motion_absolute(100.5, 200);
        input.pointer_motion_relative(1.5, -2);
        input.button(0x110, true);
        input.button(0x110, false);
        input.scroll_discrete(0, 60);  // half a notch: nothing yet
        input.scroll_discrete(0, 60);
        input.scroll_discrete(0, 240);
        input.scroll_discrete(0, -60);
        input.scroll_discrete(0, 30);  // direction change drops the -60
        input.scroll_discrete(0, -120);
        input.scroll_discrete(-120, 0);
        input.text(U'a');
        input.key(42, true);  // released when the input goes away
        input.flush();
    }

    const std::vector<std::string> expected{
        "NotifyKeyboardKeycode keycode=30 state=1",
        "NotifyKeyboardKeycode keycode=30 state=0",
        "NotifyPointerMotionAbsolute stream=43 x=80 y=100",
        "NotifyPointerMotionAbsolute stream=42 x=100.5 y=200",
        "NotifyPointerMotion dx=1.5 dy=-2",
        "NotifyPointerButton button=272 state=1",
        "NotifyPointerButton button=272 state=0",
        "NotifyPointerAxisDiscrete axis=0 steps=1",
        "NotifyPointerAxisDiscrete axis=0 steps=2",
        "NotifyPointerAxisDiscrete axis=0 steps=-1",
        "NotifyPointerAxisDiscrete axis=1 steps=-1",
        "NotifyKeyboardKeycode keycode=42 state=1",
        "NotifyKeyboardKeycode keycode=42 state=0",
    };
    const auto calls = mock->wait_for_calls(before + expected.size(), &session);
    REQUIRE(calls.size() == before + expected.size());
    CHECK(std::vector(calls.begin() + static_cast<std::ptrdiff_t>(before), calls.end()) == expected);
}

TEST_CASE("Portal Notify* input drops devices the user did not grant")
{
    const auto mock = start_mock_portal({"--device-types", "1"});
    PortalSession session;
    REQUIRE(session.start(mock->options()).has_value());
    REQUIRE(session.devices() == 1);
    {
        PortalNotifyInput input(session);
        input.pointer_motion_absolute(10, 10);
        input.button(0x110, true);
        input.scroll_discrete(0, 120);
        input.key(30, true);
        input.key(30, false);
        input.flush();
    }
    const auto calls = mock->wait_for_calls(6, &session);
    REQUIRE(calls.size() == 6);
    CHECK(calls[4] == "NotifyKeyboardKeycode keycode=30 state=1");
    CHECK(calls[5] == "NotifyKeyboardKeycode keycode=30 state=0");
}

TEST_CASE("Portal Notify* failures are logged, not fatal")
{
    const auto mock = start_mock_portal();
    PortalSession session;
    REQUIRE(session.start(mock->options()).has_value());
    auto eis = session.connect_to_eis();  // the portal now rejects Notify*
    REQUIRE(eis.has_value());
    PortalNotifyInput input(session);
    input.key(30, true);
    input.key(30, false);
    input.flush();
    const auto calls = mock->wait_for_calls(5, &session);
    CHECK(calls.back() == "ConnectToEIS");
    CHECK_FALSE(session.closed());
}
