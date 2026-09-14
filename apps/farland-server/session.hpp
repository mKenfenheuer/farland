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
#include <optional>
#include <string>

namespace farland::app {

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
};

/// Runs one client connection on a connected socket until it ends or `stop`
/// becomes true, all on this thread: TLS and pre-authentication, the sans-IO
/// `server::Connection`, and the synthetic test backend behind it. Closes `fd`.
void run_session(int fd, std::string peer, const auth::TlsIdentity& identity, const SessionOptions& options,
                 const std::atomic<bool>& stop);

/// Runs a session over `transport` (which pre-authenticates first if it is
/// not ready yet). `started` is when the client connected, for the
/// activation timeout. Closes the transport.
void run_session(Transport& transport, const std::string& peer, const SessionOptions& options,
                 const std::atomic<bool>& stop, std::chrono::steady_clock::time_point started);

}  // namespace farland::app
