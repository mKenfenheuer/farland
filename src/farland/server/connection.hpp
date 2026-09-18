// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/text.hpp>
#include <farland/base/writer.hpp>
#include <farland/proto/bitmap.hpp>
#include <farland/proto/capabilities.hpp>
#include <farland/proto/client_info.hpp>
#include <farland/proto/framing.hpp>
#include <farland/proto/gcc.hpp>
#include <farland/proto/input.hpp>
#include <farland/proto/pointer.hpp>
#include <farland/proto/save_session_info.hpp>
#include <farland/proto/share.hpp>
#include <farland/server/autodetect.hpp>
#include <farland/server/preauth.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

/// The server side of an RDP connection after pre-authentication, from the
/// MCS Connect Initial on, as a sans-IO state machine: bytes in, bytes and
/// events out. It knows nothing about sockets, TLS or threads (docs/PLAN.md
/// §3.2); the caller moves bytes between it, the TLS layer and the network.
///
/// Contract for callers: after every `receive()` and every command, send
/// `take_output()` through TLS, then handle each event from `poll_event()`.
/// The connection keeps no clock: `tick()` tells it the time.
namespace farland::server {

/// How much network characteristics detection ([MS-RDPBCGR] 1.3.9) to run
/// with clients that support it (RNS_UD_CS_SUPPORT_NETCHAR_AUTODETECT and a
/// joined message channel).
enum class AutoDetectMode : std::uint8_t {
    off,
    /// RTT probes and bandwidth measurements once the connection is active.
    continuous,
    /// Also the connect-time detection before licensing.
    full,
};

struct ServerConfig {
    /// Client desktop sizes are clamped to this range.
    std::uint16_t min_desktop_size = 64;
    std::uint16_t max_desktop_size = 8192;
    /// Deepest color depth to negotiate (32, 24 or 16).
    std::uint16_t max_bits_per_pixel = 32;
    /// The desktop size to announce instead of the client's request (a shared
    /// desktop has the size it has). Clamped like client sizes.
    std::optional<std::pair<std::uint16_t, std::uint16_t>> desktop_size;
    /// Chooses the desktop size from the client's data blocks (a session
    /// that lays out the client's monitors takes their bounding box). Takes
    /// precedence over `desktop_size`; clamped the same way.
    std::function<std::pair<std::uint16_t, std::uint16_t>(const proto::gcc::ClientData&)> choose_desktop_size;
    AutoDetectMode autodetect = AutoDetectMode::full;
    AutoDetect::Config autodetect_config;
    /// A Heartbeat PDU ([MS-RDPBCGR] 2.2.16.1) goes out after this long
    /// without other output, to clients that set RNS_UD_CS_SUPPORT_HEARTBEAT_PDU
    /// and joined the message channel. Zero: never.
    std::chrono::seconds heartbeat_period{5};
};

enum class State : std::uint8_t {
    wait_connect_initial,
    wait_erect_domain,
    wait_attach_user,
    wait_channel_joins,
    /// Connect-time auto-detect between Client Info and licensing ([MS-RDPBCGR] 1.3.1.1, phase 6).
    connect_time_autodetect,
    wait_confirm_active,
    finalizing,
    active,
    closed,
};

[[nodiscard]] std::string_view to_string(State state) noexcept;

/// What client and server agreed on. Filled in step by step; complete when
/// the connection is active.
struct Session {
    Negotiation negotiation;
    proto::gcc::ClientData client_data;
    std::uint16_t user_channel_id = 0;
    std::optional<std::uint16_t> message_channel_id;
    std::vector<std::pair<std::string, std::uint16_t>> static_channels;  ///< name, MCS channel ID

    std::string user_name;
    std::string domain;
    std::uint32_t info_flags = 0;

    std::uint32_t share_id = 0;
    std::uint16_t desktop_width = 0;
    std::uint16_t desktop_height = 0;
    std::uint16_t bits_per_pixel = 32;
    /// Client capabilities from the last Confirm Active; raw sets are dropped.
    proto::caps::CapabilitySets client_capabilities;

    bool fastpath_output = false;
    bool no_bitmap_compression_header = false;
    /// Largest reassembled fast-path update the client accepts; 0 if the
    /// client did not send a MultifragmentUpdate capability.
    std::uint32_t max_request_size = 0;
    bool supports_error_info = false;

    /// Pointer support both sides agreed on: the client's cache sizes, capped
    /// at what the server advertised, and the large pointer flags both set
    /// ([MS-RDPBCGR] 2.2.7.1.5, 2.2.7.2.7). CursorEncoder::Config::negotiated
    /// turns this into an encoder configuration.
    struct PointerSupport {
        std::uint16_t color_pointer_cache_size = 0;
        /// 0: no New or Large Pointer Updates (the client left pointerCacheSize out or set it to 0).
        std::uint16_t pointer_cache_size = 0;
        std::uint16_t large_pointer_flags = 0;  ///< proto::caps::large_pointer_flags
    };
    PointerSupport pointer;

    /// Auto-detect and heartbeats run on the message channel; the
    /// estimates are in Connection::network().
    bool autodetect = false;
    bool heartbeat = false;

    /// The MCS channel ID of the static channel `name` (compared without
    /// regard to case), if the client asked for it.
    [[nodiscard]] std::optional<std::uint16_t> static_channel_id(std::string_view name) const;
    /// The client can run the Graphics Pipeline ([MS-RDPEGFX] 1.3): it set
    /// RNS_UD_CS_SUPPORT_DYNVC_GFX_PROTOCOL, asked for the drdynvc channel
    /// and runs at 32 bpp (mstsc refuses GFX otherwise, docs/PLAN.md §4.1).
    [[nodiscard]] bool supports_gfx() const;
};

namespace event {
/// Client Info PDU. With NLA the password is usually empty; the credentials
/// came with `preauth_event::Authenticated`.
struct ClientInfo {
    std::string domain;
    std::string user_name;
    SecretString password;
    std::uint32_t flags = 0;
    /// ARC_CS_PRIVATE_PACKET from the extended info: the client returns to
    /// the session whose cookie it got ([MS-RDPBCGR] 5.5, auth/auto_reconnect.hpp).
    std::optional<proto::AutoReconnectCookie> auto_reconnect_cookie;
};
/// The connection is active (again, after `reactivate`); `session()` is complete.
struct Activated {
    bool reactivation = false;
};
struct Input {
    std::vector<proto::InputEvent> events;
};
/// Refresh Rect PDU: resend these areas.
struct RefreshRequested {
    std::vector<proto::Rectangle16> areas;
};
/// Suppress Output PDU: stop (or resume) sending graphics.
struct OutputSuppressed {
    bool suppressed = false;
    std::optional<proto::Rectangle16> area;
};
/// Data on a static virtual channel (reassembly is the channel's job).
struct ChannelData {
    std::uint16_t channel_id = 0;
    std::vector<std::byte> data;
};
/// The client asks to end the session; the caller decides (usually `disconnect()`).
struct ShutdownRequested {};
/// The connection is over. `error` distinguishes protocol failures from orderly ends.
struct Closed {
    std::string reason;
    bool error = false;
};
}  // namespace event

using Event = std::variant<event::ClientInfo, event::Activated, event::Input, event::RefreshRequested,
                           event::OutputSuppressed, event::ChannelData, event::ShutdownRequested, event::Closed>;

class Connection {
public:
    using Clock = std::chrono::steady_clock;

    Connection(ServerConfig config, Negotiation negotiation);

    /// Decrypted TLS data from the client.
    void receive(std::span<const std::byte> bytes);
    /// The current time. Call it before every receive() (auto-detect times
    /// the client's answers with it) and at least a few times a second:
    /// RTT probes, heartbeats and the connect-time detection's timeout go
    /// out from here.
    void tick(Clock::time_point now);

    /// Output since the last call. When a continuous bandwidth measurement is
    /// due and the output is large enough, it comes wrapped in a Bandwidth
    /// Measure Start and Stop ([MS-RDPBCGR] 2.2.14.1.2, 2.2.14.1.4).
    [[nodiscard]] std::vector<std::byte> take_output();
    [[nodiscard]] std::optional<Event> poll_event();

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool active() const noexcept { return state_ == State::active; }
    [[nodiscard]] const Session& session() const noexcept { return session_; }
    /// What auto-detect measured so far (empty without it).
    [[nodiscard]] const NetworkEstimate& network() const noexcept { return autodetect_.estimate(); }

    /// Bytes of TS_UPDATE_BITMAP_DATA one update may carry, given the
    /// negotiated output path and the client's MultifragmentUpdate limit.
    [[nodiscard]] std::size_t max_update_size() const noexcept;
    /// Sends one TS_UPDATE_BITMAP_DATA (fast-path when negotiated). Active only.
    void send_bitmap_update(std::span<const std::byte> update_data);
    /// Sends one pointer update ([MS-RDPBCGR] 2.2.9.1.1.4, 2.2.9.1.2.1.4 -
    /// 2.2.9.1.2.1.11): fast-path when negotiated, otherwise a slow-path
    /// Pointer Update PDU. Large pointers need fast-path output. Active only.
    void send_pointer(const proto::PointerUpdate& update);
    /// Sends one static virtual channel chunk (CHANNEL_PDU_HEADER and data,
    /// [MS-RDPBCGR] 2.2.6.1) on a channel the client asked for. Allowed from
    /// the capability exchange on; ignored once the connection is closed.
    void send_channel_data(std::uint16_t channel_id, std::span<const std::byte> chunk);
    /// Sends a Save Session Info PDU with the extended logon information
    /// ([MS-RDPBCGR] 2.2.10.1), such as a new auto-reconnect cookie (5.5).
    /// Active only.
    void send_save_session_info(const proto::LogonInfoExtended& info);
    /// Deactivate All followed by a new Demand Active with another desktop size.
    void reactivate(std::uint16_t width, std::uint16_t height);
    /// Orderly server-side disconnect: Set Error Info (when the client supports
    /// it), then the MCS Disconnect Provider Ultimatum.
    void disconnect(std::uint32_t error_info = proto::errinfo::rpc_initiated_disconnect);

private:
    Result<void> handle_pdu(proto::FrameKind kind, std::span<const std::byte> pdu);
    Result<void> on_connect_initial(Reader& data);
    Result<void> on_domain_pdu(Reader& data);
    Result<void> on_channel_join(std::uint16_t initiator, std::uint16_t channel_id);
    Result<void> on_client_info(Reader& data);
    Result<void> on_message_channel(Reader& data);
    Result<void> on_io_data(Reader& data);
    Result<void> on_confirm_active(Reader& body);
    Result<void> on_data_pdu(Reader& body);
    Result<void> on_fastpath_input(Reader& pdu);

    [[nodiscard]] bool message_channel_joined() const;
    /// Licensing and the Demand Active: the connection sequence after
    /// Client Info and the optional connect-time auto-detect.
    void start_licensing();
    void finish_connect_time_autodetect();
    void send_demand_active();
    void send_io(std::span<const std::byte> payload);
    /// One PDU on the message channel: basic security header with `flags`,
    /// then `payload` ([MS-RDPBCGR] 2.2.14.3, 2.2.16.1).
    void write_message_channel(Writer& out, std::uint16_t flags, std::span<const std::byte> payload) const;
    void send_autodetect(Writer& out, const proto::autodetect::Request& request) const;
    template <class Pdu>
    void send_data_pdu(const Pdu& pdu);
    void activate();
    void fail(std::string reason);
    void close(std::string reason);

    ServerConfig config_;
    State state_ = State::wait_connect_initial;
    Session session_;
    std::vector<std::byte> input_;
    Writer output_;
    std::deque<Event> events_;
    bool reactivating_ = false;
    std::vector<std::uint16_t> joined_channels_;
    AutoDetect autodetect_;
    Clock::time_point now_;
    Clock::time_point last_output_;
};

}  // namespace farland::server
