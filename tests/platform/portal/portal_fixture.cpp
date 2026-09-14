// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "portal_fixture.hpp"

#include <farland/platform/portal/sd_bus.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;  // NOLINT(readability-redundant-declaration)

namespace farland::test {

namespace {

using namespace std::chrono_literals;

/// Spawns `argv` with its stdout on a pipe; returns the pid and the read end.
std::pair<pid_t, int> spawn(const std::vector<std::string>& argv, int& error)
{
    std::array<int, 2> pipe{-1, -1};
    if (::pipe2(pipe.data(), O_CLOEXEC) != 0) {
        error = errno;
        return {-1, -1};
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipe[1], STDOUT_FILENO);
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& arg : argv) {
        args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);
    pid_t pid = -1;
    error = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe[1]);
    if (error != 0) {
        ::close(pipe[0]);
        return {-1, -1};
    }
    return {pid, pipe[0]};
}

/// Reads one line, waiting up to `timeout`; empty on EOF or timeout.
std::string read_line(int fd, std::chrono::milliseconds timeout)
{
    std::string line;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            return {};
        }
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        if (::poll(&pfd, 1, static_cast<int>(left.count())) <= 0) {
            continue;
        }
        char c = 0;
        if (::read(fd, &c, 1) != 1) {
            return {};
        }
        if (c == '\n') {
            return line;
        }
        line += c;
    }
}

void stop(pid_t& pid)
{
    if (pid > 0) {
        ::kill(pid, SIGTERM);
        int status = 0;
        ::waitpid(pid, &status, 0);
        pid = -1;
    }
}

std::string mock_script()
{
    return (std::filesystem::path(FARLAND_TEST_DATA_DIR).parent_path() / "platform" / "portal" / "mock_portal.py")
        .string();
}

/// A connection to the private bus for inspecting the mock.
platform::portal::detail::BusPtr connect(const std::string& address)
{
    sd_bus* raw = nullptr;
    REQUIRE(sd_bus_new(&raw) >= 0);
    platform::portal::detail::BusPtr bus(raw);
    REQUIRE(sd_bus_set_address(raw, address.c_str()) >= 0);
    REQUIRE(sd_bus_set_bus_client(raw, 1) >= 0);
    REQUIRE(sd_bus_start(raw) >= 0);
    return bus;
}

}  // namespace

std::unique_ptr<MockPortal> start_mock_portal(const std::vector<std::string>& args, bool with_portal)
{
    auto mock = std::make_unique<MockPortal>();
    int error = 0;
    auto [bus_pid, bus_out] = spawn({"dbus-daemon", "--session", "--nofork", "--print-address=1"}, error);
    if (bus_pid < 0) {
        SKIP("cannot run dbus-daemon: " << std::strerror(error));
    }
    mock->bus_pid_ = bus_pid;
    mock->address_ = read_line(bus_out, 10s);
    ::close(bus_out);
    if (mock->address_.empty()) {
        SKIP("dbus-daemon --session did not start");
    }
    if (!with_portal) {
        return mock;
    }

    std::vector<std::string> argv{"python3", mock_script(), "--address", mock->address_};
    argv.insert(argv.end(), args.begin(), args.end());
    auto [mock_pid, mock_out] = spawn(argv, error);
    if (mock_pid < 0) {
        SKIP("cannot run python3: " << std::strerror(error));
    }
    mock->mock_pid_ = mock_pid;
    const std::string ready = read_line(mock_out, 20s);
    ::close(mock_out);
    if (ready != "ready") {
        int status = 0;
        ::waitpid(mock_pid, &status, 0);
        mock->mock_pid_ = -1;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 77) {
            SKIP("mock_portal.py needs python3-dbus and PyGObject");
        }
        FAIL("mock_portal.py did not start");
    }
    return mock;
}

MockPortal::~MockPortal()
{
    stop(mock_pid_);
    stop(bus_pid_);
}

std::vector<std::string> MockPortal::calls() const
{
    auto bus = connect(address_);
    platform::portal::detail::BusError error;
    sd_bus_message* raw = nullptr;
    const int r = sd_bus_call_method(bus.get(), "org.freedesktop.portal.Desktop", "/org/farland/Mock",
                                     "org.farland.Mock", "Calls", error.get(), &raw, "");
    platform::portal::detail::MessagePtr reply(raw);
    INFO(error.message());
    REQUIRE(r >= 0);
    char** strv = nullptr;
    REQUIRE(sd_bus_message_read_strv(reply.get(), &strv) >= 0);
    std::vector<std::string> result;
    for (char** s = strv; s != nullptr && *s != nullptr; ++s) {
        result.emplace_back(*s);
        std::free(*s);  // NOLINT(cppcoreguidelines-no-malloc)
    }
    std::free(strv);  // NOLINT(cppcoreguidelines-no-malloc)
    return result;
}

std::vector<std::string> MockPortal::wait_for_calls(std::size_t count, platform::portal::PortalSession* session) const
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (true) {
        if (session != nullptr) {
            session->process();
        }
        auto seen = calls();
        if (seen.size() >= count || std::chrono::steady_clock::now() > deadline) {
            return seen;
        }
        std::this_thread::sleep_for(10ms);
    }
}

void MockPortal::close_sessions() const
{
    auto bus = connect(address_);
    platform::portal::detail::BusError error;
    REQUIRE(sd_bus_call_method(bus.get(), "org.freedesktop.portal.Desktop", "/org/farland/Mock", "org.farland.Mock",
                               "CloseSessions", error.get(), nullptr, "") >= 0);
}

platform::portal::PortalOptions MockPortal::options() const
{
    platform::portal::PortalOptions options;
    options.bus_address = address_;
    options.timeout = 20s;
    return options;
}

std::string read_all(int fd)
{
    std::string text;
    std::array<char, 256> buffer{};
    while (true) {
        const auto n = ::read(fd, buffer.data(), buffer.size());
        if (n <= 0) {
            return text;
        }
        text.append(buffer.data(), static_cast<std::size_t>(n));
    }
}

}  // namespace farland::test
