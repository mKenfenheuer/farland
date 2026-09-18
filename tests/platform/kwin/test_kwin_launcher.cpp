// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/platform/kwin/kwin_launcher.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using namespace farland::platform::kwin;

namespace {

PlasmaLaunchOptions options()
{
    PlasmaLaunchOptions o;
    o.width = 1280;
    o.height = 800;
    o.runtime_dir = "/run/user/1000/farland/plasma-7";
    o.data_dir = "/run/user/1000/farland/kwin-data";
    o.client_executable = "/usr/bin/farland-server";
    return o;
}

GetEnv environment(std::map<std::string, std::string> values)
{
    return [values = std::move(values)](const char* name) -> std::optional<std::string> {
        const auto it = values.find(name);
        return it == values.end() ? std::nullopt : std::optional<std::string>(it->second);
    };
}

std::optional<std::optional<std::string>> change(const LaunchPlan& plan, const std::string& name)
{
    const auto it =
        std::ranges::find(plan.environment, name, &std::pair<std::string, std::optional<std::string>>::first);
    if (it == plan.environment.end()) {
        return std::nullopt;
    }
    return it->second;
}

/// True when the plan removes `name` from the environment.
bool removed(const LaunchPlan& plan, const std::string& name)
{
    const auto value = change(plan, name);
    return value.has_value() && !value->has_value();
}

}  // namespace

TEST_CASE("KWin launch: a private bus and KWin's virtual backend with plasma_session")
{
    const auto plan = plan_plasma_launch(options(), environment({}));
    CHECK(plan.bus_address == "unix:path=/run/user/1000/farland/plasma-7/bus");
    CHECK(plan.dbus_argv == std::vector<std::string>{"dbus-daemon", "--session", "--nofork", "--nopidfile",
                                                     "--nosyslog",
                                                     "--address=unix:path=/run/user/1000/farland/plasma-7/bus"});
    CHECK(plan.kwin_argv == std::vector<std::string>{"kwin_wayland_wrapper", "--virtual", "--width", "1280", "--height",
                                                     "800", "--no-lockscreen", "--xwayland",
                                                     "--exit-with-session=plasma_session"});
    CHECK(change(plan, "DBUS_SESSION_BUS_ADDRESS") == std::optional<std::string>(plan.bus_address));
    for (const char* name : {"WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY"}) {
        CHECK(removed(plan, name));
    }
    CHECK(change(plan, "XDG_CURRENT_DESKTOP") == std::optional<std::string>("KDE"));
    CHECK(change(plan, "KDE_SESSION_VERSION") == std::optional<std::string>("6"));
    CHECK_FALSE(change(plan, "KWIN_XKB_DEFAULT_KEYMAP"));
    CHECK_FALSE(change(plan, "KWIN_WAYLAND_NO_PERMISSION_CHECKS"));
}

TEST_CASE("KWin launch: more outputs, no session")
{
    auto o = options();
    o.output_count = 2;
    o.session_command.clear();
    const auto plan = plan_plasma_launch(o, environment({}));
    CHECK(plan.kwin_argv == std::vector<std::string>{"kwin_wayland_wrapper", "--virtual", "--width", "1280", "--height",
                                                     "800", "--output-count", "2", "--no-lockscreen", "--xwayland"});
}

TEST_CASE("KWin launch: the desktop file is found through XDG_DATA_DIRS")
{
    SECTION("without XDG_DATA_DIRS: the XDG default, then ours")
    {
        const auto plan = plan_plasma_launch(options(), environment({}));
        CHECK(change(plan, "XDG_DATA_DIRS") ==
              std::optional<std::string>("/usr/local/share:/usr/share:/run/user/1000/farland/kwin-data"));
    }
    SECTION("appended once to the session's")
    {
        const auto plan =
            plan_plasma_launch(options(), environment({{"XDG_DATA_DIRS", "/usr/share/plasma:/usr/share"}}));
        CHECK(change(plan, "XDG_DATA_DIRS") ==
              std::optional<std::string>("/usr/share/plasma:/usr/share:/run/user/1000/farland/kwin-data"));
        const auto again = plan_plasma_launch(
            options(), environment({{"XDG_DATA_DIRS", "/usr/share:/run/user/1000/farland/kwin-data"}}));
        CHECK(change(again, "XDG_DATA_DIRS") ==
              std::optional<std::string>("/usr/share:/run/user/1000/farland/kwin-data"));
    }
    const auto plan = plan_plasma_launch(options(), environment({}));
    CHECK(plan.desktop_file == "/run/user/1000/farland/kwin-data/applications/org.farland.headless-kwin.desktop");
    CHECK(plan.desktop_file_contents.find("\nExec=/usr/bin/farland-server\n") != std::string::npos);
    CHECK(plan.desktop_file_contents.find("\nX-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1\n") !=
          std::string::npos);
    CHECK(plan.desktop_file_contents.starts_with("[Desktop Entry]\nType=Application\n"));
}

TEST_CASE("KWin launch: Exec quotes paths with spaces")
{
    const auto text = screencast_desktop_file("/home/max/my builds/farland-server");
    CHECK(text.find("\nExec=\"/home/max/my builds/farland-server\"\n") != std::string::npos);
}

TEST_CASE("KWin launch: the keymap layout")
{
    CHECK(split_xkb_layout("de") == std::pair<std::string, std::string>{"de", ""});
    CHECK(split_xkb_layout("de(nodeadkeys)") == std::pair<std::string, std::string>{"de", "nodeadkeys"});
    CHECK(split_xkb_layout("fr:bepo") == std::pair<std::string, std::string>{"fr", "bepo"});

    auto o = options();
    o.keymap_layout = "de(nodeadkeys)";
    auto plan = plan_plasma_launch(o, environment({}));
    CHECK(change(plan, "KWIN_XKB_DEFAULT_KEYMAP") == std::optional<std::string>("1"));
    CHECK(change(plan, "XKB_DEFAULT_LAYOUT") == std::optional<std::string>("de"));
    CHECK(change(plan, "XKB_DEFAULT_VARIANT") == std::optional<std::string>("nodeadkeys"));
    o.keymap_layout = "us";
    plan = plan_plasma_launch(o, environment({{"XKB_DEFAULT_VARIANT", "dvorak"}}));
    CHECK(removed(plan, "XKB_DEFAULT_VARIANT"));
}

TEST_CASE("KWin launch: the socket from KWin's command line")
{
    const std::vector<std::string> wrapped{
        "/usr/bin/kwin_wayland", "--wayland-fd", "7", "--socket", "wayland-1", "--xwayland-fd", "8", "--virtual"};
    CHECK(socket_argument(wrapped) == "wayland-1");
    CHECK(socket_argument(std::vector<std::string>{"kwin_wayland", "--socket=wayland-3"}) == "wayland-3");
    CHECK_FALSE(socket_argument(std::vector<std::string>{"kwin_wayland", "--socket"}));
    CHECK_FALSE(socket_argument(std::vector<std::string>{"kwin_wayland_wrapper", "--virtual"}));
}
