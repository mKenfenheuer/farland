// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include <chrono>
#include <cstdint>
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

/// Whether the account's session on a local seat is showing its lock
/// screen. A locked GNOME session refuses to be shared at all, so a client
/// coming back to one has to be let in through it first
/// (apps/farland-agent/unlock.hpp).
[[nodiscard]] bool session_locked_at_the_machine(uid_t uid);

/// Whether the screen at the machine is showing a graphical session of
/// `uid` right now: a local session that logind calls active, which is the
/// one the seat has switched to. A session that is merely open (the seat
/// shows a login screen, or another user's session) is not it.
///
/// This is what says whether taking the session over takes it away from
/// somebody, and so whether they are asked first ([policy] takeover).
[[nodiscard]] bool session_shows_at_the_machine(uid_t uid);

/// Whether logind still knows the session with this id. A GNOME session
/// whose compositor has died takes its logind session with it, and what is
/// left of farland's session cannot serve anybody: there is no Mutter to
/// attach to and never will be again.
[[nodiscard]] bool login_session_exists(const std::string& id);

/// logind's TerminateSession, as root.
[[nodiscard]] Result<void> terminate_login_session(const std::string& id);

/// Which of GDM's two display factories starts a session.
enum class DisplayFactory : std::uint8_t {
    /// LocalDisplayFactory: the session gets a virtual terminal on the
    /// machine's seat, exactly as a login at the greeter would. logind
    /// records it as Remote=no on seat0, which is what lets the greeter
    /// offer it again later: logging in there switches to the session that
    /// is already open, with its windows.
    local,
    /// RemoteDisplayFactory: a headless session with no seat and Remote=yes.
    /// The greeter never shows it, so logging in at the machine starts a
    /// second, empty session instead of coming back to this one.
    remote,
};

/// Asks GDM for a GNOME session of `user`, logged in without a password
/// through its gdm-autologin PAM service (CreateUserDisplay on `factory`,
/// what gnome-headless-session@.service does for the remote one), and waits
/// until the display runs a logind session of `uid`. Returns the logind
/// session id. As root (GDM's bus policy and polkit).
///
/// CreateRemoteDisplay with a preauthenticated-user would name the display
/// after farland's session, but GDM 50.0 (Ubuntu 26.04) has no
/// preauthenticated-user and only starts a greeter there.
[[nodiscard]] Result<std::string> create_gdm_user_display(const std::string& user, uid_t uid, DisplayFactory factory,
                                                          std::chrono::seconds timeout);
/// Ends that session (DestroyUserDisplay). Both factories are asked, because
/// which one holds the display is farlandd's own bookkeeping and a restart
/// may have lost it; the one that does not have it answers harmlessly.
[[nodiscard]] Result<void> destroy_gdm_user_display(const std::string& user);

/// Starts `argv` as the transient service `unit` in `user`'s systemd
/// instance, with `environment` added. As root.
[[nodiscard]] Result<void> start_user_unit(const std::string& user, const std::string& unit,
                                           const std::vector<std::string>& argv,
                                           const std::vector<std::string>& environment);
[[nodiscard]] Result<void> stop_user_unit(const std::string& user, const std::string& unit);

}  // namespace farland::daemon
