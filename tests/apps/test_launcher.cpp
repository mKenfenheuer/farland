// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/credential_store.hpp>
#include <farland/auth/ntlm.hpp>

#include "agent_token.hpp"
#include "enrol.hpp"
#include "headless.hpp"
#include "launcher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <unistd.h>

using farland::daemon::Account;
using farland::daemon::AgentLaunch;
using farland::daemon::DesktopKind;

namespace {

bool contains(const std::vector<std::string>& list, const std::string& item)
{
    return std::ranges::find(list, item) != list.end();
}

}  // namespace

TEST_CASE("Launcher: the agent's command line")
{
    AgentLaunch launch;
    launch.agent = "/usr/bin/farland-agent";
    launch.socket = "/run/farland/agent.sock";
    launch.session_id = 12;
    launch.desktop = DesktopKind::plasma;
    launch.log_level = "debug";
    const std::vector<std::string> expected{"/usr/bin/farland-agent",
                                            "--socket",
                                            "/run/farland/agent.sock",
                                            "--session-id",
                                            "12",
                                            "--desktop",
                                            "plasma",
                                            "--log-level",
                                            "debug",
                                            "--token-fd",
                                            "3"};
    CHECK(farland::daemon::agent_arguments(launch) == expected);

    // Through a service manager: the token is in the environment; attached; cage's application after --.
    launch.token_on_fd = false;
    launch.attach = true;
    launch.desktop = DesktopKind::cage;
    launch.cage_command = {"firefox", "--kiosk", "https://example.org"};
    const auto args = farland::daemon::agent_arguments(launch);
    CHECK_FALSE(contains(args, "--token-fd"));
    CHECK(contains(args, "--attach"));
    const auto dashes = std::ranges::find(args, std::string("--"));
    REQUIRE(dashes != args.end());
    CHECK(std::vector<std::string>(dashes + 1, args.end()) == launch.cage_command);
    // Only cage takes a command.
    launch.desktop = DesktopKind::sway;
    CHECK_FALSE(contains(farland::daemon::agent_arguments(launch), "--"));
}

TEST_CASE("Launcher: PAM variables and the agent's environment")
{
    CHECK(farland::daemon::pam_session_variables(DesktopKind::plasma) ==
          std::vector<std::string>{"XDG_SESSION_TYPE=wayland", "XDG_SESSION_CLASS=user", "XDG_SESSION_DESKTOP=KDE"});
    CHECK(farland::daemon::session_desktop_name(DesktopKind::labwc) == "labwc");

    const Account alice{"alice", 1001, 1001, "/home/alice", "/bin/bash"};
    const std::vector<std::string> pam{"XDG_RUNTIME_DIR=/run/user/1001",
                                       "XDG_SESSION_ID=42",
                                       "LANG=de_DE.UTF-8",
                                       "HOME=/srv/alice",
                                       "FARLAND_AGENT_TOKEN=00",
                                       "garbage"};
    const auto env = farland::daemon::agent_environment(pam, alice, DesktopKind::sway);
    CHECK(contains(env, "XDG_RUNTIME_DIR=/run/user/1001"));
    CHECK(contains(env, "XDG_SESSION_ID=42"));
    CHECK(contains(env, "LANG=de_DE.UTF-8"));
    CHECK(contains(env, "HOME=/srv/alice"));  // PAM's value wins
    CHECK_FALSE(contains(env, "HOME=/home/alice"));
    CHECK(contains(env, "USER=alice"));
    CHECK(contains(env, "LOGNAME=alice"));
    CHECK(contains(env, "SHELL=/bin/bash"));
    CHECK(contains(env, "XDG_SESSION_TYPE=wayland"));
    CHECK(contains(env, "XDG_SESSION_CLASS=user"));
    CHECK(contains(env, "XDG_SESSION_DESKTOP=sway"));
    CHECK(contains(env, "XDG_CURRENT_DESKTOP=sway"));
    CHECK(contains(env, "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1001/bus"));
    CHECK(std::ranges::none_of(env, [](const std::string& e) { return e.starts_with("FARLAND_AGENT_TOKEN"); }));
    CHECK_FALSE(contains(env, "garbage"));
    CHECK(std::ranges::count_if(env, [](const std::string& e) { return e.starts_with("PATH="); }) == 1);

    // Without PAM's runtime directory there is no bus to name.
    const auto bare = farland::daemon::agent_environment({}, alice, DesktopKind::test);
    CHECK(std::ranges::none_of(bare, [](const std::string& e) { return e.starts_with("DBUS_SESSION_BUS_ADDRESS"); }));
}

TEST_CASE("Launcher: the remote host for PAM_RHOST")
{
    CHECK(farland::daemon::remote_host("192.0.2.7:51234") == "192.0.2.7");
    CHECK(farland::daemon::remote_host("[2001:db8::1]:3389") == "2001:db8::1");
    CHECK(farland::daemon::remote_host("unknown peer") == "unknown peer");
}

TEST_CASE("Agent token as hex")
{
    farland::server::broker::Token token{};
    for (std::size_t i = 0; i < token.size(); ++i) {
        token.at(i) = static_cast<std::byte>(i * 7);
    }
    const auto text = farland::app::token_to_hex(token);
    CHECK(text.size() == 64);
    CHECK(text.starts_with("00070e15"));
    CHECK(farland::app::token_from_hex(text) == token);
    CHECK_FALSE(farland::app::token_from_hex(text.substr(1)).has_value());
    CHECK_FALSE(farland::app::token_from_hex("zz" + text.substr(2)).has_value());
    CHECK_FALSE(farland::app::token_from_hex("0A" + text.substr(2)).has_value());  // lower case only
}

TEST_CASE("Headless dispatcher: names, and backends this build lacks fail cleanly")
{
    using farland::app::HeadlessKind;
    for (const auto kind :
         {HeadlessKind::gnome, HeadlessKind::plasma, HeadlessKind::sway, HeadlessKind::labwc, HeadlessKind::cage}) {
        CHECK(farland::app::parse_headless_kind(farland::app::to_string(kind)) == kind);
    }
    CHECK_FALSE(farland::app::parse_headless_kind("kde").has_value());

    farland::app::HeadlessOptions options;
    options.kind = HeadlessKind::cage;
    const auto cage = farland::app::start_headless_desktop(options);
    REQUIRE_FALSE(cage.has_value());
    CHECK(cage.error().code == farland::Errc::invalid_value);  // no application to run
    for (const auto kind : {HeadlessKind::gnome, HeadlessKind::plasma, HeadlessKind::sway}) {
        if (farland::app::headless_backend_built(kind)) {
            continue;  // a real compositor would start; not here
        }
        options.kind = kind;
        const auto desktop = farland::app::start_headless_desktop(options);
        REQUIRE_FALSE(desktop.has_value());
        CHECK(desktop.error().code == farland::Errc::unsupported);
    }
}

TEST_CASE("Self-enrolment stores the caller's own account")
{
    const auto dir = std::filesystem::temp_directory_path() / ("farland-enrol-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    const auto path = dir / "users";
    REQUIRE(farland::daemon::store_enrolment(path, "alice", "", "Secret1!").has_value());
    REQUIRE(farland::daemon::store_enrolment(path, "bob", "CORP", "Other2?").has_value());
    // A second enrolment changes the password.
    REQUIRE(farland::daemon::store_enrolment(path, "alice", "", "Newer3#").has_value());

    const auto store = farland::auth::CredentialStore::load(path);
    REQUIRE(store.has_value());
    CHECK(store->entries().size() == 2);
    const auto* alice = store->find("alice", "WHATEVER");
    REQUIRE(alice != nullptr);
    CHECK(alice->account() == "alice");
    CHECK(alice->local_account == "alice");
    CHECK(alice->hash == farland::auth::ntlm::nt_hash("Newer3#"));
    const auto* bob = store->find("bob", "corp");
    REQUIRE(bob != nullptr);
    CHECK(bob->hash == farland::auth::ntlm::nt_hash("Other2?"));
    CHECK((std::filesystem::status(path).permissions() & std::filesystem::perms::group_all) ==
          std::filesystem::perms::none);

    CHECK_FALSE(farland::daemon::store_enrolment(path, "eve:root", "", "x").has_value());
    CHECK_FALSE(farland::daemon::store_enrolment(path, "eve", "", "").has_value());
    CHECK_FALSE(farland::daemon::store_enrolment(path, "-eve", "", "x").has_value());
    std::filesystem::remove_all(dir);
}
