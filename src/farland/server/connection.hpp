// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/text.hpp>
#include <farland/base/writer.hpp>
#include <farland/proto/bitmap.hpp>
#include <farland/proto/capabilities.hpp>
#include <farland/proto/framing.hpp>
#include <farland/proto/gcc.hpp>
#include <farland/proto/input.hpp>
#include <farland/proto/share.hpp>
#include <farland/proto/x224.hpp>

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

/// The server side of an RDP connection as a sans-IO state machine: bytes in,
/// bytes and events out. It knows nothing about sockets, TLS or threads
/// (docs/PLAN.md §3.2); the caller moves bytes between it, the TLS layer and
/// the network.
///
/// Contract for callers, after every `receive()` and every command:
///   1. send `take_output()`: raw before `StartTls`, through TLS after it;
///   2. then handle each event from `poll_event()`.
/// Draining output before events keeps the X.224 Connection Confirm ahead of
/// the TLS handshake.
namespace farland::server {

struct ServerConfig {
    /// Security protocols farland may select (M1: TLS only; NLA arrives in M2).
    std::uint32_t supported_protocols = proto::protocol::ssl;
    /// Client desktop sizes are clamped to this range.
    std::uint16_t min_desktop_size = 64;
    std::uint16_t max_desktop_size = 8192;
    /// Deepest color depth to negotiate (32, 24 or 16).
    std::uint16_t max_bits_per_pixel = 32;
};

enum class State : std::uint8_t {
    wait_connection_request,
    wait_tls,
    wait_connect_initial,
    wait_erect_domain,
    wait_attach_user,
    wait_channel_joins,
    wait_confirm_active,
    finalizing,
    active,
    closed,
};

[[nodiscard]] std::string_view to_string(State state) noexcept;

/// What client and server agreed on. Filled in step by step; complete when
/// the connection is active.
struct Session {
    std::string cookie;  ///< X.224 mstshash user name hint, if any
    std::uint32_t requested_protocols = 0;
    std::uint32_t selected_protocol = 0;
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
};

namespace event {
/// The X.224 Connection Confirm has been queued: flush output, then run the
/// TLS server handshake and call `tls_established()`.
struct StartTls {};
/// Client Info PDU. Carries the password for autologon (M2: CredSSP instead).
struct ClientInfo {
    std::string domain;
    std::string user_name;
    SecretString password;
    std::uint32_t flags = 0;
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

using Event = std::variant<event::StartTls, event::ClientInfo, event::Activated, event::Input, event::RefreshRequested,
                           event::OutputSuppressed, event::ChannelData, event::ShutdownRequested, event::Closed>;

class Connection {
public:
    explicit Connection(ServerConfig config = {});

    /// Bytes from the client: plaintext before TLS, decrypted TLS data after.
    void receive(std::span<const std::byte> bytes);
    /// The TLS handshake requested by `event::StartTls` has completed.
    void tls_established();

    [[nodiscard]] std::vector<std::byte> take_output();
    [[nodiscard]] std::optional<Event> poll_event();

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool active() const noexcept { return state_ == State::active; }
    [[nodiscard]] const Session& session() const noexcept { return session_; }

    /// Bytes of TS_UPDATE_BITMAP_DATA one update may carry, given the
    /// negotiated output path and the client's MultifragmentUpdate limit.
    [[nodiscard]] std::size_t max_update_size() const noexcept;
    /// Sends one TS_UPDATE_BITMAP_DATA (fast-path when negotiated). Active only.
    void send_bitmap_update(std::span<const std::byte> update_data);
    /// Deactivate All followed by a new Demand Active with another desktop size.
    void reactivate(std::uint16_t width, std::uint16_t height);
    /// Orderly server-side disconnect: Set Error Info (when the client supports
    /// it), then the MCS Disconnect Provider Ultimatum.
    void disconnect(std::uint32_t error_info = proto::errinfo::rpc_initiated_disconnect);

private:
    Result<void> handle_pdu(proto::FrameKind kind, std::span<const std::byte> pdu);
    Result<void> on_connection_request(Reader& tpdu);
    Result<void> on_connect_initial(Reader& data);
    Result<void> on_domain_pdu(Reader& data);
    Result<void> on_channel_join(std::uint16_t initiator, std::uint16_t channel_id);
    Result<void> on_client_info(Reader& data);
    Result<void> on_io_data(Reader& data);
    Result<void> on_confirm_active(Reader& body);
    Result<void> on_data_pdu(Reader& body);
    Result<void> on_fastpath_input(Reader& pdu);

    void send_demand_active();
    void send_io(std::span<const std::byte> payload);
    template <class Pdu>
    void send_data_pdu(const Pdu& pdu);
    void activate();
    void fail(std::string reason);
    void close(std::string reason);

    ServerConfig config_;
    State state_ = State::wait_connection_request;
    Session session_;
    std::vector<std::byte> input_;
    Writer output_;
    std::deque<Event> events_;
    bool reactivating_ = false;
    std::vector<std::uint16_t> joined_channels_;
};

}  // namespace farland::server
