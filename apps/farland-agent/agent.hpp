// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/auth/auto_reconnect.hpp>
#include <farland/base/unique_fd.hpp>
#include <farland/proto/gcc.hpp>
#include <farland/server/broker.hpp>

#include "desktop.hpp"
#include "prompt.hpp"
#include "session.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

/// farland-agent: the per-user process of one multi-session desktop
/// (docs/ROADMAP.md M7). farlandd hands it every connection of its user as a
/// plaintext socket whose TLS and NLA farlandd's network process already
/// did (broker protocol, server/broker.hpp). The agent runs the connection
/// from the MCS Connect Initial on with the ordinary session loop
/// (app::run_session over a PlainTransport) and keeps the desktop between
/// connections, so a returning client finds its session as it left it.
namespace farland::agent {

/// What the agent learns from the client's MCS Connect Initial before the
/// desktop exists: the size to start it at.
[[nodiscard]] server::broker::ClientSummary summarize(const proto::gcc::ClientData& data);
/// The size of the client's primary monitor (else its desktop), clamped to
/// 64..8192; 1024 x 768 without either.
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> initial_size(const server::broker::ClientSummary& client);

/// The first RDP PDU read off a fresh plaintext socket, which is the
/// Connect Initial, and its summary if it parses. Everything read is in
/// `bytes`, for the session to process as usual.
struct ConnectInitialPeek {
    std::vector<std::byte> bytes;
    std::optional<server::broker::ClientSummary> client;
};
[[nodiscard]] ConnectInitialPeek read_connect_initial(int fd, int timeout_ms);

/// What the session's desktop starts with: the first client's size, and the
/// part of the configuration the desktop itself needs.
struct DesktopRequest {
    std::uint32_t width = 1024;
    std::uint32_t height = 768;
    /// [graphics] render_node; empty: the compositor's choice.
    std::string render_node;
    /// [graphics] frames_per_second, for the test pattern's animation.
    unsigned frames_per_second = 30;
    /// Passed to HeadlessOptions::ask_for_greeter: asks farlandd, which runs
    /// as root, to put a login screen on the seat where the agent may not.
    std::function<void()> ask_for_greeter;
    /// Passed to HeadlessOptions::still_waiting: the backend calls it while
    /// it waits for the compositor, so that whoever is watching the agent
    /// keeps hearing from it.
    std::function<void()> still_waiting;
};

/// Starts the session's desktop for `request`.
using DesktopFactory = std::function<Result<std::unique_ptr<app::Desktop>>(const DesktopRequest& request)>;

struct AgentConfig {
    /// Connected to farlandd's agent socket.
    UniqueFd daemon;
    /// That socket's path, so that the agent can find the next farlandd
    /// after this one goes away (a restart, which every package upgrade
    /// does). Empty: the session ends with farlandd, as it used to.
    std::string socket_path;
    server::broker::Token token{};
    /// The session's number at farlandd; the logon id of its auto-reconnect
    /// cookies ([MS-RDPBCGR] 2.2.4.2).
    std::uint32_t logon_id = 1;
    DesktopFactory make_desktop;
    /// Codecs and channels for every connection; `desktop` and `control`
    /// are the agent's. farlandd's Settings message, which arrives before
    /// the first connection, replaces these with /etc/farland/farland.toml.
    app::SessionOptions session;
    std::chrono::milliseconds stats_period{5000};
    /// How long to keep the desktop and its client while looking for
    /// farlandd again. farlandd waits as long for the agent, so whichever
    /// gives up first ends the session cleanly.
    std::chrono::seconds reattach_timeout{120};
    /// Asks the person using the session whether a new connection may take
    /// it over ([policy] takeover); unset: a desktop notification.
    ConsentAsker ask_consent;
};

class Agent {
public:
    explicit Agent(AgentConfig config);
    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;
    Agent(Agent&&) = delete;
    Agent& operator=(Agent&&) = delete;
    ~Agent();

    /// Greets farlandd and serves it until the session ends: the desktop
    /// closed (logout), farlandd said Terminate or went away, or `stop`
    /// became true. Sends SessionEnded when it can and returns the reason.
    server::broker::EndReason run(const std::atomic<bool>& stop);

    /// Returning clients whose auto-reconnect cookie matched, and did not.
    [[nodiscard]] unsigned cookies_matched() const noexcept { return cookies_matched_.load(); }
    [[nodiscard]] unsigned cookies_mismatched() const noexcept { return cookies_mismatched_.load(); }

private:
    struct Connection;
    struct Prompt;

    [[nodiscard]] bool send(const server::broker::Message& message);
    /// Takes over farlandd's configuration, before the desktop starts.
    void apply(const server::broker::Settings& settings);
    /// Starts the session thread for a new connection, after ending the
    /// current one (a takeover). False when the desktop cannot start.
    [[nodiscard]] bool on_new_connection(server::broker::NewConnection message, UniqueFd fd);
    void stop_connection(std::uint32_t error_info);
    /// Puts farlandd's question to the user, on a thread of its own so that
    /// nothing else in the session waits for the answer.
    void start_prompt(const server::broker::ConsentRequest& request);
    /// Takes the question down and answers nothing (farlandd withdrew it,
    /// or the session is ending).
    void withdraw_prompt();
    /// Sends farlandd what the user answered, once the prompt is done.
    void finish_prompt();
    /// Joins a finished (or stopping) connection and reports it to farlandd.
    void join_connection();
    void send_stats();
    /// farlandd went away. Keeps the desktop and whatever client is
    /// connected, and looks for the socket to come back until
    /// `reattach_timeout`; true when a new farlandd took the greeting.
    /// False ends the session, as losing farlandd always used to.
    [[nodiscard]] bool reattach(const std::atomic<bool>& stop);
    server::broker::EndReason finish(server::broker::EndReason reason, std::string detail);

    AgentConfig config_;
    auth::arc::Secret arc_;
    std::unique_ptr<app::Desktop> desktop_;
    /// This session has had a desktop at least once. A GNOME session on a
    /// seat gives its desktop back whenever no client holds it, so every
    /// later connection builds one again -- and one that cannot (the screen
    /// at the machine is locked, say) must take only itself down, never the
    /// session and the windows in it. Only the connection that was to start
    /// the session in the first place is worth ending it for.
    bool had_desktop_ = false;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::unique_ptr<Connection> current_;
    std::unique_ptr<Prompt> prompt_;
    /// The session thread writes a byte when it ends.
    UniqueFd wake_read_;
    UniqueFd wake_write_;
    std::atomic<unsigned> cookies_matched_{0};
    std::atomic<unsigned> cookies_mismatched_{0};
    bool daemon_gone_ = false;
};

}  // namespace farland::agent
