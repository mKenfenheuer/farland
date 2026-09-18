// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "mutter_fixture.hpp"

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
using platform::portal::detail::BusError;
using platform::portal::detail::BusPtr;
using platform::portal::detail::MessagePtr;

constexpr const char* mock_service = "org.gnome.Mutter.RemoteDesktop";
constexpr const char* mock_path = "/org/farland/Mock";
constexpr const char* mock_interface = "org.farland.Mock";

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

/// A connection to the private bus; null if it cannot be made. No Catch2
/// assertions: helper threads get here.
BusPtr connect(const std::string& address)
{
    sd_bus* raw = nullptr;
    if (sd_bus_new(&raw) < 0) {
        return nullptr;
    }
    BusPtr bus(raw);
    if (sd_bus_set_address(raw, address.c_str()) < 0 || sd_bus_set_bus_client(raw, 1) < 0 || sd_bus_start(raw) < 0) {
        return nullptr;
    }
    return bus;
}

bool call_void(const std::string& address, const char* member)
{
    auto bus = connect(address);
    if (!bus) {
        return false;
    }
    BusError error;
    return sd_bus_call_method(bus.get(), mock_service, mock_path, mock_interface, member, error.get(), nullptr, "") >=
           0;
}

}  // namespace

std::string mock_mutter_script()
{
    return (std::filesystem::path(FARLAND_TEST_DATA_DIR).parent_path() / "platform" / "mutter" / "mock_mutter.py")
        .string();
}

std::unique_ptr<MockMutter> start_mock_mutter(const std::vector<std::string>& args, bool with_mutter)
{
    auto mock = std::make_unique<MockMutter>();
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
    if (!with_mutter) {
        return mock;
    }
    std::vector<std::string> argv{"python3", mock_mutter_script(), "--address", mock->address_};
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
            SKIP("mock_mutter.py needs python3-dbus and PyGObject");
        }
        FAIL("mock_mutter.py did not start");
    }
    return mock;
}

MockMutter::~MockMutter()
{
    stop(mock_pid_);
    stop(bus_pid_);
}

std::vector<std::string> MockMutter::calls() const
{
    auto bus = connect(address_);
    if (!bus) {
        return {};
    }
    BusError error;
    sd_bus_message* raw = nullptr;
    if (sd_bus_call_method(bus.get(), mock_service, mock_path, mock_interface, "Calls", error.get(), &raw, "") < 0) {
        return {};
    }
    const MessagePtr reply(raw);
    char** strv = nullptr;
    if (sd_bus_message_read_strv(reply.get(), &strv) < 0) {
        return {};
    }
    std::vector<std::string> result;
    for (char** s = strv; s != nullptr && *s != nullptr; ++s) {
        result.emplace_back(*s);
        std::free(*s);  // NOLINT(cppcoreguidelines-no-malloc)
    }
    std::free(strv);  // NOLINT(cppcoreguidelines-no-malloc)
    return result;
}

bool MockMutter::wait_for_call(const std::string& prefix, platform::mutter::MutterSession* session) const
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (session != nullptr) {
            session->process();
        }
        for (const auto& call : calls()) {
            if (call.starts_with(prefix)) {
                return true;
            }
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

bool MockMutter::close_sessions() const
{
    return call_void(address_, "CloseSessions");
}

bool MockMutter::vanish() const
{
    return call_void(address_, "Vanish");
}

bool MockMutter::copy(const std::vector<std::string>& mime_types, const std::vector<std::string>& contents) const
{
    auto bus = connect(address_);
    if (!bus) {
        return false;
    }
    sd_bus_message* raw = nullptr;
    if (sd_bus_message_new_method_call(bus.get(), &raw, mock_service, mock_path, mock_interface, "Copy") < 0) {
        return false;
    }
    const MessagePtr call(raw);
    for (const auto* list : {&mime_types, &contents}) {
        std::vector<char*> strv;
        for (const auto& text : *list) {
            strv.push_back(const_cast<char*>(text.c_str()));
        }
        strv.push_back(nullptr);
        if (sd_bus_message_append_strv(call.get(), strv.data()) < 0) {
            return false;
        }
    }
    BusError error;
    return sd_bus_call(bus.get(), call.get(), 0, error.get(), nullptr) >= 0;
}

std::optional<std::uint32_t> MockMutter::paste(const std::string& mime_type) const
{
    auto bus = connect(address_);
    if (!bus) {
        return std::nullopt;
    }
    BusError error;
    sd_bus_message* raw = nullptr;
    if (sd_bus_call_method(bus.get(), mock_service, mock_path, mock_interface, "Paste", error.get(), &raw, "s",
                           mime_type.c_str()) < 0) {
        return std::nullopt;
    }
    const MessagePtr reply(raw);
    std::uint32_t serial = 0;
    if (sd_bus_message_read_basic(reply.get(), 'u', &serial) <= 0) {
        return std::nullopt;
    }
    return serial;
}

std::optional<std::pair<bool, std::string>> MockMutter::written(std::uint32_t serial) const
{
    auto bus = connect(address_);
    if (!bus) {
        return std::nullopt;
    }
    BusError error;
    sd_bus_message* raw = nullptr;
    if (sd_bus_call_method(bus.get(), mock_service, mock_path, mock_interface, "Written", error.get(), &raw, "u",
                           serial) < 0) {
        return std::nullopt;
    }
    const MessagePtr reply(raw);
    int finished = 0;
    int success = 0;
    const char* data = nullptr;
    if (sd_bus_message_read(reply.get(), "bbs", &finished, &success, &data) < 0 || finished == 0) {
        return std::nullopt;
    }
    return std::pair(success != 0, std::string(data != nullptr ? data : ""));
}

platform::mutter::MutterOptions MockMutter::options() const
{
    platform::mutter::MutterOptions options;
    options.bus_address = address_;
    options.timeout = 10s;
    return options;
}

}  // namespace farland::test
