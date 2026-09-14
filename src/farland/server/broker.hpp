// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/server/preauth.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

/// The broker protocol between farlandd, the system daemon, and
/// farland-agent, the per-user process that owns one desktop session
/// (docs/ROADMAP.md M7).
///
/// farlandd accepts every client on the one port. Its network process runs
/// X.224, TLS and NLA in the sandbox as before (privsep.hpp) and relays the
/// decrypted RDP stream over a plaintext socket. farlandd picks the session
/// by the NLA identity and hands that plaintext socket to the session's
/// agent with `NewConnection`; the descriptor travels next to the message as
/// SCM_RIGHTS (apps/farland-server/unix_socket.hpp). The agent continues the
/// connection from the MCS Connect Initial on, as farland-server does after
/// privsep: `server::Connection` over a plaintext transport. TLS keys and the
/// credential store never reach the agent.
///
/// Who sends what:
///
/// | Message       | Sender                                      | Descriptor           |
/// |---------------|---------------------------------------------|----------------------|
/// | Hello         | agent, as its first message                 | none                 |
/// | NewConnection | daemon                                      | the plaintext socket |
/// | Disconnect    | daemon: end that connection with this code  | none                 |
/// |               | agent: that connection has ended            |                      |
/// | SessionEnded  | agent, as its last message                  | none                 |
/// | Stats         | agent, periodically                         | none                 |
///
/// Wire format, as privsep: u32le length of what follows, a u8 message type,
/// then the fields. Integers are little-endian; strings are a u16le length
/// plus UTF-8; optional parts are a presence byte (0 or 1) and the part.
/// Decoding is strict: every presence byte, enum value, count and length is
/// checked against its limit, and a message must fill its frame exactly.
namespace farland::server::broker {

/// Bumped on every incompatible change; farlandd and the agent come from one
/// package, so the daemon simply refuses another version.
inline constexpr std::uint16_t protocol_version = 1;

/// A per-agent secret farlandd generates when it starts the agent. Together
/// with the peer's uid (SO_PEERCRED) it ties the socket connection to the
/// session farlandd created.
using Token = std::array<std::byte, 32>;

/// Largest message either side accepts (length prefix excluded).
inline constexpr std::size_t max_message_size = std::size_t{64} * 1024;
/// Client bytes read off the plaintext socket before the handover.
inline constexpr std::size_t max_pending_input = std::size_t{32} * 1024;
inline constexpr std::size_t max_monitors = 16;  ///< [MS-RDPBCGR] 2.2.1.3.6

/// The agent introduces itself.
struct Hello {
    std::uint16_t version = protocol_version;
    Token token{};
};

/// One monitor, as TS_MONITOR_DEF ([MS-RDPBCGR] 2.2.1.3.6.1): inclusive
/// virtual desktop coordinates.
struct Monitor {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    bool primary = false;
};

/// What the session needs from the client's GCC Conference Create Request
/// (TS_UD_CS_CORE and TS_UD_CS_MONITOR, [MS-RDPBCGR] 2.2.1.3.2 and 2.2.1.3.6)
/// before the connection runs: a new headless desktop starts at this size,
/// with these monitors and this keyboard layout.
struct ClientSummary {
    std::uint16_t desktop_width = 0;
    std::uint16_t desktop_height = 0;
    std::vector<Monitor> monitors;  ///< empty: one monitor of the desktop size
    std::uint32_t keyboard_layout = 0;
    std::uint32_t keyboard_type = 0;
    std::uint32_t keyboard_subtype = 0;
    std::uint32_t client_build = 0;
    std::string client_name;
    std::optional<std::uint32_t> desktop_scale_factor;  ///< percent
};

/// ARC_CS_PRIVATE_PACKET fields ([MS-RDPBCGR] 2.2.4.3).
struct ReconnectCookie {
    std::uint32_t logon_id = 0;
    std::array<std::byte, 16> security_verifier{};
};

/// A client for this session. The plaintext socket comes with it.
struct NewConnection {
    std::uint64_t connection_id = 0;  ///< farlandd's number, never 0
    /// X.224 result and the NLA identity; the agent builds its
    /// `server::Connection` from it.
    Negotiation negotiation;
    std::string peer;  ///< the client's address, for logs
    /// Milliseconds since farlandd accepted the TCP connection; the agent's
    /// activation timeout counts from then.
    std::uint32_t elapsed_ms = 0;
    /// Filled when farlandd has seen the Connect Initial.
    std::optional<ClientSummary> client;
    /// Filled when whoever handed over already read the Client Info PDU.
    /// With a handover right after NLA (S1) it is empty, and the agent reads
    /// the cookie from the Client Info PDU itself.
    std::optional<ReconnectCookie> auto_reconnect;
    /// Client bytes already read from the socket (for example the Connect
    /// Initial, to fill `client`). The agent feeds them to its Connection
    /// before anything it reads from the socket.
    std::vector<std::byte> pending_input;
};

/// Daemon to agent: end connection `connection_id` with Set Error Info
/// `error_info` ([MS-RDPBCGR] 2.2.5.1.1). Agent to daemon: connection
/// `connection_id` has ended; `error_info` is what the agent sent, or 0 when
/// the client left.
struct Disconnect {
    std::uint64_t connection_id = 0;
    std::uint32_t error_info = 0;
};

enum class EndReason : std::uint8_t {
    logout = 1,                ///< the user logged out; the compositor exited normally
    desktop_failed = 2,        ///< the compositor or the capture backend died
    idle_timeout = 3,          ///< [policy] idle_timeout
    disconnected_timeout = 4,  ///< [policy] disconnected_timeout
    terminated = 5,            ///< an administrator ended the session
    error = 6,                 ///< anything else; see `detail`
};

/// The desktop session is over; the agent exits after sending this.
struct SessionEnded {
    EndReason reason = EndReason::logout;
    std::string detail;
};

/// Counters for policies, farlandctl and metrics. Byte and frame counts
/// accumulate over the current connection.
struct Stats {
    std::uint64_t connection_id = 0;    ///< 0: no client connected
    std::uint32_t session_seconds = 0;  ///< since the desktop started
    std::uint32_t idle_seconds = 0;     ///< since the last client input
    std::uint16_t desktop_width = 0;
    std::uint16_t desktop_height = 0;
    std::uint64_t frames_sent = 0;
    std::uint64_t bytes_sent = 0;
    std::uint64_t bytes_received = 0;
    std::uint32_t rtt_ms = 0;          ///< 0: unknown
    std::uint32_t bandwidth_kbps = 0;  ///< 0: unknown
};

using Message = std::variant<Hello, NewConnection, Disconnect, SessionEnded, Stats>;

enum class Sender : std::uint8_t { daemon, agent };

/// Whether `sender` may send `message` at all.
[[nodiscard]] bool may_send(Sender sender, const Message& message) noexcept;
/// Whether `message` travels with a descriptor (only NewConnection does).
[[nodiscard]] bool carries_fd(const Message& message) noexcept;

/// Asserts the limits above; our own messages always satisfy them.
[[nodiscard]] std::vector<std::byte> encode(const Message& message);
/// Length of the first complete message in `buffered` (prefix included), or
/// nullopt until it has fully arrived. Errors for an oversized length.
[[nodiscard]] Result<std::optional<std::size_t>> message_length(std::span<const std::byte> buffered);
/// Decodes exactly one framed message.
[[nodiscard]] Result<Message> decode(std::span<const std::byte> frame);
/// Decodes a message from `sender` and checks that it may send it and that a
/// descriptor came with it exactly when it must (`with_fd`).
[[nodiscard]] Result<Message> decode_from(Sender sender, std::span<const std::byte> frame, bool with_fd);

/// farlandd's side of one agent connection. The first message must be a
/// Hello with this protocol version and the token farlandd gave the agent;
/// after that only what agents send, and nothing after SessionEnded. An error
/// means farlandd drops the agent's connection.
class AgentLink {
public:
    explicit AgentLink(const Token& expected) : expected_(expected) {}
    AgentLink(const AgentLink&) = delete;
    AgentLink& operator=(const AgentLink&) = delete;
    AgentLink(AgentLink&&) = delete;
    AgentLink& operator=(AgentLink&&) = delete;
    ~AgentLink();

    [[nodiscard]] Result<Message> receive(std::span<const std::byte> frame, bool with_fd);
    [[nodiscard]] bool greeted() const noexcept { return greeted_; }
    [[nodiscard]] bool ended() const noexcept { return ended_; }

private:
    Token expected_;
    bool greeted_ = false;
    bool ended_ = false;
};

}  // namespace farland::server::broker
