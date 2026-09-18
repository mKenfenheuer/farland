// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/auth/tls_identity.hpp>
#include <farland/server/connection.hpp>
#include <farland/server/frame_encoder.hpp>
#include <farland/server/graphics_pipeline.hpp>
#include <farland/server/preauth.hpp>

#include "desktop.hpp"
#include "transport.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace farland::app {

/// How a caller that runs sessions on another thread (farland-agent) follows
/// and steers one (docs/ROADMAP.md M7). The session updates the counters and
/// calls the callbacks on its own thread; the caller reads the counters from
/// its own.
struct SessionControl {
    /// The Set Error Info code ([MS-RDPBCGR] 2.2.5.1.1) the session sends when
    /// `stop` ends it, e.g. ERRINFO_DISCONNECTED_BY_OTHERCONNECTION.
    std::atomic<std::uint32_t> stop_error_info{proto::errinfo::rpc_initiated_disconnect};
    /// The code the session sent when `stop` ended it; 0 when it ended
    /// otherwise (the client left).
    std::atomic<std::uint32_t> sent_error_info{0};
    /// When the client last sent input (keyboard, pointer, touch), in
    /// steady_clock ticks since its epoch; set at activation too; 0 before.
    std::atomic<std::int64_t> last_input{0};
    std::atomic<std::uint64_t> frames_sent{0};
    std::atomic<std::uint64_t> bytes_sent{0};
    std::atomic<std::uint64_t> bytes_received{0};
    std::atomic<std::uint32_t> desktop_width{0};
    std::atomic<std::uint32_t> desktop_height{0};
    std::atomic<std::uint32_t> rtt_ms{0};          ///< 0: unknown
    std::atomic<std::uint32_t> bandwidth_kbps{0};  ///< 0: unknown
    /// The Client Info PDU arrived, with the client's auto-reconnect cookie
    /// if it sent one.
    std::function<void(const std::optional<proto::AutoReconnectCookie>& cookie)> on_client_info;
    /// The connection became active (not on reactivation); returns what to
    /// send in a Save Session Info PDU, such as a fresh auto-reconnect cookie.
    std::function<std::optional<proto::LogonInfoExtended>()> on_activated;
};

struct SessionOptions {
    unsigned frames_per_second = 30;
    server::BitmapCodec codec = server::BitmapCodec::planar;
    /// Codec for the Graphics Pipeline, when the client runs it.
    server::TileCodec gfx_codec = server::TileCodec::progressive;
    /// OpenH264 library to load for AVC420 (empty: the usual names).
    std::string openh264_library;
    /// H.264 backend for AVC420; nullopt tries video::compiled_backends() in
    /// order (NVENC on NVIDIA, VA-API where a render node works, then OpenH264).
    std::optional<video::Backend> h264_backend;
    /// DRM render node for NVENC and VA-API (empty: the first that has an H.264 encoder).
    std::string render_node;
    /// Hand captured dmabufs to an AVC420 encoder that takes them, instead
    /// of reading every frame into CPU memory.
    bool zero_copy = true;
    /// Progressive surfaces: text and UI tiles through ClearCodec, and
    /// coarse first passes refined while the picture stands still.
    bool clearcodec = true;
    bool refine = true;
    /// Audio: the desktop's output plays on the client (rdpsnd), and the
    /// client's microphone becomes a local audio source (audin).
    bool audio = true;
    bool microphone = true;
    /// Seconds a client may take from TCP accept to an active connection.
    unsigned activation_timeout = 30;
    /// Network characteristics detection for clients that support it. It
    /// feeds the quality tiers; `continuous` skips the connect-time burst
    /// before licensing, `off` leaves the tiers to the GFX acknowledgements.
    server::AutoDetectMode autodetect = server::AutoDetectMode::full;
    /// Protocol policy for X.224 negotiation.
    server::PreAuthConfig preauth;
    /// Creates the CredSSP acceptor for NLA connections; without it only TLS is offered.
    server::PreAuth::NlaFactory make_nla;
    /// The shared desktop; null: the synthetic test pattern. Must outlive the
    /// session, and only one session may use it at a time.
    Desktop* desktop = nullptr;
    /// Share the desktop's clipboard (cliprdr), where the desktop has one.
    bool clipboard = true;
    /// Set by farland-agent to follow and steer the session; must outlive it.
    SessionControl* control = nullptr;
};

/// Runs one client connection on a connected socket until it ends or `stop`
/// becomes true, all on this thread: TLS and pre-authentication, the sans-IO
/// `server::Connection`, and the synthetic test backend behind it. Closes `fd`.
void run_session(int fd, std::string peer, const auth::TlsIdentity& identity, const SessionOptions& options,
                 const std::atomic<bool>& stop);

/// Runs a session over `transport` (which pre-authenticates first if it is
/// not ready yet). `started` is when the client connected, for the
/// activation timeout. `initial_input` is RDP stream the caller already read
/// from the transport (farland-agent reads the Connect Initial to size a new
/// desktop); the connection gets it before anything else. Closes the transport.
void run_session(Transport& transport, const std::string& peer, const SessionOptions& options,
                 const std::atomic<bool>& stop, std::chrono::steady_clock::time_point started,
                 std::span<const std::byte> initial_input = {});

}  // namespace farland::app
