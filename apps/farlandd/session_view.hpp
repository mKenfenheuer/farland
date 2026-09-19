// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

/// What `farlandctl sessions` and `farlandctl terminate` need of farlandd
/// (docs/ROADMAP.md M7, S5): who is logged in and a way to end a session.
///
/// The questions come in on the D-Bus thread and the answers live in the
/// daemon's loop, so this is the bit in between: the loop publishes a
/// snapshot of its registry after every turn, and the requests to end a
/// session wait here until the loop picks them up. Everything here is safe
/// to call from either thread, and nothing here does I/O.
namespace farland::daemon {

class SessionView {
public:
    using Clock = std::chrono::steady_clock;

    /// One session, as the control API reports it.
    struct Entry {
        std::uint32_t id = 0;
        std::string account;
        /// "starting", "running" or "ending".
        std::string state;
        /// The desktop it runs: "gnome", "plasma", "test" and so on.
        std::string desktop;
        /// It shows the user's local session rather than a headless one.
        bool attached = false;
        /// A client is connected now.
        bool connected = false;
        /// The client's address while one is connected; empty otherwise.
        std::string peer;
        /// Seconds since the session started.
        std::uint64_t age_seconds = 0;
        /// Seconds since the last client left; 0 while one is connected.
        std::uint64_t disconnected_seconds = 0;
        /// Seconds the client has sent no input, from the agent's last Stats.
        std::uint32_t idle_seconds = 0;
    };

    /// Daemon loop: what the registry holds now.
    void publish(std::vector<Entry> sessions);
    /// D-Bus thread: the last snapshot the loop published.
    [[nodiscard]] std::vector<Entry> list() const;

    /// D-Bus thread: end session `id`. False when the snapshot has no such
    /// session, which is the answer the caller gets; a session that goes
    /// away between the snapshot and the loop is simply not there any more,
    /// and the loop ignores the request.
    [[nodiscard]] bool request_terminate(std::uint32_t id);
    /// D-Bus thread: end every session of `account`; the number asked for.
    [[nodiscard]] std::size_t request_terminate_account(const std::string& account);
    /// Daemon loop: the sessions asked to end since the last call.
    [[nodiscard]] std::vector<std::uint32_t> take_terminations();

private:
    mutable std::mutex mutex_;
    std::vector<Entry> sessions_;
    std::vector<std::uint32_t> terminate_;
};

}  // namespace farland::daemon
