// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/platform/mutter/mutter_clipboard.hpp>
#include <farland/platform/mutter/mutter_session.hpp>

#include "mutter_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <poll.h>
#include <span>
#include <string>
#include <vector>

using farland::platform::ClipboardEvent;
using farland::platform::mutter::MutterClipboard;
using farland::platform::mutter::MutterResult;
using farland::platform::mutter::MutterSession;
using farland::test::start_mock_mutter;
namespace pev = farland::platform::clipboard_event;
using namespace std::chrono_literals;

namespace {

template <class T>
bool succeeded(const MutterResult<T>& result)
{
    if (!result) {
        UNSCOPED_INFO(std::string(to_string(result.error().code)) + ": " + result.error().message);
    }
    return result.has_value();
}

template <class Done>
bool dispatch_until(MutterSession& session, MutterClipboard& clipboard, Done done)
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

std::optional<ClipboardEvent> next_event(MutterSession& session, MutterClipboard& clipboard)
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

TEST_CASE("Mutter clipboard: both directions")
{
    const auto mock = start_mock_mutter({"--clipboard-owner-at-start"});
    auto created = MutterSession::create(mock->options());
    REQUIRE(succeeded(created));
    auto& session = **created;
    REQUIRE(succeeded(session.start()));
    auto made = MutterClipboard::create(session);
    REQUIRE(succeeded(made));
    auto& clipboard = **made;

    // Announced while the clipboard was enabled.
    auto event = next_event(session, clipboard);
    REQUIRE(event.has_value());
    CHECK(std::get<pev::OwnerChanged>(*event).mime_types == std::vector<std::string>{"text/plain;charset=utf-8"});

    SECTION("reading the desktop's clipboard, one read after the other")
    {
        const std::string big(300'000, 'x');
        REQUIRE(mock->copy({"text/plain;charset=utf-8", "text/html"}, {"h\xC3\xA9llo", big}));
        event = next_event(session, clipboard);
        REQUIRE(event.has_value());
        CHECK(clipboard.mime_types() == std::vector<std::string>{"text/plain;charset=utf-8", "text/html"});

        // Mutter refuses parallel reads; the second waits for the first.
        const auto large = clipboard.read("text/html");
        const auto small = clipboard.read("text/plain;charset=utf-8");
        std::vector<pev::ReadFinished> finished;
        REQUIRE(dispatch_until(session, clipboard, [&] {
            while (auto e = clipboard.poll_event()) {
                finished.push_back(std::get<pev::ReadFinished>(std::move(*e)));
            }
            return finished.size() == 2;
        }));
        CHECK(finished[0].id == large);
        CHECK(finished[0].data.value().size() == big.size());
        CHECK(finished[1].id == small);
        CHECK(text(finished[1].data.value()) == "h\xC3\xA9llo");

        const auto missing = clipboard.read("image/png");
        event = next_event(session, clipboard);
        REQUIRE(event.has_value());
        CHECK(std::get<pev::ReadFinished>(*event).id == missing);
        CHECK_FALSE(std::get<pev::ReadFinished>(*event).data.has_value());

        // Nobody owns the clipboard any more.
        REQUIRE(mock->copy({}, {}));
        event = next_event(session, clipboard);
        REQUIRE(event.has_value());
        CHECK(std::get<pev::OwnerChanged>(*event).mime_types.empty());
    }
    SECTION("the desktop pastes the session's clipboard")
    {
        clipboard.set_selection({"text/plain;charset=utf-8", "text/plain"});
        REQUIRE(mock->wait_for_call("SetSelection mime-types=text/plain;charset=utf-8,text/plain", &session));
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
        clipboard.write(*big, bytes(large));  // more than a pipe holds: the rest goes out in dispatch()
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

TEST_CASE("Mutter clipboard: enabled once, disabled when it goes")
{
    const auto mock = start_mock_mutter();
    auto created = MutterSession::create(mock->options());
    REQUIRE(succeeded(created));
    REQUIRE(succeeded((*created)->start()));
    {
        auto made = MutterClipboard::create(**created);
        REQUIRE(succeeded(made));
        CHECK(mock->wait_for_call("EnableClipboard"));
    }
    CHECK(mock->wait_for_call("DisableClipboard", created->get()));
}
