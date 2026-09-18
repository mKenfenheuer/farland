// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

/// [policy] seat_takeover: the person at the machine logs in to take back a
/// session a client holds. The display manager's PAM stack asks farlandd
/// before it lets them in (packaging/pam/), farlandd asks the client through
/// the session's agent, and the login waits for the answer.
///
/// The question comes in on the D-Bus thread and is answered from the
/// daemon's loop, so this is the bit in between: it holds the waiting logins
/// and the accounts whose session a client has. Everything here is safe to
/// call from either thread, and nothing here does I/O.
///
/// Nobody may be kept waiting at a login screen by farland going wrong, so
/// every path that does not have a clear answer lets the login through:
/// an account no client holds, a question the loop never picks up, a wait
/// that runs out, farlandd shutting down.
namespace farland::daemon {

class SeatTakeoverGate {
public:
    using Clock = std::chrono::steady_clock;

    /// A login waiting to be asked about, for the daemon's loop.
    struct Waiting {
        std::uint64_t cookie = 0;
        std::string account;
    };

    /// D-Bus thread: somebody is logging in as `account` at the machine.
    /// A cookie to wait on when a client holds that account's session;
    /// nullopt when none does, and the login goes ahead at once.
    [[nodiscard]] std::optional<std::uint64_t> begin(const std::string& account);

    /// D-Bus thread: waits for the answer to `cookie`, at most `timeout`.
    /// True lets the login through, which is also what a timeout, an
    /// unknown cookie and a shutdown give.
    [[nodiscard]] bool await(std::uint64_t cookie, std::chrono::milliseconds timeout);

    /// Daemon loop: the accounts whose session a client holds now. Only
    /// these are ever asked about.
    void set_held_accounts(std::vector<std::string> accounts);

    /// Daemon loop: the logins nobody has been asked about yet.
    [[nodiscard]] std::vector<Waiting> take_new();

    /// Daemon loop: the answer for `cookie`; unknown cookies are ignored
    /// (the login gave up).
    void resolve(std::uint64_t cookie, bool allowed);

    /// Daemon loop: farlandd is going away, so every login waiting goes
    /// through and nothing new is held.
    void release_all();

    /// How many logins are waiting (for the tests).
    [[nodiscard]] std::size_t waiting() const;

private:
    struct Pending {
        std::string account;
        bool taken = false;   ///< the loop has it
        bool done = false;    ///< it has an answer
        bool allowed = true;  ///< what the answer was
    };

    mutable std::mutex mutex_;
    std::condition_variable answered_;
    std::map<std::uint64_t, Pending> pending_;
    std::vector<std::string> held_;
    std::uint64_t next_cookie_ = 1;
    bool closed_ = false;
};

}  // namespace farland::daemon
