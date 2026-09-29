// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "clipboard_relay.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <poll.h>
#include <thread>

using farland::app::ClipboardRelay;
namespace platform = farland::platform;
namespace ce = platform::clipboard_event;

namespace {

/// A backend that remembers the calls and answers reads at once. Only the
/// desktop thread touches it, as the relay promises.
class FakeBackend final : public platform::Clipboard {
public:
    [[nodiscard]] std::optional<std::vector<std::string>> mime_types() const override { return owner_; }
    void set_selection(const std::vector<std::string>& mime_types) override
    {
        selection = mime_types;
        thread = std::this_thread::get_id();
    }
    void write(std::uint32_t serial, std::optional<std::vector<std::byte>> data) override
    {
        written_serial = serial;
        written = std::move(data);
    }
    [[nodiscard]] std::uint64_t read(const std::string& mime_type) override
    {
        const std::uint64_t id = 1000 + next_++;  // the backend's own numbering
        events_.push_back(ce::ReadFinished{id, std::vector<std::byte>(mime_type.size(), std::byte{0x61})});
        return id;
    }
    [[nodiscard]] std::optional<platform::ClipboardEvent> poll_event() override
    {
        if (events_.empty()) {
            return std::nullopt;
        }
        auto event = std::move(events_.front());
        events_.pop_front();
        return event;
    }
    [[nodiscard]] std::vector<platform::PollFd> poll_fds() const override { return {}; }
    void dispatch() override {}

    void copy_on_the_desktop(std::vector<std::string> types)
    {
        owner_ = types;
        events_.push_back(ce::OwnerChanged{std::move(types)});
    }

    std::vector<std::string> selection;
    std::thread::id thread;
    std::uint32_t written_serial = 0;
    std::optional<std::vector<std::byte>> written;

private:
    std::optional<std::vector<std::string>> owner_;
    std::deque<platform::ClipboardEvent> events_;
    std::uint64_t next_ = 0;
};

/// The next event on the connection thread's side, waiting for it.
platform::ClipboardEvent next_event(platform::Clipboard& proxy)
{
    for (;;) {
        pollfd pfd{.fd = proxy.poll_fds().at(0).fd, .events = POLLIN, .revents = 0};
        REQUIRE(::poll(&pfd, 1, 5000) == 1);
        proxy.dispatch();
        if (auto event = proxy.poll_event()) {
            return *event;
        }
    }
}

}  // namespace

TEST_CASE("The clipboard relay runs the backend on the desktop thread and the protocol against a stand-in")
{
    FakeBackend backend;
    ClipboardRelay relay(backend);
    auto& proxy = relay.proxy();
    std::atomic<bool> quit{false};
    std::mutex backend_mutex;  // the test's own access to the fake, besides the desktop thread's
    std::thread desktop([&] {
        while (!quit.load()) {
            std::vector<pollfd> fds;
            for (const auto& fd : relay.desktop_fds()) {
                fds.push_back(pollfd{fd.fd, fd.events, 0});
            }
            ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 10);
            const std::scoped_lock lock(backend_mutex);
            relay.service_desktop();
        }
    });

    // The client copied: the desktop thread puts it on the desktop.
    CHECK_FALSE(proxy.mime_types());
    proxy.set_selection({"text/plain;charset=utf-8"});
    // A desktop application copies: the stand-in hears of it.
    {
        const std::scoped_lock lock(backend_mutex);
        backend.copy_on_the_desktop({"text/html"});
    }
    auto event = next_event(proxy);
    REQUIRE(std::holds_alternative<ce::OwnerChanged>(event));
    CHECK(proxy.mime_types() == std::vector<std::string>{"text/html"});

    // A paste: the read's ID is the stand-in's, whatever the backend numbered it.
    const auto id = proxy.read("text/html");
    event = next_event(proxy);
    const auto& finished = std::get<ce::ReadFinished>(event);
    CHECK(finished.id == id);
    CHECK(finished.data->size() == std::string("text/html").size());

    // Answering the desktop's request.
    proxy.write(7, std::vector<std::byte>{std::byte{1}, std::byte{2}});
    for (int i = 0; i < 500; ++i) {
        {
            const std::scoped_lock lock(backend_mutex);
            if (backend.written_serial == 7) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    quit.store(true);
    desktop.join();
    CHECK(backend.written_serial == 7);
    CHECK(backend.written->size() == 2);
    CHECK(backend.selection == std::vector<std::string>{"text/plain;charset=utf-8"});
    CHECK(backend.thread != std::this_thread::get_id());
}
