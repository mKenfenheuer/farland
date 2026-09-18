// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// farland-agent against a scripted farlandd, in process over socket pairs:
// the broker messages, the handover of plaintext RDP streams, a desktop that
// outlives its connections, takeover, auto-reconnect cookies and Terminate.

#include <farland/auth/auto_reconnect.hpp>
#include <farland/proto/share.hpp>
#include <farland/proto/x224.hpp>
#include <farland/server/broker.hpp>

#include "agent.hpp"
#include "support/rdp_test_client.hpp"
#include "test_desktop.hpp"
#include "unix_socket.hpp"

#include <catch2/catch_test_macros.hpp>

#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace broker = farland::server::broker;
using farland::UniqueFd;
using farland::test::RdpTestClient;

namespace {

std::pair<UniqueFd, UniqueFd> socket_pair()
{
    std::array<int, 2> fds{-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
    return {UniqueFd(fds[0]), UniqueFd(fds[1])};
}

/// The next message from the agent that is not Stats.
broker::Message next_message(int fd, broker::AgentLink& link)
{
    for (;;) {
        auto received = farland::app::receive_message(fd, broker::max_message_size, 10'000);
        REQUIRE(received.has_value());
        REQUIRE(received->has_value());
        auto message = link.receive((*received)->frame, (*received)->fd.valid());
        REQUIRE(message.has_value());
        if (!std::holds_alternative<broker::Stats>(*message)) {
            return std::move(*message);
        }
    }
}

broker::NewConnection new_connection(std::uint64_t id)
{
    broker::NewConnection message;
    message.connection_id = id;
    message.negotiation.requested_protocols = farland::proto::protocol::hybrid_ex;
    message.negotiation.selected_protocol = farland::proto::protocol::hybrid_ex;
    message.negotiation.identity = farland::auth::Identity{"alice", ""};
    message.peer = "client-" + std::to_string(id);
    return message;
}

/// The colour of the test pattern's cell for a key (test_pattern.cpp), keyed by evdev code.
std::uint32_t key_cell_color(std::uint32_t evdev)
{
    return ((evdev * 0x9E3779B1U) >> 8U) & 0xFFFFFFU;
}

constexpr std::uint32_t key_a = 30;  // KEY_A, from scancode 0x1E

bool shows_key(const RdpTestClient& c, std::uint32_t evdev, std::size_t cell = 0)
{
    return c.width() > 0 &&
           c.rgb(15 + static_cast<std::uint32_t>(cell * 20), c.height() - 18U) == key_cell_color(evdev);
}

}  // namespace

TEST_CASE("Agent: connections share one desktop; takeover, cookies and Terminate")
{
    auto [daemon_end, agent_end] = socket_pair();
    broker::Token token{};
    token.fill(std::byte{0x5A});
    std::atomic<int> desktops{0};
    // Written on the agent's thread; atomic because the order the test
    // relies on travels through sockets, which ThreadSanitizer cannot see.
    std::atomic<std::uint32_t> desktop_width{0};
    std::atomic<std::uint32_t> desktop_height{0};
    std::atomic<unsigned> desktop_fps{0};
    std::atomic<farland::app::TestDesktop*> made{nullptr};

    farland::agent::AgentConfig config;
    config.daemon = std::move(agent_end);
    config.token = token;
    config.logon_id = 7;
    config.session.frames_per_second = 60;
    config.session.audio = false;
    config.session.microphone = false;
    config.stats_period = std::chrono::milliseconds(100);
    config.make_desktop =
        [&](const farland::agent::DesktopRequest& request) -> farland::Result<std::unique_ptr<farland::app::Desktop>> {
        ++desktops;
        desktop_width = request.width;
        desktop_height = request.height;
        desktop_fps = request.frames_per_second;
        auto desktop =
            std::make_unique<farland::app::TestDesktop>(request.width, request.height, request.frames_per_second);
        made = desktop.get();
        return desktop;
    };
    farland::agent::Agent agent(std::move(config));
    std::atomic<bool> stop{false};
    broker::EndReason reason = broker::EndReason::error;
    std::thread thread([&] { reason = agent.run(stop); });

    broker::AgentLink link(token);
    CHECK(std::holds_alternative<broker::Hello>(next_message(daemon_end.get(), link)));

    // farlandd's configuration, before the first connection: it replaces the
    // agent's own options and reaches the desktop it starts.
    broker::Settings settings;
    settings.frames_per_second = 24;
    settings.audio = false;
    settings.microphone = false;
    settings.clipboard = false;
    REQUIRE(farland::app::send_message(daemon_end.get(), broker::encode(settings)));

    // Connection 1 starts the desktop at its size and gets a cookie.
    auto [server1, client1_fd] = socket_pair();
    REQUIRE(farland::app::send_message(daemon_end.get(), broker::encode(new_connection(1)), server1.get()));
    server1.reset();
    RdpTestClient client1(client1_fd.release());
    CHECK(client1.activate(320, 240) == std::pair<std::uint16_t, std::uint16_t>{320, 240});
    CHECK(desktop_width.load() == 320);
    CHECK(desktop_height.load() == 240);
    CHECK(desktop_fps.load() == 24);
    REQUIRE(client1.pump_until([&] { return client1.arc_cookie().has_value() && client1.bitmap_updates() > 0; }));
    CHECK(client1.arc_cookie()->logon_id == 7);
    client1.send_keys({0x1E});
    REQUIRE(client1.pump_until([&] { return shows_key(client1, key_a); }));

    // [policy] seat_takeover: a login at the machine that was refused reaches
    // the desktop, which keeps the seat's login screen instead of ending.
    REQUIRE(farland::app::send_message(daemon_end.get(), broker::encode(broker::SeatTakeover{false})));
    REQUIRE(client1.pump_until([&] {
        auto* desktop = made.load();
        return desktop != nullptr && desktop->seat_takeover_allowed() == std::optional(false);
    }));

    // Connection 2 takes over: the first ends with ERRINFO_DISCONNECTED_BY_OTHERCONNECTION.
    auto [server2, client2_fd] = socket_pair();
    REQUIRE(farland::app::send_message(daemon_end.get(), broker::encode(new_connection(2)), server2.get()));
    server2.reset();
    CHECK_FALSE(client1.pump_until([] { return false; }));
    CHECK(client1.ended());
    CHECK(client1.error_info() == 0x00000005U);
    const auto ended = next_message(daemon_end.get(), link);
    REQUIRE(std::holds_alternative<broker::Disconnect>(ended));
    CHECK(std::get<broker::Disconnect>(ended).connection_id == 1);
    CHECK(std::get<broker::Disconnect>(ended).error_info == 0x00000005U);

    // The returning client proves its cookie and finds the key it typed.
    const auto random = client1.arc_cookie()->random_bits;
    farland::proto::AutoReconnectCookie cookie;
    cookie.logon_id = 7;
    cookie.security_verifier = farland::auth::arc::security_verifier(random);
    RdpTestClient client2(client2_fd.release());
    client2.activate(320, 240, cookie);
    REQUIRE(client2.pump_until([&] { return shows_key(client2, key_a) && client2.arc_cookie().has_value(); }));
    CHECK(client2.arc_cookie()->random_bits != random);  // rotated
    CHECK(agent.cookies_matched() == 1);
    CHECK(agent.cookies_mismatched() == 0);
    CHECK(desktops.load() == 1);

    // Stats name the current connection.
    for (;;) {
        auto received = farland::app::receive_message(daemon_end.get(), broker::max_message_size, 10'000);
        if (!received) {
            FAIL(received.error().message());
        }
        REQUIRE(received->has_value());
        auto message = link.receive((*received)->frame, false);
        REQUIRE(message.has_value());
        // The first Stats of connection 2 may come before it is active.
        if (const auto* stats = std::get_if<broker::Stats>(&*message);
            stats != nullptr && stats->connection_id == 2 && stats->desktop_width != 0) {
            CHECK(stats->desktop_width == 320);
            CHECK(stats->bytes_sent > 0);
            break;
        }
    }

    // The daemon ends the session.
    REQUIRE(farland::app::send_message(daemon_end.get(),
                                       broker::encode(broker::Terminate{broker::EndReason::disconnected_timeout})));
    CHECK_FALSE(client2.pump_until([] { return false; }));
    CHECK(client2.error_info() == 0x00000002U);  // ERRINFO_RPC_INITIATED_LOGOFF
    const auto disconnect = next_message(daemon_end.get(), link);
    REQUIRE(std::holds_alternative<broker::Disconnect>(disconnect));
    const auto last = next_message(daemon_end.get(), link);
    REQUIRE(std::holds_alternative<broker::SessionEnded>(last));
    CHECK(std::get<broker::SessionEnded>(last).reason == broker::EndReason::disconnected_timeout);
    thread.join();
    CHECK(reason == broker::EndReason::disconnected_timeout);
}

TEST_CASE("Agent: the takeover question reaches the user and the answer reaches farlandd")
{
    auto [daemon_end, agent_end] = socket_pair();
    broker::Token token{};
    farland::agent::AgentConfig config;
    config.daemon = std::move(agent_end);
    config.token = token;
    config.logon_id = 4;
    config.stats_period = std::chrono::hours(1);  // Stats would only be noise here
    config.make_desktop =
        [](const farland::agent::DesktopRequest& request) -> farland::Result<std::unique_ptr<farland::app::Desktop>> {
        return std::make_unique<farland::app::TestDesktop>(request.width, request.height, request.frames_per_second);
    };
    // A user who takes their time: the prompt stands until the test answers
    // it or farlandd takes it back.
    std::atomic<int> asked{0};
    std::atomic<bool> details_ok{false};
    std::atomic<bool> withdrawn{false};
    std::atomic<bool> answer_ready{false};
    std::atomic<broker::ConsentAnswer> answer{broker::ConsentAnswer::allowed};
    config.ask_consent = [&](const farland::agent::ConsentQuestion& question,
                             const std::atomic<bool>& cancel) -> broker::ConsentAnswer {
        details_ok = question.user == "LAB\\alice" && question.peer == "192.0.2.10:50123" &&
                     question.client_name == "WORKSTATION" && question.timeout == std::chrono::seconds(30) &&
                     question.allow_on_timeout;
        ++asked;
        while (!cancel.load() && !answer_ready.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (cancel.load()) {
            withdrawn = true;
        }
        return answer.load();
    };
    farland::agent::Agent agent(std::move(config));
    std::atomic<bool> stop{false};
    broker::EndReason reason = broker::EndReason::error;
    std::thread thread([&] { reason = agent.run(stop); });

    broker::AgentLink link(token);
    CHECK(std::holds_alternative<broker::Hello>(next_message(daemon_end.get(), link)));

    const broker::ConsentRequest request{5, "LAB\\alice", "192.0.2.10:50123", "WORKSTATION", 30, true};
    REQUIRE(farland::app::send_message(daemon_end.get(), broker::encode(request)));
    while (asked.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(details_ok.load());
    answer_ready = true;
    const auto allowed = next_message(daemon_end.get(), link);
    REQUIRE(std::holds_alternative<broker::ConsentReply>(allowed));
    CHECK(std::get<broker::ConsentReply>(allowed).connection_id == 5);
    CHECK(std::get<broker::ConsentReply>(allowed).answer == broker::ConsentAnswer::allowed);

    // A question farlandd takes back is answered to nobody.
    answer_ready = false;
    broker::ConsentRequest second = request;
    second.connection_id = 6;
    REQUIRE(farland::app::send_message(daemon_end.get(), broker::encode(second)));
    while (asked.load() < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(farland::app::send_message(daemon_end.get(), broker::encode(broker::ConsentCancel{6})));
    while (!withdrawn.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // The next question is answered, and its answer is the next message: no
    // reply for connection 6 came before it.
    answer = broker::ConsentAnswer::denied;
    answer_ready = true;
    broker::ConsentRequest third = request;
    third.connection_id = 7;
    REQUIRE(farland::app::send_message(daemon_end.get(), broker::encode(third)));
    const auto denied = next_message(daemon_end.get(), link);
    REQUIRE(std::holds_alternative<broker::ConsentReply>(denied));
    CHECK(std::get<broker::ConsentReply>(denied).connection_id == 7);
    CHECK(std::get<broker::ConsentReply>(denied).answer == broker::ConsentAnswer::denied);

    REQUIRE(farland::app::send_message(daemon_end.get(), broker::encode(broker::Terminate{})));
    const auto last = next_message(daemon_end.get(), link);
    CHECK(std::holds_alternative<broker::SessionEnded>(last));
    thread.join();
    CHECK(reason == broker::EndReason::terminated);
}

TEST_CASE("Agent: the session ends with the desktop, and when farlandd goes away")
{
    auto [daemon_end, agent_end] = socket_pair();
    broker::Token token{};
    std::atomic<farland::app::TestDesktop*> desktop{nullptr};
    farland::agent::AgentConfig config;
    config.daemon = std::move(agent_end);
    config.token = token;
    config.session.audio = false;
    config.session.microphone = false;
    config.make_desktop =
        [&](const farland::agent::DesktopRequest& request) -> farland::Result<std::unique_ptr<farland::app::Desktop>> {
        auto made = std::make_unique<farland::app::TestDesktop>(request.width, request.height);
        desktop = made.get();
        return made;
    };
    farland::agent::Agent agent(std::move(config));
    std::atomic<bool> stop{false};
    broker::EndReason reason = broker::EndReason::error;
    std::thread thread([&] { reason = agent.run(stop); });
    broker::AgentLink link(token);
    CHECK(std::holds_alternative<broker::Hello>(next_message(daemon_end.get(), link)));

    auto [server, client_fd] = socket_pair();
    REQUIRE(farland::app::send_message(daemon_end.get(), broker::encode(new_connection(3)), server.get()));
    server.reset();
    RdpTestClient client(client_fd.release());
    client.activate(640, 480);
    REQUIRE(client.pump_until([&] { return client.bitmap_updates() > 0; }));
    client.drop();  // the client goes; the session stays
    const auto gone = next_message(daemon_end.get(), link);
    REQUIRE(std::holds_alternative<broker::Disconnect>(gone));
    CHECK(std::get<broker::Disconnect>(gone).error_info == 0);

    REQUIRE(desktop.load() != nullptr);
    desktop.load()->close();  // the user logged out
    const auto ended = next_message(daemon_end.get(), link);
    REQUIRE(std::holds_alternative<broker::SessionEnded>(ended));
    CHECK(std::get<broker::SessionEnded>(ended).reason == broker::EndReason::logout);
    thread.join();
    CHECK(reason == broker::EndReason::logout);

    // A second agent whose farlandd closes the socket ends at once.
    auto [daemon2, agent2_end] = socket_pair();
    farland::agent::AgentConfig config2;
    config2.daemon = std::move(agent2_end);
    config2.make_desktop =
        [](const farland::agent::DesktopRequest&) -> farland::Result<std::unique_ptr<farland::app::Desktop>> {
        return farland::fail(farland::Errc::unsupported, "unused");
    };
    farland::agent::Agent agent2(std::move(config2));
    daemon2.reset();
    CHECK(agent2.run(stop) == broker::EndReason::error);
}
