// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/platform/portal/portal_clipboard.hpp>
#include <farland/platform/portal/portal_session.hpp>

#include "portal_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <poll.h>
#include <string>
#include <vector>

using farland::platform::ClipboardEvent;
using farland::platform::portal::PortalClipboard;
using farland::platform::portal::PortalErrc;
using farland::platform::portal::PortalResult;
using farland::platform::portal::PortalSession;
using farland::test::start_mock_portal;
namespace pev = farland::platform::clipboard_event;
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

/// Dispatches the clipboard until `done()` or five seconds passed.
template <class Done>
bool dispatch_until(PortalSession& session, PortalClipboard& clipboard, Done done)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        clipboard.dispatch();
        if (done()) {
            return true;
        }
        std::vector<pollfd> fds{pollfd{session.fd(), session.events(), 0}};
        for (const auto& fd : clipboard.poll_fds()) {
            fds.push_back(pollfd{fd.fd, fd.events, 0});
        }
        ::poll(fds.data(), fds.size(), 10);
    }
    return false;
}

std::optional<ClipboardEvent> next_event(PortalSession& session, PortalClipboard& clipboard)
{
    std::optional<ClipboardEvent> event;
    static_cast<void>(dispatch_until(session, clipboard, [&] {
        event = clipboard.poll_event();
        return event.has_value();
    }));
    return event;
}

std::vector<std::byte> bytes(const std::string& text)
{
    const auto view = std::as_bytes(std::span(text));
    return {view.begin(), view.end()};
}

std::string text(const std::vector<std::byte>& data)
{
    std::string out;
    for (const auto b : data) {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

}  // namespace

TEST_CASE("Portal clipboard: access is requested before Start")
{
    const auto mock = start_mock_portal();
    PortalSession session;
    auto options = mock->options();
    options.clipboard = true;
    REQUIRE(succeeded(session.start(options)));
    CHECK(session.capabilities().clipboard_version == 1);
    CHECK(session.clipboard_enabled());
    const auto calls = mock->wait_for_calls(5);
    REQUIRE(calls.size() == 5);
    CHECK(calls[3] == "RequestClipboard");
    CHECK(calls[4] == "Start parent_window=");
}

TEST_CASE("Portal clipboard: not granted, or no Clipboard interface")
{
    SECTION("the user did not allow it")
    {
        const auto mock = start_mock_portal({"--no-grant-clipboard"});
        PortalSession session;
        auto options = mock->options();
        options.clipboard = true;
        REQUIRE(succeeded(session.start(options)));
        CHECK_FALSE(session.clipboard_enabled());
        const auto clipboard = PortalClipboard::create(session);
        REQUIRE_FALSE(clipboard.has_value());
        CHECK(clipboard.error().code == PortalErrc::invalid_state);
    }
    SECTION("an older portal")
    {
        const auto mock = start_mock_portal({"--clipboard-version", "0"});
        PortalSession session;
        auto options = mock->options();
        options.clipboard = true;
        REQUIRE(succeeded(session.start(options)));
        CHECK(session.capabilities().clipboard_version == 0);
        CHECK_FALSE(session.clipboard_enabled());
        const auto calls = mock->wait_for_calls(4);
        CHECK(std::ranges::find(calls, "RequestClipboard") == calls.end());
    }
}

TEST_CASE("Portal clipboard: both directions")
{
    const auto mock = start_mock_portal({"--clipboard-owner-at-start"});
    PortalSession session;
    auto options = mock->options();
    options.clipboard = true;
    REQUIRE(succeeded(session.start(options)));
    auto created = PortalClipboard::create(session);
    REQUIRE(succeeded(created));
    auto& clipboard = **created;

    // Announced while the session started.
    auto event = next_event(session, clipboard);
    REQUIRE(event.has_value());
    CHECK(std::get<pev::OwnerChanged>(*event).mime_types == std::vector<std::string>{"text/plain;charset=utf-8"});

    SECTION("reading the desktop's clipboard")
    {
        const std::string big(300'000, 'x');
        REQUIRE(mock->copy({"text/plain;charset=utf-8", "text/html"}, {"h\xC3\xA9llo", big}));
        event = next_event(session, clipboard);
        REQUIRE(event.has_value());
        CHECK(std::get<pev::OwnerChanged>(*event).mime_types ==
              std::vector<std::string>{"text/plain;charset=utf-8", "text/html"});
        CHECK(clipboard.mime_types() == std::vector<std::string>{"text/plain;charset=utf-8", "text/html"});

        const auto small = clipboard.read("text/plain;charset=utf-8");
        event = next_event(session, clipboard);
        REQUIRE(event.has_value());
        auto finished = std::get<pev::ReadFinished>(*event);
        CHECK(finished.id == small);
        CHECK(text(finished.data.value()) == "h\xC3\xA9llo");

        const auto large = clipboard.read("text/html");
        event = next_event(session, clipboard);
        REQUIRE(event.has_value());
        finished = std::get<pev::ReadFinished>(*event);
        CHECK(finished.id == large);
        CHECK(finished.data.value().size() == big.size());

        const auto missing = clipboard.read("image/png");
        event = next_event(session, clipboard);
        REQUIRE(event.has_value());
        CHECK(std::get<pev::ReadFinished>(*event).id == missing);
        CHECK_FALSE(std::get<pev::ReadFinished>(*event).data.has_value());
    }
    SECTION("the desktop pastes the session's clipboard")
    {
        clipboard.set_selection({"text/plain;charset=utf-8", "text/plain"});
        const auto calls = mock->wait_for_calls(6, &session);
        REQUIRE(calls.size() >= 6);
        CHECK(calls[5] == "SetSelection mime_types=text/plain;charset=utf-8,text/plain");
        CHECK_FALSE(clipboard.mime_types().has_value());

        const auto serial = mock->paste("text/plain");
        REQUIRE(serial.has_value());
        event = next_event(session, clipboard);  // not an OwnerChanged for our own selection
        REQUIRE(event.has_value());
        const auto transfer = std::get<pev::TransferRequested>(*event);
        CHECK(transfer.serial == *serial);
        CHECK(transfer.mime_type == "text/plain");
        clipboard.write(*serial, bytes("pasted"));
        std::optional<std::pair<bool, std::string>> result;
        REQUIRE(dispatch_until(session, clipboard, [&] { return (result = mock->written(*serial)).has_value(); }));
        CHECK(*result == std::pair(true, std::string("pasted")));

        const std::string large(1'000'000, 'y');
        const auto big = mock->paste("text/plain");
        REQUIRE(big.has_value());
        static_cast<void>(next_event(session, clipboard));
        clipboard.write(*big, bytes(large));
        CHECK_FALSE(clipboard.poll_fds().empty());  // more than a pipe holds
        REQUIRE(dispatch_until(session, clipboard, [&] { return (result = mock->written(*big)).has_value(); }));
        CHECK(result->first);
        CHECK(result->second.size() == large.size());

        const auto refused = mock->paste("text/plain");
        REQUIRE(refused.has_value());
        static_cast<void>(next_event(session, clipboard));
        clipboard.write(*refused, std::nullopt);
        REQUIRE(dispatch_until(session, clipboard, [&] { return (result = mock->written(*refused)).has_value(); }));
        CHECK_FALSE(result->first);
    }
}
