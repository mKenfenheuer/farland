// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// farlandd: the multi-session RDP daemon (docs/ROADMAP.md M7). It runs as
// root from farlandd.service; `--no-pam` runs it as any user for
// development and tests, with every session as that user.

#include <farland/auth/tls_identity.hpp>
#include <farland/base/log.hpp>

#include "daemon.hpp"
#include "launcher.hpp"
#include "nla.hpp"
#include "privsep_process.hpp"
#include "sandbox.hpp"

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <unistd.h>

#ifndef FARLAND_AGENT_PATH
#define FARLAND_AGENT_PATH "/usr/local/bin/farland-agent"
#endif

namespace {

namespace log = farland::log;
namespace app = farland::app;
namespace daemon = farland::daemon;
constexpr std::string_view log_component = "daemon";

std::atomic<bool> stop_requested{false};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): signal state

extern "C" void on_signal(int /*signal*/)
{
    stop_requested.store(true);
}

void usage()
{
    std::cout << "usage: farlandd [options]\n"
                 "Serves multi-session RDP: one headless desktop per user behind one port.\n"
                 "  --config FILE        configuration (default /etc/farland/farland.toml)\n"
                 "  --check-config       parse the configuration, print every effective setting and exit\n"
                 "  --port N             listen on this port instead of [server] port\n"
                 "  --no-pam             development mode: run every session as this user, without PAM\n"
                 "                       and logind (also works without root)\n"
                 "  --runtime-dir DIR    where the agent socket goes (default /run/farland; with --no-pam\n"
                 "                       $XDG_RUNTIME_DIR/farland)\n"
                 "  --state-dir DIR      where a generated TLS certificate goes (default /var/lib/farland;\n"
                 "                       with --no-pam $XDG_STATE_HOME/farland)\n"
                 "  --agent PATH         farland-agent (default: next to farlandd, else " FARLAND_AGENT_PATH ")\n"
                 "  --hostname NAME      certificate and NTLM host name (default: this host's name)\n"
                 "  --log-level LEVEL    trace, debug, info, warn, error (default: [server] log_level)\n"
                 "\n"
                 "Users enrol with farlandctl passwd (the credential store in [auth]). See the multi-session\n"
                 "section of README.md.\n";
}

bool set_log_level(const std::string& level)
{
    static constexpr std::array levels{"trace", "debug", "info", "warn", "error"};
    const auto* found = std::ranges::find(levels, level);
    if (found == levels.end()) {
        return false;
    }
    log::set_level(static_cast<log::Level>(found - levels.begin()));
    return true;
}

std::string local_hostname()
{
    std::string name(256, '\0');
    if (::gethostname(name.data(), name.size()) != 0) {
        return "localhost";
    }
    name.resize(name.find('\0'));
    return name.empty() ? "localhost" : name;
}

std::filesystem::path user_dir(const char* xdg, const char* fallback)
{
    if (const char* value = std::getenv(xdg); value != nullptr && *value != '\0') {
        return std::filesystem::path(value) / "farland";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / fallback / "farland";
    }
    return std::filesystem::temp_directory_path() / ("farland-" + std::to_string(::getuid()));
}

/// The value after `name` in `args`, if any.
std::optional<std::string> option(std::span<char*> args, std::string_view name)
{
    for (std::size_t i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == name) {
            return std::string(args[i + 1]);
        }
    }
    return std::nullopt;
}

bool has_flag(std::span<char*> args, std::string_view name)
{
    return std::ranges::any_of(args.subspan(1), [name](const char* a) { return a == name; });
}

/// The network process of one client (as farland-server's).
int run_network_child(std::span<char*> args)
{
    std::signal(SIGPIPE, SIG_IGN);
    set_log_level(option(args, "--log-level").value_or("info"));
    const std::string hostname = option(args, "--hostname").value_or("localhost");
    auto identity = farland::auth::TlsIdentity::load_or_create(option(args, "--cert").value_or(""),
                                                               option(args, "--key").value_or(""), hostname);
    if (!identity) {
        log::error(log_component, "network process: cannot load the TLS identity: {}", identity.error().message());
        return 1;
    }
    if (!app::enter_network_sandbox()) {
        return 1;
    }
    app::SessionOptions options;
    options.preauth.require_nla = true;
    options.preauth.advertise_gfx = true;
    if (const auto timeout = option(args, "--activation-timeout")) {
        options.activation_timeout = static_cast<unsigned>(std::strtoul(timeout->c_str(), nullptr, 10));
    }
    const auto& tls = *identity;
    return app::run_network_child(
        option(args, "--peer").value_or("?"), tls, options,
        [&](farland::auth::NtlmVerifier& verifier) { return app::make_nla_factory(tls, verifier, hostname); });
}

}  // namespace

int main(int argc, char** argv)
{
    const std::span args(argv, static_cast<std::size_t>(argc));
    if (has_flag(args, "--privsep-child")) {
        return run_network_child(args);
    }
    if (has_flag(args, "--refuse-child")) {
        std::signal(SIGPIPE, SIG_IGN);
        set_log_level(option(args, "--log-level").value_or("info"));
        const auto number = [&](std::string_view name) {
            return static_cast<std::uint32_t>(std::strtoul(option(args, name).value_or("0").c_str(), nullptr, 10));
        };
        return daemon::run_refusal(3, number("--selected-protocol"), number("--error-info"));
    }
    if (has_flag(args, "--session-helper")) {
        set_log_level(option(args, "--log-level").value_or("info"));
        return daemon::run_session_helper(args);
    }

    daemon::DaemonOptions options;
    std::filesystem::path config_path(daemon::default_config_path);
    std::optional<std::uint16_t> port;
    bool check = false;
    bool log_level_given = false;  ///< --log-level wins over [server] log_level
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const bool has_value = i + 1 < args.size();
        if (arg == "--config" && has_value) {
            config_path = args[++i];
        } else if (arg == "--check-config") {
            check = true;
        } else if (arg == "--port" && has_value) {
            const long value = std::strtol(args[++i], nullptr, 10);
            if (value < 1 || value > 65535) {
                std::cerr << "farlandd: --port must be 1 to 65535\n";
                return 2;
            }
            port = static_cast<std::uint16_t>(value);
        } else if (arg == "--no-pam") {
            options.no_pam = true;
        } else if (arg == "--runtime-dir" && has_value) {
            options.runtime_dir = args[++i];
        } else if (arg == "--state-dir" && has_value) {
            options.state_dir = args[++i];
        } else if (arg == "--agent" && has_value) {
            options.agent = args[++i];
        } else if (arg == "--hostname" && has_value) {
            options.hostname = args[++i];
        } else if (arg == "--log-level" && has_value) {
            options.log_level = args[++i];
            if (!set_log_level(options.log_level)) {
                std::cerr << "farlandd: unknown log level " << options.log_level << "\n";
                return 2;
            }
            log_level_given = true;
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return 0;
        } else {
            std::cerr << "farlandd: unknown option " << arg << "\n";
            usage();
            return 2;
        }
    }

    auto config = daemon::load_config(config_path);
    if (!config) {
        std::cerr << "farlandd: " << config.error().describe(config_path.string()) << "\n";
        return 2;
    }
    if (port) {
        config->server.port = *port;
    }
    if (check) {
        std::cout << config_path.string() << ": ok\n" << daemon::describe(*config);
        return 0;
    }
    if (!log_level_given) {
        options.log_level = std::string(daemon::to_string(config->server.log_level));
        set_log_level(options.log_level);
    }
    if (config->auth.mode == daemon::AuthMode::kerberos) {
        std::cerr << "farlandd: [auth] mode = \"kerberos\" is not implemented yet\n";
        return 2;
    }
    if (!options.no_pam) {
        if (::geteuid() != 0) {
            std::cerr << "farlandd: starting sessions for other users needs root; --no-pam runs every session "
                         "as you (development mode)\n";
            return 2;
        }
        const bool needs_pam = config->session.desktop != daemon::DesktopKind::gnome;
        if (needs_pam && !daemon::pam_supported()) {
            std::cerr << "farlandd: this build has no PAM support (-Dpam); use --no-pam or desktop = \"gnome\"\n";
            return 2;
        }
    }
    options.config = std::move(*config);
    options.self = app::current_executable(argv[0]);
    if (options.agent.empty()) {
        const auto sibling = options.self.parent_path() / "farland-agent";
        options.agent = std::filesystem::exists(sibling) ? sibling : std::filesystem::path(FARLAND_AGENT_PATH);
    }
    if (options.runtime_dir.empty()) {
        options.runtime_dir = options.no_pam ? user_dir("XDG_RUNTIME_DIR", ".cache") : "/run/farland";
    }
    if (options.state_dir.empty()) {
        options.state_dir = options.no_pam ? user_dir("XDG_STATE_HOME", ".local/state") : "/var/lib/farland";
    }
    if (options.hostname.empty()) {
        options.hostname = local_hostname();
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);
    daemon::Daemon d(std::move(options));
    return d.run(stop_requested);
}
