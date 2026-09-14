// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace farland::daemon;
using namespace std::chrono_literals;

TEST_CASE("An empty farland.toml gives the defaults")
{
    const auto config = parse_config("").value();
    CHECK(config.server.bind == "0.0.0.0");
    CHECK(config.server.port == 3389);
    CHECK_FALSE(config.server.certificate.has_value());
    CHECK(config.auth.mode == AuthMode::store);
    CHECK(config.auth.credential_store == "/var/lib/farland/users");
    CHECK(config.session.desktop == DesktopKind::gnome);
    CHECK(config.session.command.empty());
    CHECK(config.policy.disconnected_timeout == 0s);
    CHECK(config.policy.idle_timeout == 0s);
    CHECK(config.policy.max_sessions == 0);
    CHECK(config.policy.max_sessions_per_user == 1);
    CHECK(config.policy.on_local_session == LocalSessionPolicy::refuse);
}

TEST_CASE("A complete farland.toml parses")
{
    const auto config = parse_config(R"(# farlandd
[server]
bind = "::"
port = 3390
certificate = "/etc/farland/tls.crt"
private_key = "/etc/farland/tls.key"

[auth]
mode = "store"
credential_store = "/var/lib/farland/nla-users"

[session]
desktop = "cage"
command = ["firefox", "--kiosk", "https://example.org"]

[policy]
disconnected_timeout = "2h"
idle_timeout = 900
max_sessions = 20
max_sessions_per_user = 1
on_local_session = "attach"
)")
                            .value();
    CHECK(config.server.bind == "::");
    CHECK(config.server.port == 3390);
    CHECK(config.server.certificate == std::filesystem::path("/etc/farland/tls.crt"));
    CHECK(config.server.private_key == std::filesystem::path("/etc/farland/tls.key"));
    CHECK(config.auth.credential_store == "/var/lib/farland/nla-users");
    CHECK(config.session.desktop == DesktopKind::cage);
    CHECK(config.session.command == std::vector<std::string>{"firefox", "--kiosk", "https://example.org"});
    CHECK(config.policy.disconnected_timeout == 2h);
    CHECK(config.policy.idle_timeout == 900s);
    CHECK(config.policy.max_sessions == 20);
    CHECK(config.policy.on_local_session == LocalSessionPolicy::attach);

    const auto kerberos = parse_config("[auth]\nmode = \"kerberos\"\nkeytab = \"/etc/krb5.keytab\"\n").value();
    CHECK(kerberos.auth.mode == AuthMode::kerberos);
    CHECK(kerberos.auth.keytab == std::filesystem::path("/etc/krb5.keytab"));
    CHECK(to_string(kerberos.auth.mode) == "kerberos");
    CHECK(to_string(DesktopKind::labwc) == "labwc");
}

TEST_CASE("Timeouts take seconds or a number with a unit")
{
    const auto timeout = [](std::string_view value) {
        return parse_config("[policy]\nidle_timeout = " + std::string(value) + "\n").value().policy.idle_timeout;
    };
    CHECK(timeout("0") == 0s);
    CHECK(timeout("3600") == 1h);
    CHECK(timeout("\"90s\"") == 90s);
    CHECK(timeout("\"30m\"") == 30min);
    CHECK(timeout("\"2h\"") == 2h);
    CHECK(timeout("\"1d\"") == 24h);
    CHECK(timeout("\"0s\"") == 0s);
}

TEST_CASE("Configuration errors name the setting and its line")
{
    const auto check = [](std::string_view text, std::string_view message, std::uint32_t line) {
        const auto config = parse_config(text);
        REQUIRE_FALSE(config.has_value());
        INFO(config.error().message);
        CHECK(config.error().message.find(message) != std::string::npos);
        CHECK(config.error().line == line);
    };
    check("[server]\nport = 70000\n", "[server] port must be between 1 and 65535", 2);
    check("[server]\nport = \"3389\"\n", "[server] port must be an integer", 2);
    check("[server]\nbind = \"localhost\"\n", "IPv4 or IPv6 address", 2);
    check("[server]\ncertificate = \"/etc/farland/tls.crt\"\n", "certificate and private_key go together", 2);
    check("[sever]\nport = 1\n", "unknown section [sever]", 1);
    check("server = 1\n", "server must be a section", 1);
    check("[policy]\n\nidle_timout = 5\n", "unknown key idle_timout in [policy]", 3);
    check("[auth]\nmode = \"pam\"\n", "[auth] mode must be one of store, kerberos, not \"pam\"", 2);
    check("[auth]\ncredential_store = \"users\"\n", "must be an absolute path", 2);
    check("[auth]\nkeytab = \"/etc/krb5.keytab\"\n", "keytab needs mode = \"kerberos\"", 2);
    check("[session]\ndesktop = \"kde\"\n", "one of gnome, plasma, sway, labwc, cage", 2);
    check("[session]\ndesktop = \"cage\"\n", "needs [session] command", 2);
    check("[session]\ndesktop = \"cage\"\ncommand = []\n", "non-empty array of strings", 3);
    check("[session]\ndesktop = \"cage\"\ncommand = [\"a\", 1]\n", "only strings", 3);
    check("[session]\ncommand = [\"firefox\"]\n", "only for desktop = \"cage\"", 2);
    check("[policy]\nmax_sessions_per_user = 2\n", "can only be 1", 2);
    check("[policy]\nmax_sessions = -1\n", "between 0 and", 2);
    check("[policy]\non_local_session = \"steal\"\n", "one of refuse, attach", 2);
    check("[policy]\ndisconnected_timeout = \"5w\"\n", "must be seconds or a number with s, m, h or d", 2);
    check("[policy]\ndisconnected_timeout = \"-5m\"\n", "must be seconds", 2);
    check("[policy]\ndisconnected_timeout = \"999999d\"\n", "at most 366 days", 2);
    check("[policy]\nidle_timeout = 1.5\n", "must be seconds or a string", 2);
    check("\n[server\n", "", 2);  // TOML syntax errors come from toml++

    const auto error = parse_config("[server]\nport = 0\n").error();
    CHECK(error.describe("farland.toml").starts_with("farland.toml:2:8: [server] port"));
    CHECK(ConfigError{"cannot read it"}.describe("x.toml") == "x.toml: cannot read it");
}

TEST_CASE("load_config reads the file; a missing file gives the defaults")
{
    std::random_device rd;
    const auto dir = std::filesystem::temp_directory_path() / ("farland-config-" + std::to_string(rd()));
    std::filesystem::create_directories(dir);
    CHECK(load_config(dir / "missing.toml").value().server.port == 3389);

    const auto path = dir / "farland.toml";
    std::ofstream(path) << "[session]\ndesktop = \"sway\"\n";
    CHECK(load_config(path).value().session.desktop == DesktopKind::sway);
    std::ofstream(path) << "[session]\ndesktop = \"weston\"\n";
    CHECK_FALSE(load_config(path).has_value());
    std::filesystem::remove_all(dir);
}
