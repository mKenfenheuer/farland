// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/server/broker.hpp>

#include "config.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

/// How farlandd starts a session's farland-agent (docs/ROADMAP.md M7):
///
/// - Plasma, sway, labwc, cage (and the test pattern) through farland's own
///   PAM and logind session: farlandd spawns itself as a session helper,
///   which opens a PAM session for the account (service "farland"; pam_systemd
///   registers a remote wayland session and starts the user's manager), then
///   runs the agent as the user and closes the session when it exits.
/// - GNOME through GDM (logind.hpp): GDM creates the session, and farlandd
///   starts the agent in the user's service manager.
/// - Development (`farlandd --no-pam`): the agent runs as farlandd's own user.
///
/// The session token reaches the agent through a pipe on descriptor 3, or,
/// through the service manager, in FARLAND_AGENT_TOKEN.
namespace farland::daemon {

inline constexpr int agent_token_fd = 3;

/// A new agent's command line.
struct AgentLaunch {
    std::filesystem::path agent;  ///< farland-agent
    std::string socket;           ///< farlandd's agent socket
    std::uint32_t session_id = 0;
    DesktopKind desktop = DesktopKind::test;
    bool attach = false;
    std::vector<std::string> cage_command;
    std::string log_level = "info";
    /// The token comes on descriptor 3 (else in the environment).
    bool token_on_fd = true;
};

[[nodiscard]] std::vector<std::string> agent_arguments(const AgentLaunch& launch);

/// A local account from the password database.
struct Account {
    std::string name;
    uid_t uid = 0;
    gid_t gid = 0;
    std::string home;
    std::string shell;
};
[[nodiscard]] std::optional<Account> lookup_account(const std::string& name);

/// XDG_SESSION_DESKTOP and XDG_CURRENT_DESKTOP for a desktop.
[[nodiscard]] std::string_view session_desktop_name(DesktopKind desktop) noexcept;
/// The PAM environment before pam_open_session: what pam_systemd reads to
/// register the logind session (type wayland, class user, the desktop).
[[nodiscard]] std::vector<std::string> pam_session_variables(DesktopKind desktop);
/// The agent's environment: PAM's (pam_env, and pam_systemd's
/// XDG_RUNTIME_DIR and XDG_SESSION_ID) plus the account's basics where PAM
/// left them out, and the session bus of the user's manager.
[[nodiscard]] std::vector<std::string> agent_environment(std::span<const std::string> pam_environment,
                                                         const Account& account, DesktopKind desktop);
/// "1.2.3.4" of "1.2.3.4:5678", "::1" of "[::1]:5678" or "::1:5678": the
/// client's host for PAM_RHOST.
[[nodiscard]] std::string remote_host(std::string_view peer);

/// Starts the agent as this process's user (farlandd --no-pam), with
/// `environment`. Returns its pid.
[[nodiscard]] Result<pid_t> spawn_agent(const AgentLaunch& launch, const server::broker::Token& token,
                                        std::span<const std::string> environment);

/// Starts `self --session-helper` (root) for `account`: a PAM session, and
/// the agent in it as the user. Returns the helper's pid; SIGTERM to it
/// ends the agent and closes the session.
[[nodiscard]] Result<pid_t> spawn_session_helper(const std::filesystem::path& self, const AgentLaunch& launch,
                                                 const std::string& account, const std::string& rhost,
                                                 const server::broker::Token& token);

/// `farlandd --session-helper --user NAME --desktop KIND --rhost HOST -- AGENT ARGS...`,
/// with the token on descriptor 3. Returns the exit status.
[[nodiscard]] int run_session_helper(std::span<char*> args);

/// Whether this build can open PAM sessions.
[[nodiscard]] bool pam_supported() noexcept;

}  // namespace farland::daemon
