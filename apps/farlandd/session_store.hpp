// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/server/broker.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

/// The sessions farlandd is running, written down so that they outlive it
/// (docs/ROADMAP.md M7). A restart used to end every desktop: the daemon
/// told each agent to terminate and started again with nothing, which turned
/// every package upgrade into a forced logout for everyone.
///
/// It does not have to be that way. The agent, not the daemon, owns the
/// desktop and the client's socket — farlandd passed the socket over and
/// kept no copy — so a session survives the daemon's absence on its own. All
/// that is missing is a way for the two to find each other again, and that
/// is this file: the daemon writes what it would otherwise forget, and an
/// agent that reconnects is matched against it by the token it still holds
/// and the uid it runs as.
///
/// The file holds a token per session, so it is written 0600 in the state
/// directory (which the package creates 0700, root-owned). Losing it costs
/// the sessions their reattachment, nothing more: the agents give up after
/// their own timeout and end cleanly.
namespace farland::daemon {

/// One session, as much of it as has to outlive the daemon.
struct StoredSession {
    std::uint32_t id = 0;
    std::string account;
    std::uint32_t uid = 0;
    /// It shows the user's local session (on_local_session = "attach").
    bool attached = false;
    /// GDM made the login session, so GDM has to be told to destroy it.
    bool gdm = false;
    /// The logind session the agent runs in; empty when unknown.
    std::string login_session;
    /// The agent's unit in the user's service manager (the GNOME route).
    std::string unit;
    /// What the agent greets with when it comes back.
    server::broker::Token token{};

    friend bool operator==(const StoredSession&, const StoredSession&) = default;
};

/// The table as it is written: one session per line, fields separated by a
/// single space, with the empty string written as "-". Text rather than
/// anything cleverer so that an administrator looking for why a session came
/// back can read it.
[[nodiscard]] std::string encode_sessions(const std::vector<StoredSession>& sessions);

/// Parses what encode_sessions wrote. A line that does not parse is skipped
/// rather than failing the lot: a half-written file must not stop farlandd
/// from starting, and the worst it costs is one session its reattachment.
[[nodiscard]] std::vector<StoredSession> decode_sessions(std::string_view text);

/// Writes the table to `path` (0600), through a temporary file and a rename
/// so that a crash leaves either the old table or the new one.
[[nodiscard]] Result<void> save_sessions(const std::filesystem::path& path, const std::vector<StoredSession>& sessions);

/// Reads the table; an absent file is no sessions, not an error.
[[nodiscard]] std::vector<StoredSession> load_sessions(const std::filesystem::path& path);

}  // namespace farland::daemon
