// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/server/connection.hpp>
#include <farland/server/frame_encoder.hpp>
#include <farland/server/graphics_pipeline.hpp>
#include <farland/server/preauth.hpp>
#include <farland/video/h264_encoder.hpp>

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
/// | Message        | Sender                                      | Descriptor           |
/// |----------------|---------------------------------------------|----------------------|
/// | Hello          | agent, as its first message                 | none                 |
/// | Settings       | daemon, right after the agent's Hello       | none                 |
/// | NewConnection  | daemon                                      | the plaintext socket |
/// | Disconnect     | daemon: end that connection with this code  | none                 |
/// |                | agent: that connection has ended            |                      |
/// | ConsentRequest | daemon: ask the user about a takeover       | none                 |
/// | ConsentCancel  | daemon: take that question back             | none                 |
/// | ConsentReply   | agent: what the user answered               | none                 |
/// | SeatTakeover   | daemon: how a login at the machine went     | none                 |
/// | SeatGreeter    | agent: put a login screen on the seat        | none                 |
/// | SessionEnded   | agent, as its last message                  | none                 |
/// | Stats          | agent, periodically                         | none                 |
/// | Terminate      | daemon: end the whole session               | none                 |
///
/// Wire format, as privsep: u32le length of what follows, a u8 message type,
/// then the fields. Integers are little-endian; strings are a u16le length
/// plus UTF-8; optional parts are a presence byte (0 or 1) and the part.
/// Decoding is strict: every presence byte, enum value, count and length is
/// checked against its limit, and a message must fill its frame exactly.
namespace farland::server::broker {

/// Bumped on every incompatible change; farlandd and the agent come from one
/// package, so the daemon simply refuses another version.
inline constexpr std::uint16_t protocol_version = 5;

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

/// Longest library name or device path in Settings.
inline constexpr std::size_t max_settings_path = 1024;
/// Frame rate range of Settings; the quality tiers lower it from there.
inline constexpr std::uint16_t min_frames_per_second = 1;
inline constexpr std::uint16_t max_frames_per_second = 240;
/// Activation timeout range of Settings, in seconds.
inline constexpr std::uint32_t min_activation_seconds = 5;
inline constexpr std::uint32_t max_activation_seconds = 3600;

/// Codecs, channels and timeouts for every connection of this session: the
/// part of /etc/farland/farland.toml the session needs. The agent runs as
/// the user and does not read /etc, so farlandd sends it once, right after
/// the agent's Hello and before the first NewConnection; the agent applies
/// it before it starts the desktop. The defaults are farland-server's, so
/// an agent that never hears it behaves as the command line does.
struct Settings {
    std::uint16_t frames_per_second = 30;
    /// What the H.264 quality ladder may spend, kbit/s. `h264_bitrate_kbps`
    /// of 0 keeps constant quality; the other two bound every tier.
    std::uint32_t h264_bitrate_kbps = 0;
    std::uint32_t h264_min_bitrate_kbps = 300;
    std::uint32_t h264_max_bitrate_kbps = 0;
    BitmapCodec bitmap_codec = BitmapCodec::planar;
    TileCodec gfx_codec = TileCodec::progressive;
    /// nullopt: try the backends this build has, GPU first.
    std::optional<video::Backend> h264_backend;
    /// OpenH264 library for AVC420 and AVC444 (empty: the usual sonames).
    std::string openh264_library;
    /// DRM render node for the compositor and for NVENC and VA-API (empty:
    /// the first that works).
    std::string render_node;
    bool zero_copy = true;
    bool clearcodec = true;
    bool refine = true;
    /// Progressive surfaces: H.264 for the tiles that keep changing, and a
    /// lossless copy of what stands still.
    bool video_regions = true;
    bool lossless_still = true;
    bool audio = true;
    bool microphone = true;
    /// The client's camera as a local camera (rdpecam).
    bool camera = true;
    bool clipboard = true;
    AutoDetectMode autodetect = AutoDetectMode::full;
    /// Seconds from the TCP connection to an active RDP connection, which
    /// includes starting the desktop.
    std::uint32_t activation_seconds = 60;
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

/// Range of ConsentRequest's timeout, in seconds; [policy] takeover_timeout
/// stays well inside it.
inline constexpr std::uint32_t min_consent_seconds = 1;
inline constexpr std::uint32_t max_consent_seconds = 3600;

/// Daemon to agent: connection `connection_id` wants this session, which
/// someone is holding — another client, or the user at the machine with
/// on_local_session = "attach". The agent asks them and answers with one
/// ConsentReply ([policy] takeover, apps/farland-agent/consent.hpp).
struct ConsentRequest {
    std::uint64_t connection_id = 0;
    std::string user;         ///< the connecting user, as NLA named them
    std::string peer;         ///< the client's address
    std::string client_name;  ///< what the client calls itself; empty: unknown
    std::uint32_t timeout_seconds = 30;
    /// What happens when nobody answers in time ([policy]
    /// takeover_on_timeout); the countdown runs in that button.
    bool allow_on_timeout = true;
    /// The session is wanted by somebody logging in at the machine, not by
    /// another client ([policy] seat_takeover): `user` is the account they
    /// log in as, `peer` and `client_name` are empty, and the prompt says so.
    bool from_seat = false;
};

/// Daemon to agent: the question about `connection_id` no longer stands —
/// whoever held the session let go, or the client is gone. The agent takes
/// the prompt down and sends no reply.
struct ConsentCancel {
    std::uint64_t connection_id = 0;
};

enum class ConsentAnswer : std::uint8_t {
    allowed = 1,      ///< hand the session over
    denied = 2,       ///< keep it
    timed_out = 3,    ///< nobody answered in time
    unavailable = 4,  ///< no notification service answered; nobody was asked
};

/// Agent to daemon: the answer to one ConsentRequest. farlandd applies
/// takeover_on_timeout to `timed_out` and `unavailable`.
struct ConsentReply {
    std::uint64_t connection_id = 0;
    ConsentAnswer answer = ConsentAnswer::allowed;
};

/// Daemon to agent: what became of a login at the machine ([policy]
/// seat_takeover), whether the session's user was asked or the policy
/// decided by itself. farlandd sends it before it lets the login go on, so
/// the agent knows what a seat coming back means: a refused login leaves the
/// display manager giving up the login screen it put on the seat, and the
/// seat falls back to this session for a moment — the agent puts a login
/// screen back and the client keeps the session.
struct SeatTakeover {
    bool allowed = true;
};

/// Agent to daemon: put a login screen on the seat, because the agent may
/// not do it itself. A greeter already on the seat belongs to the display
/// manager's uid, and logind refuses the agent -- running as the session's
/// own user -- the Activate that would switch to it; farlandd runs as root
/// and may. Nothing is created where a greeter is already there: farlandd
/// calls the same switch_seat_to_greeter(), which only creates one when the
/// seat has none. There is no reply; the agent carries on either way.
struct SeatGreeter {};

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

/// Daemon to agent: end the session (the connection, if any, then the
/// desktop) for `reason`; the agent answers with SessionEnded and exits.
/// farlandd sends it for [policy] disconnected_timeout, an administrator's
/// request and its own shutdown.
struct Terminate {
    EndReason reason = EndReason::terminated;
};

using Message = std::variant<Hello, Settings, NewConnection, Disconnect, ConsentRequest, ConsentCancel, ConsentReply,
                             SeatTakeover, SeatGreeter, SessionEnded, Stats, Terminate>;

enum class Sender : std::uint8_t { daemon, agent };

/// One line naming every setting, for the logs of both sides.
[[nodiscard]] std::string describe(const Settings& settings);

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
