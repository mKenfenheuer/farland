// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include "seat_takeover.hpp"
#include "session_view.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

/// Self-enrolment (docs/PLAN.md §3.5): a user makes their own account
/// reachable over RDP with NLA. NTLM needs the account's NT hash, which only
/// the password gives, so the user hands farlandd the password once:
///
///     farlandctl passwd  ->  D-Bus org.farland.Farland1.EnrolSelf (system bus)
///                        ->  polkit org.farland.Farland1.enrol-self (auth_self)
///                        ->  PAM "farland": the account password is right
///                        ->  the credential store gets user:domain:NT hash
///
/// The store entry names the caller's own account, so enrolment never grants
/// a login to anyone else.
namespace farland::daemon {

inline constexpr std::string_view bus_name = "org.farland.Farland1";
inline constexpr std::string_view object_path = "/org/farland/Farland1";
inline constexpr std::string_view interface_name = "org.farland.Farland1";
inline constexpr std::string_view enrol_action = "org.farland.Farland1.enrol-self";
/// Listing and ending sessions: a user may see and end their own, and an
/// administrator anybody's (org.farland.Farland1.policy).
inline constexpr std::string_view sessions_action = "org.farland.Farland1.manage-sessions";
/// The longest a login at the machine is ever held while the client that
/// has the session is asked ([policy] seat_takeover), whatever the caller
/// asks for: nobody waits at a login screen for farland.
inline constexpr std::chrono::seconds max_seat_takeover_wait{300};

/// Writes `account`'s NT hash for `password` into the store at `path`
/// (created if missing): the entry `account` in `domain`, whose local
/// account is `account` itself.
[[nodiscard]] Result<void> store_enrolment(const std::filesystem::path& path, const std::string& account,
                                           std::string_view domain, std::string_view password);

/// Whether `password` is `account`'s password, and the account may log in
/// (PAM service "farland": auth and account stacks). Linux with PAM only.
[[nodiscard]] bool check_account_password(const std::string& account, std::string_view password);

/// The D-Bus control API on the system bus, served on a thread of its own.
class ControlService {
public:
    /// `seat`: the gate the display manager's PAM module asks through;
    /// null leaves those methods answering "nothing holds it". `sessions`:
    /// what ListSessions and TerminateSession work on; null leaves them
    /// answering with nothing.
    ControlService(std::filesystem::path credential_store, SeatTakeoverGate* seat, SessionView* sessions = nullptr);
    ControlService(const ControlService&) = delete;
    ControlService& operator=(const ControlService&) = delete;
    ControlService(ControlService&&) = delete;
    ControlService& operator=(ControlService&&) = delete;
    ~ControlService();

    /// Takes the bus name and starts serving. Fails without a system bus,
    /// libsystemd or PAM, or when the name is taken.
    [[nodiscard]] Result<void> start();

    /// Opaque; public only so that the sd-bus vtable can name its handler.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace farland::daemon
