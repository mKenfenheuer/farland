// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "config.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace farland::daemon {

/// farlandd's sessions and the [policy] rules over them (docs/ROADMAP.md M7):
/// one session per local account, who may get a new one, which connection a
/// newer one replaces, and when a session has been idle or disconnected for
/// too long. No I/O and no clock of its own, so tests drive it with any
/// time they like; farlandd does what it says.
class SessionRegistry {
public:
    using Clock = std::chrono::steady_clock;

    enum class State : std::uint8_t {
        starting,  ///< its agent has not said Hello yet
        running,
        ending,  ///< told to end; waiting for the agent to go
    };

    struct Session {
        std::uint32_t id = 0;
        std::string account;  ///< the local account it runs as
        State state = State::starting;
        /// It shows the user's local session instead of a headless one
        /// (on_local_session = "attach").
        bool attached = false;
        std::uint64_t connection = 0;  ///< the client connected now; 0: none
        /// Since when no client is connected (unset while one is).
        std::optional<Clock::time_point> disconnected_since;
        std::uint32_t idle_seconds = 0;  ///< from the agent's last Stats
        bool idle_disconnect_sent = false;
    };

    enum class Admission : std::uint8_t {
        existing,              ///< hand the connection to the account's session
        create,                ///< start a headless session for it
        attach_local,          ///< start a session that shows the local one
        replace_local,         ///< end the local session, then start a headless one
        separate_local,        ///< leave it alone; start a second, headless desktop
        refuse_local_session,  ///< the account is logged in at a local seat
        refuse_limit,          ///< max_sessions reached
    };

    struct Decision {
        Admission admission = Admission::create;
        std::uint32_t session = 0;     ///< for `existing`
        std::uint32_t error_info = 0;  ///< Set Error Info for the refusals ([MS-RDPBCGR] 2.2.5.1.1)
        std::string reason;            ///< for the log
    };

    struct Action {
        enum class Kind : std::uint8_t {
            disconnect_idle,         ///< end `connection` with ERRINFO_IDLE_TIMEOUT
            terminate_disconnected,  ///< end the session: nobody came back in time
        };
        Kind kind = Kind::disconnect_idle;
        std::uint32_t session = 0;
        std::uint64_t connection = 0;
    };

    explicit SessionRegistry(PolicySection policy) : policy_(policy) {}

    /// What to do with an authenticated connection for `account`;
    /// `local_session`: the account has a graphical session on a local seat.
    [[nodiscard]] Decision admit(std::string_view account, bool local_session) const;

    /// A new session, starting; returns its id (never 0).
    std::uint32_t create(std::string account, bool attached, Clock::time_point now);
    /// A session that outlived a farlandd restart, with the id it had. Its
    /// agent has yet to come back, so it starts as `starting` and
    /// disconnected, like a new one. Ignored when `id` is taken or 0.
    void restore(std::uint32_t id, std::string account, bool attached, Clock::time_point now);
    /// Its agent greeted farlandd.
    void set_running(std::uint32_t session);
    /// `connection` now goes to `session`; returns the connection it
    /// replaces (0: none), which is to end with
    /// ERRINFO_DISCONNECTED_BY_OTHERCONNECTION.
    [[nodiscard]] std::uint64_t connect(std::uint32_t session, std::uint64_t connection);
    /// `connection` of `session` ended (another one may already have
    /// replaced it; then nothing changes).
    void disconnected(std::uint32_t session, std::uint64_t connection, Clock::time_point now);
    /// The agent's Stats for its current connection.
    void update_idle(std::uint32_t session, std::uint64_t connection, std::uint32_t idle_seconds);
    /// A connection the registry does not know about, which its agent says
    /// it is serving: after a farlandd restart the agent still has the
    /// client, and without this the session would look disconnected and be
    /// ended by `disconnected_timeout`. Does nothing when the session
    /// already has a connection.
    void adopt_connection(std::uint32_t session, std::uint64_t connection);
    void set_ending(std::uint32_t session);
    void remove(std::uint32_t session);

    /// What the policies call for now. Each action is reported once.
    [[nodiscard]] std::vector<Action> due(Clock::time_point now);

    [[nodiscard]] const Session* find(std::uint32_t session) const;
    [[nodiscard]] const Session* find_account(std::string_view account) const;
    [[nodiscard]] const std::vector<Session>& sessions() const noexcept { return sessions_; }

private:
    Session* get(std::uint32_t session);

    PolicySection policy_;
    std::vector<Session> sessions_;
    std::uint32_t next_id_ = 1;
};

}  // namespace farland::daemon
