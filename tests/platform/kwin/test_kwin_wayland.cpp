// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/platform/kwin/data_control_clipboard.hpp>
#include <farland/platform/kwin/output_management.hpp>
#include <farland/platform/kwin/screencast.hpp>
#include <farland/platform/kwin/wayland_connection.hpp>

#include "fake_kwin.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <variant>
#include <vector>

using namespace farland::platform::kwin;
using farland::test::FakeKWin;
namespace clipboard_event = farland::platform::clipboard_event;

namespace {

constexpr auto timeout = std::chrono::seconds(5);

std::unique_ptr<WaylandConnection> connect(FakeKWin& kwin)
{
    auto connection = WaylandConnection::connect_fd(kwin.take_client_fd(), timeout);
    REQUIRE(connection);
    return std::move(*connection);
}

/// Dispatches until `done` or the time-out.
bool pump(WaylandConnection& connection, const std::function<bool()>& done)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline || connection.broken()) {
            return false;
        }
        pollfd pfd{connection.fd(), POLLIN, 0};
        ::poll(&pfd, 1, 20);
        connection.dispatch();
    }
    return true;
}

std::vector<std::byte> bytes(std::string_view text)
{
    const auto view = std::as_bytes(std::span(text));
    return {view.begin(), view.end()};
}

}  // namespace

TEST_CASE("KWin backend: outputs and the seat", "[kwin]")
{
    FakeKWin kwin;
    const auto connection = connect(kwin);
    REQUIRE(connection->outputs().size() == 1);
    const auto& output = *connection->outputs().front();
    CHECK(output.name == "Virtual-0");
    CHECK(output.width == 1920);
    CHECK(output.height == 1080);
    CHECK(output.ready);
    CHECK(connection->find_output("Virtual-0") == &output);
    CHECK(connection->seat() != nullptr);
    CHECK(connection->find_global("zkde_screencast_unstable_v1") != nullptr);
}

TEST_CASE("KWin backend: screen casting", "[kwin]")
{
    SECTION("a stream gets a PipeWire node and KWin may end it")
    {
        FakeKWin kwin;
        const auto connection = connect(kwin);
        auto screencast = Screencast::create(*connection);
        REQUIRE(screencast);
        const auto stream =
            (*screencast)->stream_output(connection->outputs().front()->proxy, Screencast::Cursor::metadata);
        CHECK(stream->state() == ScreencastStream::State::pending);
        REQUIRE(pump(*connection, [&] { return stream->state() != ScreencastStream::State::pending; }));
        CHECK(stream->state() == ScreencastStream::State::created);
        CHECK(stream->node() == 40);
        kwin.close_streams();
        CHECK(pump(*connection, [&] { return stream->state() == ScreencastStream::State::closed; }));
    }
    SECTION("KWin refuses the stream")
    {
        FakeKWin kwin(FakeKWin::Options{.fail_streams = true});
        const auto connection = connect(kwin);
        auto screencast = Screencast::create(*connection);
        REQUIRE(screencast);
        const auto stream =
            (*screencast)->stream_output(connection->outputs().front()->proxy, Screencast::Cursor::metadata);
        REQUIRE(pump(*connection, [&] { return stream->state() != ScreencastStream::State::pending; }));
        CHECK(stream->state() == ScreencastStream::State::failed);
        CHECK(stream->error() == "no screen casting here");
    }
    SECTION("a virtual output is asked for with its name, description and size")
    {
        FakeKWin kwin;
        const auto connection = connect(kwin);
        auto screencast = Screencast::create(*connection);
        REQUIRE(screencast);
        // The fake offers 5, as KWin offers at least that: the bound version
        // has to reach the request that takes a description.
        CHECK((*screencast)->version() >= Screencast::description_version);
        CHECK((*screencast)->has_virtual_outputs());
        const auto stream =
            (*screencast)->stream_virtual_output("farland-0", "farland screen 1", 1280, 800, 1.0,
                                                 Screencast::Cursor::metadata);
        REQUIRE(stream);
        REQUIRE(pump(*connection, [&] { return stream->state() != ScreencastStream::State::pending; }));
        CHECK(stream->state() == ScreencastStream::State::created);
        const auto asked = kwin.virtual_outputs();
        REQUIRE(asked.size() == 1);
        CHECK(asked.front().name == "farland-0");
        CHECK(asked.front().description == "farland screen 1");
        CHECK(asked.front().width == 1280);
        CHECK(asked.front().height == 800);
        CHECK(asked.front().scale == 1.0);
    }
    SECTION("not granted: no global")
    {
        FakeKWin kwin(FakeKWin::Options{.screencast = false});
        const auto connection = connect(kwin);
        CHECK_FALSE(Screencast::create(*connection));
    }
}

TEST_CASE("KWin backend: resizing an output through a custom mode", "[kwin]")
{
    FakeKWin kwin;
    const auto connection = connect(kwin);
    auto management = OutputManagement::create(*connection);
    REQUIRE(management);
    auto& outputs = **management;
    CHECK(outputs.resizable("Virtual-0"));
    CHECK_FALSE(outputs.resizable("DP-1"));
    CHECK(outputs.size("Virtual-0") == std::pair{1920, 1080});

    outputs.request_size("Virtual-0", 1280, 800);
    CHECK(outputs.busy());
    REQUIRE(pump(*connection, [&] { return !outputs.busy(); }));
    CHECK(outputs.size("Virtual-0") == std::pair{1280, 800});
    CHECK(kwin.current_mode() == std::pair{1280, 800});
    CHECK(kwin.custom_mode_lists() == 1);
    // The wl_output follows.
    REQUIRE(pump(*connection, [&] { return connection->outputs().front()->width == 1280; }));
    CHECK(connection->outputs().front()->height == 800);

    // A size the output has a mode for needs no new custom mode.
    outputs.request_size("Virtual-0", 1920, 1080);
    REQUIRE(pump(*connection, [&] { return !outputs.busy(); }));
    CHECK(outputs.size("Virtual-0") == std::pair{1920, 1080});
    CHECK(kwin.custom_mode_lists() == 1);

    // The current size: nothing to do.
    const auto applied = kwin.configurations();
    outputs.request_size("Virtual-0", 1920, 1080);
    CHECK_FALSE(outputs.busy());
    CHECK(kwin.configurations() == applied);
}

TEST_CASE("KWin backend: laying the session out around a screen", "[kwin]")
{
    // The fake has one output, so this is the plumbing: the requests that go
    // out, that the layout counts as reached once KWin followed, and that
    // restoring puts back what was there. What a layout of several screens
    // ends up looking like is checked against a real KWin with
    // farland-plasma-headless-probe (docs/PLASMA-TAKEOVER.md).
    FakeKWin kwin(FakeKWin::Options{.output_name = "Virtual-farland-0", .width = 1280, .height = 800});
    const auto connection = connect(kwin);
    auto management = OutputManagement::create(*connection);
    REQUIRE(management);
    auto& outputs = **management;
    REQUIRE(pump(*connection, [&] { return outputs.enabled("Virtual-farland-0"); }));

    const std::vector<std::string> ours{"Virtual-farland-0"};
    outputs.request_layout(ours);
    REQUIRE(pump(*connection, [&] {
        outputs.check_timeouts();
        return !outputs.busy();
    }));
    CHECK(kwin.output_enabled());
    CHECK(kwin.position() == std::pair{0, 0});
    // The primary screen, which is where the panel and new windows go.
    CHECK(kwin.priority() == 1);

    // Asking for what it already shows sends nothing more.
    const auto applied = kwin.configurations();
    outputs.request_layout(ours);
    CHECK_FALSE(outputs.busy());
    CHECK(kwin.configurations() == applied);

    outputs.restore_layout(std::chrono::seconds(2));
    CHECK(kwin.output_enabled());
}

TEST_CASE("KWin backend: a refused resize is tried again, then given up", "[kwin]")
{
    FakeKWin kwin(FakeKWin::Options{.refuse_configurations = true});
    const auto connection = connect(kwin);
    auto management = OutputManagement::create(*connection);
    REQUIRE(management);
    auto& outputs = **management;
    outputs.request_size("Virtual-0", 1024, 768);
    // KWin refuses a configuration while its outputs are changing, which is
    // exactly when a client adds or removes a monitor, so a refusal is tried
    // again a few times (check_timeouts() is what takes the next try).
    REQUIRE(pump(*connection, [&] {
        outputs.check_timeouts();
        return !outputs.busy();
    }));
    CHECK(outputs.size("Virtual-0") == std::pair{1920, 1080});
    CHECK(kwin.configurations() > 1);
    // Once given up, the same size is not asked for again.
    const auto asked = kwin.configurations();
    outputs.request_size("Virtual-0", 1024, 768);
    CHECK_FALSE(outputs.busy());
    CHECK(kwin.configurations() == asked);
}

TEST_CASE("KWin backend: the clipboard through ext-data-control", "[kwin]")
{
    std::signal(SIGPIPE, SIG_IGN);  // NOLINT(cert-err33-c)
    FakeKWin kwin;
    const auto connection = connect(kwin);
    auto created = DataControlClipboard::create(*connection);
    REQUIRE(created);
    auto& clipboard = **created;
    // An empty clipboard: known, without types.
    CHECK(clipboard.mime_types() == std::vector<std::string>{});
    CHECK_FALSE(clipboard.poll_event());

    const auto next_event = [&]() -> std::optional<farland::platform::ClipboardEvent> {
        std::optional<farland::platform::ClipboardEvent> event;
        pump(*connection, [&] {
            clipboard.dispatch();
            event = clipboard.poll_event();
            return event.has_value();
        });
        return event;
    };

    SECTION("the desktop copies, the client reads")
    {
        kwin.desktop_copy({"text/plain;charset=utf-8", "text/html"}, "hello from the desktop");
        auto event = next_event();
        REQUIRE(event);
        const auto* owner = std::get_if<clipboard_event::OwnerChanged>(&*event);
        REQUIRE(owner != nullptr);
        CHECK(owner->mime_types == std::vector<std::string>{"text/plain;charset=utf-8", "text/html"});
        CHECK(clipboard.mime_types() == owner->mime_types);

        const auto id = clipboard.read("text/plain;charset=utf-8");
        event = next_event();
        REQUIRE(event);
        const auto* read = std::get_if<clipboard_event::ReadFinished>(&*event);
        REQUIRE(read != nullptr);
        CHECK(read->id == id);
        REQUIRE(read->data);
        CHECK(*read->data == bytes("hello from the desktop"));
    }

    SECTION("the client copies, the desktop pastes")
    {
        clipboard.set_selection({"text/plain;charset=utf-8", "image/png"});
        REQUIRE(pump(*connection, [&] { return kwin.client_selection().size() == 2; }));
        CHECK(kwin.client_selection() == std::vector<std::string>{"text/plain;charset=utf-8", "image/png"});
        // KWin announces the session's own selection back: no OwnerChanged.
        for (int i = 0; i < 5; ++i) {
            static_cast<void>(connection->roundtrip(timeout));
            clipboard.dispatch();
        }
        CHECK_FALSE(clipboard.poll_event());
        CHECK(clipboard.mime_types() == std::nullopt);

        auto pipe = kwin.desktop_paste("text/plain;charset=utf-8");
        REQUIRE(pipe.valid());
        auto event = next_event();
        REQUIRE(event);
        const auto* request = std::get_if<clipboard_event::TransferRequested>(&*event);
        REQUIRE(request != nullptr);
        CHECK(request->mime_type == "text/plain;charset=utf-8");
        clipboard.write(request->serial, bytes("hello from the client"));

        std::string received;
        std::array<char, 256> buffer{};
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            clipboard.dispatch();
            const auto n = ::read(pipe.get(), buffer.data(), buffer.size());
            if (n <= 0 || std::chrono::steady_clock::now() > deadline) {
                break;
            }
            received.append(buffer.data(), static_cast<std::size_t>(n));
        }
        CHECK(received == "hello from the client");

        SECTION("a refused paste closes the pipe")
        {
            auto refused = kwin.desktop_paste("image/png");
            REQUIRE(refused.valid());
            event = next_event();
            REQUIRE(event);
            request = std::get_if<clipboard_event::TransferRequested>(&*event);
            REQUIRE(request != nullptr);
            clipboard.write(request->serial, std::nullopt);
            CHECK(::read(refused.get(), buffer.data(), buffer.size()) == 0);
        }

        SECTION("someone else copies")
        {
            kwin.desktop_copy({"text/plain"}, "later");
            event = next_event();
            REQUIRE(event);
            const auto* owner = std::get_if<clipboard_event::OwnerChanged>(&*event);
            REQUIRE(owner != nullptr);
            CHECK(owner->mime_types == std::vector<std::string>{"text/plain"});
            CHECK(kwin.client_selection().empty());
        }
    }
}
