// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Multi-session end to end: the real farlandd in development mode (--no-pam),
// its sandboxed network processes and farland-agents with test pattern
// desktops, against scripted clients over TCP with TLS and NLA. Two users get
// two sessions; a client that drops and returns finds its session as it
// left it; a second connection of the same user takes the session over.

#include <farland/auth/auto_reconnect.hpp>
#include <farland/auth/credential_store.hpp>
#include <farland/auth/ntlm.hpp>
#include <farland/proto/client_info.hpp>

#include "support/rdp_test_client.hpp"

#include <catch2/catch_test_macros.hpp>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;  // NOLINT(readability-redundant-declaration)

using farland::test::RdpTestClient;

namespace {

std::uint16_t free_port()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    ::close(fd);
    return ntohs(address.sin_port);
}

std::uint32_t key_cell_color(std::uint32_t evdev)
{
    return ((evdev * 0x9E3779B1U) >> 8U) & 0xFFFFFFU;
}

bool shows_key(const RdpTestClient& c, std::uint32_t evdev, std::size_t cell = 0)
{
    return c.width() > 0 &&
           c.rgb(15 + static_cast<std::uint32_t>(cell * 20), c.height() - 18U) == key_cell_color(evdev);
}

constexpr std::uint32_t key_a = 30;  // scancode 0x1E
constexpr std::uint32_t key_b = 48;  // scancode 0x30

/// farlandd with a temporary configuration, stopped at the end.
class Farlandd {
public:
    Farlandd(const char* daemon, const char* agent, const std::string& policy = {})
        : dir_(std::filesystem::temp_directory_path() / ("farlandd-e2e-" + std::to_string(::getpid()))),
          port_(free_port())
    {
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        farland::auth::CredentialStore store;
        store.set("alice", "", farland::auth::ntlm::nt_hash("Alice-pass1"));
        store.set("bob", "", farland::auth::ntlm::nt_hash("Bob-pass1"));
        REQUIRE(store.save(dir_ / "users").has_value());
        {
            std::ofstream config(dir_ / "farland.toml");
            config << "[server]\nbind = \"127.0.0.1\"\nport = " << port_ << "\n"
                   << "[auth]\ncredential_store = \"" << (dir_ / "users").string() << "\"\n"
                   << "[session]\ndesktop = \"test\"\n"
                   << (policy.empty() ? "" : "[policy]\n" + policy + "\n");
        }
        const std::vector<std::string> args{daemon,
                                            "--config",
                                            (dir_ / "farland.toml").string(),
                                            "--no-pam",
                                            "--runtime-dir",
                                            (dir_ / "run").string(),
                                            "--state-dir",
                                            (dir_ / "state").string(),
                                            "--agent",
                                            agent,
                                            "--hostname",
                                            "e2e.farland.test",
                                            "--log-level",
                                            "warn"};
        std::vector<char*> argv;
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
        }
        argv.push_back(nullptr);
        REQUIRE(::posix_spawn(&pid_, daemon, nullptr, nullptr, argv.data(), environ) == 0);
    }
    Farlandd(const Farlandd&) = delete;
    Farlandd& operator=(const Farlandd&) = delete;
    Farlandd(Farlandd&&) = delete;
    Farlandd& operator=(Farlandd&&) = delete;
    ~Farlandd()
    {
        static_cast<void>(stop());
        std::filesystem::remove_all(dir_);
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    /// SIGTERM and wait; the exit status (-1 if killed).
    int stop()
    {
        if (pid_ <= 0) {
            return 0;
        }
        ::kill(pid_, SIGTERM);
        int status = 0;
        for (int i = 0; i < 300; ++i) {
            if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ::kill(pid_, SIGKILL);
        ::waitpid(pid_, &status, 0);
        pid_ = -1;
        return -1;
    }

private:
    std::filesystem::path dir_;
    std::uint16_t port_;
    pid_t pid_ = -1;
};

std::unique_ptr<RdpTestClient> connect(const Farlandd& daemon, const std::string& user, const std::string& password)
{
    const int fd = RdpTestClient::connect_tcp(daemon.port());
    REQUIRE(fd >= 0);
    auto client = std::make_unique<RdpTestClient>(fd);
    REQUIRE(client->login(user, password));
    return client;
}

}  // namespace

TEST_CASE("farlandd: two users, reconnect into the old session, takeover")
{
    const char* daemon_path = std::getenv("FARLAND_DAEMON");
    const char* agent_path = std::getenv("FARLAND_AGENT");
    if (daemon_path == nullptr || agent_path == nullptr) {
        SKIP("FARLAND_DAEMON and FARLAND_AGENT are not set");
    }
    // takeover = "always" is the takeover this case is about; the prompt of
    // the default "ask" has its own case below.
    Farlandd daemon(daemon_path, agent_path, "takeover = \"always\"");

    // alice's session starts at her size; she types A.
    auto alice = connect(daemon, "alice", "Alice-pass1");
    CHECK(alice->activate(320, 240) == std::pair<std::uint16_t, std::uint16_t>{320, 240});
    REQUIRE(alice->pump_until([&] { return alice->arc_cookie().has_value() && alice->bitmap_updates() > 0; }));
    const auto alice_logon = alice->arc_cookie()->logon_id;
    alice->send_keys({0x1E});
    REQUIRE(alice->pump_until([&] { return shows_key(*alice, key_a); }));

    // bob gets a session of his own: another logon id, no keys, his size.
    auto bob = connect(daemon, "bob", "Bob-pass1");
    CHECK(bob->activate(400, 300) == std::pair<std::uint16_t, std::uint16_t>{400, 300});
    REQUIRE(bob->pump_until([&] { return bob->arc_cookie().has_value() && bob->bitmap_updates() > 3; }));
    CHECK(bob->arc_cookie()->logon_id != alice_logon);
    CHECK_FALSE(shows_key(*bob, key_a));
    bob->send_keys({0x30});
    REQUIRE(bob->pump_until([&] { return shows_key(*bob, key_b); }));

    // A wrong password gets nowhere.
    {
        const int fd = RdpTestClient::connect_tcp(daemon.port());
        REQUIRE(fd >= 0);
        RdpTestClient mallory(fd);
        CHECK_FALSE(mallory.login("alice", "wrong"));
    }

    // alice's network drops; she comes back with her cookie to her session.
    const auto random = alice->arc_cookie()->random_bits;
    alice->drop();
    alice.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    farland::proto::AutoReconnectCookie cookie;
    cookie.logon_id = alice_logon;
    cookie.security_verifier = farland::auth::arc::security_verifier(random);
    auto alice2 = connect(daemon, "alice", "Alice-pass1");
    alice2->activate(320, 240, cookie);
    REQUIRE(alice2->pump_until([&] { return shows_key(*alice2, key_a) && alice2->arc_cookie().has_value(); }));
    CHECK(alice2->arc_cookie()->logon_id == alice_logon);
    CHECK(alice2->arc_cookie()->random_bits != random);
    CHECK_FALSE(shows_key(*alice2, key_b));

    // alice connects from elsewhere while connected: takeover.
    auto alice3 = connect(daemon, "alice", "Alice-pass1");
    alice3->activate(320, 240);
    CHECK_FALSE(alice2->pump_until([] { return false; }));
    CHECK(alice2->error_info() == 0x00000005U);  // ERRINFO_DISCONNECTED_BY_OTHERCONNECTION
    REQUIRE(alice3->pump_until([&] { return shows_key(*alice3, key_a); }));
    CHECK(alice3->arc_cookie()->logon_id == alice_logon);

    // bob was not disturbed.
    REQUIRE(bob->pump_until([&] { return bob->bitmap_updates() > 20; }));
    CHECK_FALSE(bob->ended());

    // Stopping farlandd ends every session.
    CHECK(daemon.stop() == 0);
    CHECK_FALSE(bob->pump_until([] { return false; }));
    CHECK_FALSE(alice3->pump_until([] { return false; }));
}

TEST_CASE("farlandd: takeover = \"never\" keeps a session for whoever holds it")
{
    const char* daemon_path = std::getenv("FARLAND_DAEMON");
    const char* agent_path = std::getenv("FARLAND_AGENT");
    if (daemon_path == nullptr || agent_path == nullptr) {
        SKIP("FARLAND_DAEMON and FARLAND_AGENT are not set");
    }
    Farlandd daemon(daemon_path, agent_path, "takeover = \"never\"");
    auto alice = connect(daemon, "alice", "Alice-pass1");
    alice->activate(320, 240);
    REQUIRE(alice->pump_until([&] { return alice->bitmap_updates() > 0; }));

    // A second connection of the same user is turned away while she has it.
    auto intruder = connect(daemon, "alice", "Alice-pass1");
    intruder->activate(320, 240);
    CHECK_FALSE(intruder->pump_until([] { return false; }));
    CHECK(intruder->error_info() == 0x00000007U);  // ERRINFO_SERVER_DENIED_CONNECTION
    CHECK_FALSE(alice->ended());

    // Once she has gone, her session is simply resumed.
    alice->drop();
    alice.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    auto returning = connect(daemon, "alice", "Alice-pass1");
    returning->activate(320, 240);
    REQUIRE(returning->pump_until([&] { return returning->bitmap_updates() > 0; }));
    CHECK_FALSE(returning->ended());
}

TEST_CASE("farlandd: a refused client learns why")
{
    const char* daemon_path = std::getenv("FARLAND_DAEMON");
    const char* agent_path = std::getenv("FARLAND_AGENT");
    if (daemon_path == nullptr || agent_path == nullptr) {
        SKIP("FARLAND_DAEMON and FARLAND_AGENT are not set");
    }
    Farlandd daemon(daemon_path, agent_path, "max_sessions = 1");
    auto alice = connect(daemon, "alice", "Alice-pass1");
    alice->activate(320, 240);
    REQUIRE(alice->pump_until([&] { return alice->bitmap_updates() > 0; }));

    // bob would need a second session; the sandboxed refusal process takes his
    // connection as far as Set Error Info.
    auto bob = connect(daemon, "bob", "Bob-pass1");
    bob->activate(320, 240);
    CHECK_FALSE(bob->pump_until([] { return false; }));
    CHECK(bob->error_info() == 0x00000007U);  // ERRINFO_SERVER_DENIED_CONNECTION
    CHECK_FALSE(alice->ended());
}
