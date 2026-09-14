// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/platform/portal/portal_session.hpp>

#include "portal_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <poll.h>
#include <random>
#include <thread>

using farland::platform::portal::CursorMode;
using farland::platform::portal::PortalErrc;
using farland::platform::portal::PortalResult;
using farland::platform::portal::PortalSession;
using farland::test::start_mock_portal;
using namespace std::chrono_literals;

namespace {

template <class T>
bool succeeded(const PortalResult<T>& result)
{
    if (!result) {
        UNSCOPED_INFO(std::string(to_string(result.error().code)) + ": " + result.error().message);
    }
    return result.has_value();
}

template <class T>
PortalErrc error_code(const PortalResult<T>& result)
{
    REQUIRE_FALSE(result.has_value());
    UNSCOPED_INFO(result.error().message);
    return result.error().code;
}

std::filesystem::path temp_dir()
{
    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() / ("farland-portal-" + std::to_string(rd()));
    std::filesystem::create_directories(dir);
    return dir;
}

using Calls = std::vector<std::string>;

}  // namespace

TEST_CASE("Portal session: start, fds and a restore token round trip")
{
    // The mock sends each Response before the method reply, as a fast portal
    // can: only a client that subscribed before calling sees it.
    const auto mock = start_mock_portal({"--response-before-reply"});
    const auto dir = temp_dir();
    const auto token_path = dir / "state" / "farland" / "portal-restore-token";

    {
        PortalSession session;
        REQUIRE(succeeded(session.start(mock->options())));
        CHECK(session.capabilities().remote_desktop_version == 2);
        CHECK(session.capabilities().screen_cast_version == 5);
        CHECK(session.capabilities().device_types == 7);
        CHECK(session.devices() == 7);
        CHECK(session.cursor_mode() == CursorMode::metadata);
        CHECK_FALSE(session.clipboard_enabled());
        CHECK(session.session_handle().starts_with("/org/freedesktop/portal/desktop/session/"));
        REQUIRE(session.streams().size() == 2);
        const auto& first = session.streams()[0];
        CHECK(first.node_id == 42);
        CHECK(first.id == "0");
        CHECK(first.position == std::pair(0, 0));
        CHECK(first.size == std::pair(1920, 1080));
        CHECK(first.source_type == 1);
        CHECK(first.mapping_id == "mapping-42");
        const auto& second = session.streams()[1];
        CHECK(second.node_id == 43);
        CHECK(second.position == std::pair(1920, 0));
        CHECK(second.size == std::pair(1280, 1024));
        REQUIRE(session.restore_token() == "token-1");

        auto pipewire = session.open_pipewire_remote();
        REQUIRE(succeeded(pipewire));
        CHECK(farland::test::read_all(pipewire->get()) == "pipewire");
        auto eis = session.connect_to_eis();
        REQUIRE(succeeded(eis));
        REQUIRE(eis->has_value());
        CHECK(farland::test::read_all((*eis)->get()) == "eis");

        REQUIRE(farland::platform::portal::save_restore_token(token_path, *session.restore_token()).has_value());
        CHECK_FALSE(session.start(mock->options()).has_value());  // only once
    }

    const auto loaded = farland::platform::portal::load_restore_token(token_path);
    REQUIRE(loaded.has_value());
    REQUIRE(*loaded == "token-1");
    {
        PortalSession session;
        auto options = mock->options();
        options.restore_token = *loaded;
        REQUIRE(succeeded(session.start(options)));
        CHECK(session.restore_token() == "token-2");
    }

    const Calls expected{
        "CreateSession",
        "SelectDevices persist_mode=2 types=7",
        "SelectSources cursor_mode=4 multiple=true types=1",
        "Start parent_window=",
        "OpenPipeWireRemote",
        "ConnectToEIS",
        "Session.Close",
        "CreateSession",
        "SelectDevices persist_mode=2 restore_token=token-1 types=7",
        "SelectSources cursor_mode=4 multiple=true types=1",
        "Start parent_window=",
        "Session.Close",
    };
    CHECK(mock->wait_for_calls(expected.size()) == expected);
    std::filesystem::remove_all(dir);
}

TEST_CASE("Portal session: virtual monitor, embedded cursor, options")
{
    const auto mock = start_mock_portal({"--cursor-modes", "3", "--device-types", "3"});
    PortalSession session;
    auto options = mock->options();
    options.virtual_monitor = true;
    options.persist = false;
    options.multiple = false;
    options.parent_window = "x11:1234";
    REQUIRE(succeeded(session.start(options)));
    CHECK(session.cursor_mode() == CursorMode::embedded);
    CHECK(session.devices() == 3);
    CHECK_FALSE(session.restore_token().has_value());
    REQUIRE(session.streams().size() == 3);
    const auto& virtual_stream = session.streams()[2];
    CHECK(virtual_stream.node_id == 44);
    CHECK(virtual_stream.source_type == 4);
    CHECK_FALSE(virtual_stream.position.has_value());
    CHECK(virtual_stream.size == std::pair(1280, 720));
    CHECK(mock->wait_for_calls(4) == Calls{"CreateSession", "SelectDevices types=3",
                                           "SelectSources cursor_mode=2 multiple=false types=5",
                                           "Start parent_window=x11:1234"});
}

TEST_CASE("Portal session: source types the portal lacks")
{
    SECTION("VIRTUAL is only asked for when offered")
    {
        const auto mock = start_mock_portal({"--source-types", "1"});
        PortalSession session;
        auto options = mock->options();
        options.virtual_monitor = true;
        REQUIRE(succeeded(session.start(options)));
        CHECK(mock->wait_for_calls(3).at(2) == "SelectSources cursor_mode=4 multiple=true types=1");
    }
    SECTION("no usable source type fails before creating a session")
    {
        const auto mock = start_mock_portal({"--source-types", "2"});
        PortalSession session;
        CHECK(error_code(session.start(mock->options())) == PortalErrc::unsupported);
        CHECK(mock->calls().empty());
    }
}

TEST_CASE("Portal session: the user cancels or the portal fails")
{
    const auto [response, code] = GENERATE(std::pair("1", PortalErrc::cancelled), std::pair("2", PortalErrc::failed));
    const auto mock = start_mock_portal({"--start-response", response});
    PortalSession session;
    CHECK(error_code(session.start(mock->options())) == code);
    CHECK_FALSE(session.open_pipewire_remote().has_value());
    const auto calls = mock->wait_for_calls(5);
    REQUIRE(calls.size() == 5);
    CHECK(calls[3] == "Start parent_window=");
    CHECK(calls[4] == "Session.Close");
}

TEST_CASE("Portal session: RemoteDesktop version 1 has no EIS and no persistence")
{
    const auto mock = start_mock_portal({"--rd-version", "1", "--sc-version", "1"});
    PortalSession session;
    auto options = mock->options();
    options.restore_token = "old-token";
    REQUIRE(succeeded(session.start(options)));
    CHECK(session.cursor_mode() == CursorMode::none);
    CHECK_FALSE(session.restore_token().has_value());
    CHECK(session.streams().at(0).source_type == 0);  // ScreenCast < v3 does not say
    CHECK(session.streams().at(0).mapping_id.empty());
    const auto eis = session.connect_to_eis();
    REQUIRE(succeeded(eis));
    CHECK_FALSE(eis->has_value());
    CHECK(mock->wait_for_calls(4) == Calls{"CreateSession", "SelectDevices types=7",
                                           "SelectSources multiple=true types=1", "Start parent_window="});
}

TEST_CASE("Portal session: the portal closes the session")
{
    const auto mock = start_mock_portal({"--close-after-ms", "100"});
    PortalSession session;
    int closed_calls = 0;
    session.set_closed_callback([&] { ++closed_calls; });
    REQUIRE(succeeded(session.start(mock->options())));
    CHECK_FALSE(session.closed());

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!session.closed() && std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{.fd = session.fd(), .events = session.events(), .revents = 0};
        REQUIRE(pfd.fd >= 0);
        ::poll(&pfd, 1, 100);
        session.process();
    }
    CHECK(session.closed());
    CHECK(closed_calls == 1);
    const auto pipewire = session.open_pipewire_remote();
    CHECK(error_code(pipewire) == PortalErrc::closed);
}

TEST_CASE("Portal session: Closed while the dialog is open")
{
    const auto mock = start_mock_portal({"--start-never"});
    PortalSession session;
    std::thread closer([&] {
        static_cast<void>(mock->wait_for_calls(4));
        mock->close_sessions();
    });
    CHECK(error_code(session.start(mock->options())) == PortalErrc::closed);
    closer.join();
    CHECK(session.closed());
}

TEST_CASE("Portal session: timeouts and cancel() close the dialog")
{
    const auto mock = start_mock_portal({"--start-never"});
    PortalSession session;
    std::thread canceller;
    PortalErrc expected{};
    auto options = mock->options();
    SECTION("timeout")
    {
        options.timeout = 500ms;
        expected = PortalErrc::timed_out;
    }
    SECTION("cancel() from another thread")
    {
        canceller = std::thread([&] {
            static_cast<void>(mock->wait_for_calls(4));
            session.cancel();
        });
        expected = PortalErrc::aborted;
    }
    const auto started = std::chrono::steady_clock::now();
    CHECK(error_code(session.start(options)) == expected);
    CHECK(std::chrono::steady_clock::now() - started < 10s);
    if (canceller.joinable()) {
        canceller.join();
    }
    const auto calls = mock->wait_for_calls(6);
    REQUIRE(calls.size() == 6);
    CHECK(calls[3] == "Start parent_window=");
    CHECK(calls[4] == "Request.Close");
    CHECK(calls[5] == "Session.Close");
}

TEST_CASE("Portal session: portals before 0.9 choose the request path")
{
    const auto mock = start_mock_portal({"--legacy-request-path"});
    PortalSession session;
    REQUIRE(succeeded(session.start(mock->options())));
    CHECK(session.streams().size() == 2);
}

TEST_CASE("Portal session: errors without a portal")
{
    SECTION("no portal on the bus")
    {
        const auto mock = start_mock_portal({}, false);
        PortalSession session;
        CHECK(error_code(session.start(mock->options())) == PortalErrc::unavailable);
    }
    SECTION("no bus")
    {
        PortalSession session;
        farland::platform::portal::PortalOptions options;
        options.bus_address = "unix:path=/nonexistent/farland-test-bus";
        CHECK(error_code(session.start(options)) == PortalErrc::unavailable);
    }
    SECTION("fds before start()")
    {
        PortalSession session;
        CHECK(error_code(session.open_pipewire_remote()) == PortalErrc::invalid_state);
        CHECK(error_code(session.connect_to_eis()) == PortalErrc::invalid_state);
        CHECK(session.fd() == -1);
        session.process();  // harmless
    }
}
