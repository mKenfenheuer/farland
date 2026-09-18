// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/platform/mutter/headless_shell.hpp>
#include <farland/platform/mutter/mutter_session.hpp>

#include "mutter_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

using farland::platform::mutter::MutterResult;
using farland::platform::mutter::MutterSession;
using farland::platform::mutter::PortalErrc;
using farland::test::start_mock_mutter;
using namespace std::chrono_literals;
namespace mutter = farland::platform::mutter;

namespace {

template <class T>
bool succeeded(const MutterResult<T>& result)
{
    if (!result) {
        UNSCOPED_INFO(std::string(to_string(result.error().code)) + ": " + result.error().message);
    }
    return result.has_value();
}

bool has_call(const std::vector<std::string>& calls, const std::string& call)
{
    return std::ranges::find(calls, call) != calls.end();
}

}  // namespace

TEST_CASE("Mutter session: sessions, virtual monitors and their streams")
{
    const auto mock = start_mock_mutter();
    auto created = MutterSession::create(mock->options());
    REQUIRE(succeeded(created));
    auto& session = **created;
    CHECK(session.capabilities().remote_desktop_version == 1);
    CHECK(session.capabilities().screen_cast_version == 4);
    CHECK(session.capabilities().device_types == 7);
    CHECK(session.capabilities().xkb_keymaps);
    CHECK_FALSE(session.mutter_owner().empty());

    auto first = session.record_virtual();
    REQUIRE(succeeded(first));
    const auto* stream = session.stream(*first);
    REQUIRE(stream != nullptr);
    CHECK(stream->mapping_id.starts_with("mapping-u"));
    CHECK_FALSE(stream->node_id.has_value());  // only once started

    REQUIRE(succeeded(session.start()));
    CHECK(session.process_until([&] { return session.stream(*first)->node_id.has_value(); }, 5s));
    const auto first_node = session.stream(*first)->node_id;

    // Recorded later: started at once.
    auto second = session.record_virtual();
    REQUIRE(succeeded(second));
    CHECK(session.process_until([&] { return session.stream(*second)->node_id.has_value(); }, 5s));
    CHECK(session.stream(*second)->node_id != first_node);
    CHECK(session.stream(*second)->mapping_id != session.stream(*first)->mapping_id);

    const std::string second_path = session.stream(*second)->path;
    session.stop_stream(*second);
    CHECK(session.stream(*second) == nullptr);
    CHECK(mock->wait_for_call("Stream.Stop " + second_path, &session));

    const auto calls = mock->calls();
    REQUIRE(calls.size() >= 6);
    CHECK(calls[0] == "RemoteDesktop.CreateSession");
    CHECK(calls[1].starts_with("ScreenCast.CreateSession remote-desktop-session-id=session-u"));
    CHECK(calls[2] == "RecordVirtual cursor-mode=2 is-platform=true");
    CHECK(calls[3] == "RemoteDesktop.Start");
    CHECK(calls[4] == "RecordVirtual cursor-mode=2 is-platform=true");
    CHECK(calls[5] == "Stream.Start " + second_path);
    CHECK_FALSE(session.closed());
}

TEST_CASE("Mutter session: taking the session's monitors over")
{
    const auto mock = start_mock_mutter();
    auto created = MutterSession::create(mock->options());
    REQUIRE(succeeded(created));
    auto& session = **created;
    REQUIRE(succeeded(session.record_virtual()));
    REQUIRE(succeeded(session.start()));
    // Let Mutter announce the monitor it made for the stream.
    static_cast<void>(session.process_until([] { return false; }, 300ms));

    auto state = session.monitors();
    REQUIRE(succeeded(state));
    // The seat's monitor is the primary one, and ours is next to it.
    REQUIRE(state->monitors.size() == 2);
    const auto seat = std::ranges::find(state->monitors, false, &farland::platform::mutter::Monitor::is_virtual);
    REQUIRE(seat != state->monitors.end());
    CHECK(seat->connector == "Virtual-1");
    CHECK(seat->active);
    CHECK(seat->primary);
    CHECK(seat->width == 1280);
    const auto ours = state->virtual_connectors();
    REQUIRE(ours.size() == 1);
    CHECK(ours.front() == "Meta-0");

    // Ours alone, and primary: the seat's monitor goes off.
    REQUIRE(succeeded(session.set_monitors(state->serial, state->side_by_side(ours))));
    CHECK(mock->wait_for_call("ApplyMonitorsConfig method=1 Meta-0@0,0*", &session));
    auto after = session.monitors();
    REQUIRE(succeeded(after));
    for (const auto& monitor : after->monitors) {
        CHECK(monitor.active == monitor.is_virtual);
        CHECK(monitor.primary == monitor.is_virtual);
    }
    // What was attached besides ours comes back the way it was.
    std::vector<farland::platform::mutter::LogicalMonitor> before;
    for (const auto& entry : state->active_layout()) {
        if (std::ranges::find(ours, entry.connector) == ours.end()) {
            before.push_back(entry);
        }
    }
    REQUIRE(before.size() == 1);
    CHECK(before.front().connector == "Virtual-1");
    CHECK(before.front().scale == 1.25);
    REQUIRE(succeeded(session.set_monitors(after->serial, before)));
    CHECK(mock->wait_for_call("ApplyMonitorsConfig method=1 Virtual-1@0,0*", &session));
}

TEST_CASE("Mutter session: a headless session has only our monitors")
{
    const auto mock = start_mock_mutter({"--no-seat-monitor"});
    auto created = MutterSession::create(mock->options());
    REQUIRE(succeeded(created));
    auto& session = **created;
    REQUIRE(succeeded(session.record_virtual()));
    REQUIRE(succeeded(session.start()));
    // Let Mutter announce the monitor it made for the stream.
    static_cast<void>(session.process_until([] { return false; }, 300ms));
    auto state = session.monitors();
    REQUIRE(succeeded(state));
    REQUIRE(state->monitors.size() == 1);
    CHECK(state->monitors.front().is_virtual);
    CHECK(state->monitors.front().primary);  // nothing to take over
    CHECK(state->active_layout().size() == 1);
}

TEST_CASE("Mutter session: waiting for Mutter")
{
    SECTION("Mutter comes up late")
    {
        const auto mock = start_mock_mutter({"--names-after-ms", "400"});
        auto created = MutterSession::create(mock->options());
        CHECK(succeeded(created));
    }
    SECTION("no Mutter on the bus")
    {
        const auto mock = start_mock_mutter({}, false);
        auto options = mock->options();
        options.timeout = 300ms;
        const auto created = MutterSession::create(options);
        REQUIRE_FALSE(created.has_value());
        CHECK(created.error().code == PortalErrc::unavailable);
    }
    SECTION("the compositor exited")
    {
        const auto mock = start_mock_mutter({}, false);
        auto options = mock->options();
        int asked = 0;
        options.keep_waiting = [&] { return ++asked < 3; };
        const auto started = std::chrono::steady_clock::now();
        const auto created = MutterSession::create(options);
        REQUIRE_FALSE(created.has_value());
        CHECK(std::chrono::steady_clock::now() - started < 5s);
    }
    SECTION("ScreenCast too old for is-platform")
    {
        const auto mock = start_mock_mutter({"--sc-version", "2"});
        const auto created = MutterSession::create(mock->options());
        REQUIRE_FALSE(created.has_value());
        CHECK(created.error().code == PortalErrc::unsupported);
    }
}

TEST_CASE("Mutter session: closed by Mutter")
{
    SECTION("the session closes")
    {
        const auto mock = start_mock_mutter();
        auto created = MutterSession::create(mock->options());
        REQUIRE(succeeded(created));
        auto& session = **created;
        REQUIRE(succeeded(session.record_virtual()));
        REQUIRE(succeeded(session.start()));
        REQUIRE(mock->close_sessions());
        CHECK_FALSE(session.process_until([] { return false; }, 5s));
        CHECK(session.closed());
        CHECK(session.record_virtual().error().code == PortalErrc::closed);
    }
    SECTION("Mutter leaves the bus")
    {
        const auto mock = start_mock_mutter();
        auto created = MutterSession::create(mock->options());
        REQUIRE(succeeded(created));
        auto& session = **created;
        REQUIRE(mock->vanish());
        CHECK_FALSE(session.process_until([] { return false; }, 5s));
        CHECK(session.closed());
    }
    SECTION("destroying the session stops it")
    {
        const auto mock = start_mock_mutter();
        {
            auto created = MutterSession::create(mock->options());
            REQUIRE(succeeded(created));
            REQUIRE(succeeded((*created)->start()));
        }
        CHECK(mock->wait_for_call("RemoteDesktop.Stop"));
    }
}

TEST_CASE("Mutter session: input and keymap")
{
    const auto mock = start_mock_mutter();
    auto created = MutterSession::create(mock->options());
    REQUIRE(succeeded(created));
    auto& session = **created;
    REQUIRE(succeeded(session.start()));

    auto eis = session.connect_to_eis(mutter::device_keyboard | mutter::device_pointer | mutter::device_touchscreen);
    REQUIRE(succeeded(eis));
    CHECK(eis->valid());
    CHECK(has_call(mock->calls(), "ConnectToEIS device-types=7"));

    CHECK_FALSE(mutter::xkb_keymap_for_layout("").has_value());
    CHECK_FALSE(mutter::xkb_keymap_for_layout("de; rm -rf").has_value());
    const auto keymap = mutter::xkb_keymap_for_layout("de(nodeadkeys)");
    if (!keymap) {
        SKIP("built without libxkbcommon, or no XKB data");
    }
    CHECK(keymap->starts_with("xkb_keymap"));
    CHECK(mutter::xkb_keymap_for_layout("us,de").has_value());
    REQUIRE(succeeded(session.set_keymap(*keymap)));
    CHECK(has_call(mock->calls(), "SetKeymap keymap-type=0 nul=true sealed=true text=xkb_keymap xkb-keymap-format=1"));
}

TEST_CASE("Mutter session: empty keymap capabilities still take XKB keymaps")
{
    const auto mock = start_mock_mutter({"--empty-keymap-capabilities"});
    auto created = MutterSession::create(mock->options());
    REQUIRE(succeeded(created));
    CHECK((*created)->capabilities().xkb_keymaps);
}

TEST_CASE("Mutter session: no SetKeymap before GNOME 49")
{
    const auto mock = start_mock_mutter({"--no-keymap"});
    auto created = MutterSession::create(mock->options());
    REQUIRE(succeeded(created));
    CHECK_FALSE((*created)->capabilities().xkb_keymaps);
    const auto set = (*created)->set_keymap("xkb_keymap {};");
    REQUIRE_FALSE(set.has_value());
    CHECK(set.error().code == PortalErrc::unsupported);
}

TEST_CASE("Headless shell: a private bus, and the compositor on it")
{
    // The runtime directory must be the user's own; CI containers may lack one.
    std::filesystem::path temporary;
    if (mutter::user_runtime_dir().empty()) {
        temporary = std::filesystem::temp_directory_path() / ("farland-runtime-" + std::to_string(::getpid()));
        std::filesystem::create_directories(temporary);
        std::filesystem::permissions(temporary, std::filesystem::perms::owner_all);
        ::setenv("XDG_RUNTIME_DIR", temporary.c_str(), 1);  // NOLINT(concurrency-mt-unsafe): single-threaded here
    }
    {
        // A stand-in for gnome-shell: the mock Mutter on the private bus.
        const auto probe = start_mock_mutter({}, false);  // skips without dbus-daemon
        mutter::HeadlessShellOptions options;
        options.command = {"python3", farland::test::mock_mutter_script()};
        auto shell = mutter::HeadlessShell::launch(options);
        REQUIRE(succeeded(shell));
        CHECK((*shell)->bus_address().starts_with("unix:"));
        CHECK((*shell)->running());

        mutter::MutterOptions session_options;
        session_options.bus_address = (*shell)->bus_address();
        session_options.timeout = 20s;
        session_options.keep_waiting = [&] { return (*shell)->running(); };
        auto session = MutterSession::create(session_options);
        REQUIRE(succeeded(session));
        REQUIRE(succeeded((*session)->start()));
        const pid_t pid = (*shell)->pid();
        session->reset();
        shell->reset();
        CHECK(::kill(pid, 0) != 0);  // gone and reaped
    }
    if (!temporary.empty()) {
        ::unsetenv("XDG_RUNTIME_DIR");  // NOLINT(concurrency-mt-unsafe)
        std::filesystem::remove_all(temporary);
    }
}
