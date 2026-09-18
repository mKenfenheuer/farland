// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// [policy] takeover: farlandd's side of the question (who is asked, what
// the answer means, what happens when nobody answers) with a clock of the
// test's own, and the agent's side against a fake notification service on a
// private bus (mock_notifications.py).

#include "config.hpp"
#include "consent.hpp"
#include "prompt.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>

// The fake notification service runs on a private bus; the asker needs
// sd-bus to reach it, so those tests build only where it is there.
#if defined(FARLAND_HAVE_LIBSYSTEMD)
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;  // NOLINT(readability-redundant-declaration)
#endif

namespace broker = farland::server::broker;
using farland::agent::ConsentQuestion;
using farland::daemon::ConsentBroker;
using farland::daemon::PolicySection;
using farland::daemon::TakeoverDefault;
using farland::daemon::TakeoverPolicy;
using Admission = ConsentBroker::Admission;
using namespace std::chrono_literals;

namespace {

/// A clock the test moves by hand.
struct FakeClock {
    ConsentBroker::Clock::time_point now{std::chrono::hours(500)};
    void advance(ConsentBroker::Clock::duration d) { now += d; }
};

ConsentBroker::Party alice()
{
    return ConsentBroker::Party{"LAB\\alice", "192.0.2.10:50123", "WORKSTATION"};
}

}  // namespace

TEST_CASE("takeover decides who may have a session someone is holding")
{
    PolicySection asking;
    CHECK(ConsentBroker(asking).admit(true) == Admission::ask);
    // A session nobody holds is simply resumed; that is the ordinary
    // reconnect, and nobody is asked.
    CHECK(ConsentBroker(asking).admit(false) == Admission::hand_over);

    PolicySection always;
    always.takeover = TakeoverPolicy::always;
    CHECK(ConsentBroker(always).admit(true) == Admission::hand_over);
    CHECK(ConsentBroker(always).admit(false) == Admission::hand_over);

    PolicySection never;
    never.takeover = TakeoverPolicy::never;
    CHECK(ConsentBroker(never).admit(true) == Admission::refuse);
    CHECK(ConsentBroker(never).admit(false) == Admission::hand_over);
    CHECK(ConsentBroker(never).refusal("alice").find("takeover = \"never\"") != std::string::npos);
}

TEST_CASE("The question names the connection and carries the policy")
{
    FakeClock clock;
    PolicySection policy;
    policy.takeover_timeout = 45s;
    policy.takeover_on_timeout = TakeoverDefault::deny;
    ConsentBroker consent(policy);
    const auto request = consent.ask(3, 7, alice(), clock.now);
    CHECK(request.connection_id == 7);
    CHECK(request.user == "LAB\\alice");
    CHECK(request.peer == "192.0.2.10:50123");
    CHECK(request.client_name == "WORKSTATION");
    CHECK(request.timeout_seconds == 45);
    CHECK_FALSE(request.allow_on_timeout);
    CHECK(consent.waiting(7));
    CHECK_FALSE(consent.waiting(8));
    consent.forget(7);
    CHECK_FALSE(consent.waiting(7));
}

TEST_CASE("The answer decides: yes hands the session over, cancel refuses the connection")
{
    FakeClock clock;
    ConsentBroker consent{PolicySection{}};
    static_cast<void>(consent.ask(3, 7, alice(), clock.now));
    const auto allowed = consent.answered(7, broker::ConsentAnswer::allowed);
    REQUIRE(allowed.has_value());
    CHECK(allowed->session == 3);
    CHECK(allowed->connection == 7);
    CHECK(allowed->allowed);
    CHECK_FALSE(allowed->withdraw);  // the agent's prompt is already gone
    CHECK_FALSE(consent.waiting(7));
    // Only once, and only for a question that stands.
    CHECK_FALSE(consent.answered(7, broker::ConsentAnswer::allowed).has_value());
    CHECK_FALSE(consent.answered(99, broker::ConsentAnswer::denied).has_value());

    static_cast<void>(consent.ask(3, 8, alice(), clock.now));
    const auto denied = consent.answered(8, broker::ConsentAnswer::denied);
    REQUIRE(denied.has_value());
    CHECK_FALSE(denied->allowed);
    CHECK(denied->reason.find("kept") != std::string::npos);
}

TEST_CASE("takeover_on_timeout applies when nobody answers, and to a session with nobody to ask")
{
    FakeClock clock;
    PolicySection policy;
    policy.takeover_timeout = 30s;
    ConsentBroker allowing(policy);
    static_cast<void>(allowing.ask(3, 7, alice(), clock.now));
    clock.advance(29s);
    CHECK(allowing.due(clock.now).empty());
    // farlandd leaves the agent's own countdown a moment to come in first.
    clock.advance(1s);
    CHECK(allowing.due(clock.now).empty());
    clock.advance(ConsentBroker::answer_grace);
    const auto late = allowing.due(clock.now);
    REQUIRE(late.size() == 1);
    CHECK(late[0].connection == 7);
    CHECK(late[0].allowed);
    CHECK(late[0].withdraw);  // the prompt is still on screen
    CHECK(late[0].reason.find("takeover_on_timeout = \"allow\"") != std::string::npos);
    CHECK(allowing.due(clock.now).empty());  // reported once

    policy.takeover_on_timeout = TakeoverDefault::deny;
    ConsentBroker denying(policy);
    static_cast<void>(denying.ask(3, 9, alice(), clock.now));
    const auto timed_out = denying.answered(9, broker::ConsentAnswer::timed_out);
    REQUIRE(timed_out.has_value());
    CHECK_FALSE(timed_out->allowed);
    CHECK_FALSE(timed_out->withdraw);

    // A desktop with no notification service is not a reason to wait.
    static_cast<void>(denying.ask(3, 10, alice(), clock.now));
    const auto unavailable = denying.answered(10, broker::ConsentAnswer::unavailable);
    REQUIRE(unavailable.has_value());
    CHECK_FALSE(unavailable->allowed);
    CHECK(unavailable->reason.find("notification service") != std::string::npos);
}

TEST_CASE("A holder who leaves, or a session that ends, resolves the question")
{
    FakeClock clock;
    ConsentBroker consent{PolicySection{}};
    static_cast<void>(consent.ask(3, 7, alice(), clock.now));
    static_cast<void>(consent.ask(4, 8, alice(), clock.now));
    const auto released = consent.released(3);
    REQUIRE(released.size() == 1);
    CHECK(released[0].connection == 7);
    CHECK(released[0].allowed);  // nobody holds it any more: the takeover goes ahead
    CHECK(released[0].withdraw);
    CHECK(consent.waiting(8));  // the other session is untouched

    const auto ended = consent.ended(4);
    REQUIRE(ended.size() == 1);
    CHECK(ended[0].connection == 8);
    CHECK_FALSE(ended[0].allowed);
    CHECK_FALSE(ended[0].withdraw);  // there is nobody left to tell
    CHECK(consent.released(3).empty());
}

TEST_CASE("The prompt says who wants the session, and the countdown runs in the default button")
{
    ConsentQuestion question;
    question.user = "LAB\\alice";
    question.peer = "192.0.2.10:50123";
    question.client_name = "WORKSTATION";
    CHECK(farland::agent::consent_summary() == "Hand over your session?");
    const auto body = farland::agent::consent_body(question, "14:32");
    CHECK(body.starts_with("Someone is trying to connect to your session via RDP."));
    CHECK(body.find("\nLAB\\alice from 192.0.2.10:50123 (WORKSTATION), 14:32") != std::string::npos);

    const auto [yes, cancel] = farland::agent::consent_buttons(question, 30s);
    CHECK(yes == "Yes (30s)");
    CHECK(cancel == "Cancel");
    question.allow_on_timeout = false;
    const auto [yes_plain, cancel_counting] = farland::agent::consent_buttons(question, 7s);
    CHECK(yes_plain == "Yes");
    CHECK(cancel_counting == "Cancel (7s)");

    // A client that gave no name only makes the line shorter, and what it
    // did give goes in as text, not as markup.
    ConsentQuestion nameless;
    nameless.user = "bob & co";
    nameless.peer = "[2001:db8::1]:3389";
    const auto plain = farland::agent::consent_body(nameless, "09:05");
    CHECK(plain.find("bob &amp; co from [2001:db8::1]:3389, 09:05") != std::string::npos);
    CHECK(plain.find("(") == std::string::npos);
    CHECK_FALSE(farland::agent::local_time_now().empty());
}

#if defined(FARLAND_HAVE_LIBSYSTEMD)

namespace {

/// A private D-Bus daemon, with mock_notifications.py on it when asked.
class MockNotifications {
public:
    MockNotifications() = default;
    MockNotifications(const MockNotifications&) = delete;
    MockNotifications& operator=(const MockNotifications&) = delete;
    MockNotifications(MockNotifications&&) = delete;
    MockNotifications& operator=(MockNotifications&&) = delete;
    ~MockNotifications()
    {
        stop(mock_);
        stop(bus_);
    }

    [[nodiscard]] const std::string& address() const noexcept { return address_; }

    void set_bus(pid_t pid, std::string address)
    {
        bus_ = pid;
        address_ = std::move(address);
    }
    void set_mock(pid_t pid) { mock_ = pid; }

private:
    static void stop(pid_t& pid)
    {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
    }

    pid_t bus_ = -1;
    pid_t mock_ = -1;
    std::string address_;
};

std::pair<pid_t, int> spawn_with_stdout(const std::vector<std::string>& argv, int& error)
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
    for (const auto& argument : argv) {
        args.push_back(const_cast<char*>(argument.c_str()));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    }
    args.push_back(nullptr);
    pid_t pid = -1;
    error = ::posix_spawnp(&pid, args.front(), &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe[1]);
    if (error != 0) {
        ::close(pipe[0]);
        return {-1, -1};
    }
    return {pid, pipe[0]};
}

/// One line, waiting up to `timeout`; empty on end of file or timeout.
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

std::string mock_script()
{
    return (std::filesystem::path(FARLAND_TEST_DATA_DIR).parent_path() / "apps" / "mock_notifications.py").string();
}

/// Starts the bus and, with `answer` other than "no service", the mock.
/// SKIPs the test where dbus-daemon, python3-dbus or PyGObject is missing.
std::unique_ptr<MockNotifications> start_mock(const std::vector<std::string>& args, bool with_service = true)
{
    auto mock = std::make_unique<MockNotifications>();
    int error = 0;
    auto [bus_pid, bus_out] = spawn_with_stdout({"dbus-daemon", "--session", "--nofork", "--print-address=1"}, error);
    if (bus_pid < 0) {
        SKIP("cannot run dbus-daemon: " << std::strerror(error));
    }
    const std::string address = read_line(bus_out, 10s);
    ::close(bus_out);
    mock->set_bus(bus_pid, address);
    if (address.empty()) {
        SKIP("dbus-daemon --session did not start");
    }
    if (!with_service) {
        return mock;
    }
    std::vector<std::string> argv{"python3", mock_script(), "--address", address};
    argv.insert(argv.end(), args.begin(), args.end());
    auto [mock_pid, mock_out] = spawn_with_stdout(argv, error);
    if (mock_pid < 0) {
        SKIP("cannot run python3: " << std::strerror(error));
    }
    mock->set_mock(mock_pid);
    const std::string ready = read_line(mock_out, 20s);
    ::close(mock_out);
    if (ready != "ready") {
        SKIP("mock_notifications.py needs python3-dbus and PyGObject");
    }
    return mock;
}

/// The question the tests ask, on the mock's bus.
ConsentQuestion question_on(const MockNotifications& mock, std::chrono::seconds timeout)
{
    ConsentQuestion question;
    question.user = "LAB\\alice";
    question.peer = "192.0.2.10:50123";
    question.client_name = "WORKSTATION";
    question.timeout = timeout;
    question.bus_address = mock.address();
    return question;
}

}  // namespace

TEST_CASE("The user presses Yes or Cancel in the notification")
{
    const std::atomic<bool> cancel{false};
    {
        const auto mock = start_mock({"--answer", "farland-allow"});
        CHECK(farland::agent::ask_over_notifications(question_on(*mock, 20s), cancel) ==
              broker::ConsentAnswer::allowed);
    }
    {
        const auto mock = start_mock({"--answer", "farland-deny"});
        CHECK(farland::agent::ask_over_notifications(question_on(*mock, 20s), cancel) == broker::ConsentAnswer::denied);
    }
    {
        // Dismissing the notification answers nothing; the default applies.
        const auto mock = start_mock({"--answer", "dismiss"});
        CHECK(farland::agent::ask_over_notifications(question_on(*mock, 20s), cancel) ==
              broker::ConsentAnswer::timed_out);
    }
}

TEST_CASE("A prompt nobody answers runs out, and one nobody serves does not wait")
{
    const std::atomic<bool> cancel{false};
    {
        const auto mock = start_mock({"--answer", "none"});
        const auto started = std::chrono::steady_clock::now();
        CHECK(farland::agent::ask_over_notifications(question_on(*mock, 2s), cancel) ==
              broker::ConsentAnswer::timed_out);
        CHECK(std::chrono::steady_clock::now() - started >= 2s);
    }
    {
        // A bus with no notification service on it: farlandd's default
        // applies rather than the client waiting.
        const auto mock = start_mock({}, false);
        const auto started = std::chrono::steady_clock::now();
        CHECK(farland::agent::ask_over_notifications(question_on(*mock, 60s), cancel) ==
              broker::ConsentAnswer::unavailable);
        CHECK(std::chrono::steady_clock::now() - started < 30s);
    }
}

TEST_CASE("The prompt is withdrawn when the person holding the session leaves")
{
    const auto mock = start_mock({"--answer", "none"});
    std::atomic<bool> cancel{false};
    auto answer = broker::ConsentAnswer::allowed;
    std::thread asking([&] { answer = farland::agent::ask_over_notifications(question_on(*mock, 60s), cancel); });
    std::this_thread::sleep_for(500ms);
    cancel = true;
    asking.join();
    // Whatever it says, the agent throws the answer away; what matters is
    // that it came back at once and took the notification down.
    CHECK(answer == broker::ConsentAnswer::timed_out);
}

#endif
