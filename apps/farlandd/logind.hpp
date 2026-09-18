// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

/// What farlandd asks of systemd and GDM (docs/ROADMAP.md M7): the user's
/// logind sessions, GDM's headless remote displays for GNOME, and transient
/// units in a user's service manager for agents that run inside a session
/// someone else started. Linux with libsystemd; elsewhere every call fails
/// with Errc::unsupported and there are no sessions.
///
/// The calls block (D-Bus round trips, GDM starting a session); farlandd
/// makes the slow ones on a thread of their own.
namespace farland::daemon {

/// One logind session (sd-login).
struct LoginSession {
    std::string id;
    std::string seat;           ///< empty: no seat (remote, headless)
    std::string type;           ///< wayland, x11, tty, unspecified
    std::string session_class;  ///< user, greeter, manager, ...
    std::string state;          ///< online, active, closing
    bool remote = false;
};

/// A graphical user session on a local seat, which on_local_session is
/// about. Headless sessions (GDM's remote displays, farland's own) have no seat.
[[nodiscard]] bool is_local_graphical(const LoginSession& session);

[[nodiscard]] std::vector<LoginSession> user_sessions(uid_t uid);
[[nodiscard]] std::optional<LoginSession> local_graphical_session(uid_t uid);

/// logind's TerminateSession, as root.
[[nodiscard]] Result<void> terminate_login_session(const std::string& id);

/// Asks GDM for a headless GNOME session of `user`, logged in without a
/// password through its gdm-autologin PAM service
/// (RemoteDisplayFactory.CreateUserDisplay, what gnome-headless-session@.service
/// does), and waits until the display runs a logind session of `uid`.
/// Returns the logind session id. As root (GDM's bus policy and polkit).
///
/// CreateRemoteDisplay with a preauthenticated-user would name the display
/// after farland's session, but GDM 50.0 (Ubuntu 26.04) has no
/// preauthenticated-user and only starts a greeter there.
[[nodiscard]] Result<std::string> create_gdm_user_display(const std::string& user, uid_t uid,
                                                          std::chrono::seconds timeout);
/// Ends that session (RemoteDisplayFactory.DestroyUserDisplay).
[[nodiscard]] Result<void> destroy_gdm_user_display(const std::string& user);

/// Starts `argv` as the transient service `unit` in `user`'s systemd
/// instance, with `environment` added. As root.
[[nodiscard]] Result<void> start_user_unit(const std::string& user, const std::string& unit,
                                           const std::vector<std::string>& argv,
                                           const std::vector<std::string>& environment);
[[nodiscard]] Result<void> stop_user_unit(const std::string& user, const std::string& unit);

}  // namespace farland::daemon
