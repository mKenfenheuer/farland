// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "config.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

/// farlandd, the multi-session daemon (docs/ROADMAP.md M7, PLAN §3.5).
///
/// It owns the port. Every client gets a sandboxed network process, as in
/// farland-server: X.224, TLS and NLA against the credential store. With the
/// NLA identity farlandd picks the local account (the store's account
/// column, else the user name), applies [policy] and finds or starts that
/// account's session, then passes the plaintext socket the network process
/// relays to the session's farland-agent (broker protocol, SCM_RIGHTS). A
/// second connection of the same account takes over the session; the first
/// ends with ERRINFO_DISCONNECTED_BY_OTHERCONNECTION.
namespace farland::daemon {

struct DaemonOptions {
    Config config;
    /// Development and CI: agents run as farlandd's own user, without PAM
    /// or logind, and every account shares that user.
    bool no_pam = false;
    std::filesystem::path runtime_dir;  ///< the agent socket, agent.sock
    std::filesystem::path state_dir;    ///< tls/ when the config names no certificate
    std::filesystem::path self;         ///< this executable: network processes, session helpers
    std::filesystem::path agent;        ///< farland-agent
    std::string hostname;
    std::string log_level = "info";
};

class Daemon {
public:
    explicit Daemon(DaemonOptions options);
    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;
    Daemon(Daemon&&) = delete;
    Daemon& operator=(Daemon&&) = delete;
    ~Daemon();

    /// Listens and serves until `stop`; then ends every session. The exit status.
    int run(const std::atomic<bool>& stop);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// `farlandd --refuse-child`: in the network sandbox, runs the RDP
/// connection on descriptor `fd` until Set Error Info can go out, then ends
/// it with `error_info` ([MS-RDPBCGR] 2.2.5.1.1), so that the client shows
/// why it was refused. Returns the exit status.
[[nodiscard]] int run_refusal(int fd, std::uint32_t selected_protocol, std::uint32_t error_info);

}  // namespace farland::daemon
