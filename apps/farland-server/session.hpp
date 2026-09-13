// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/auth/tls_identity.hpp>
#include <farland/server/frame_encoder.hpp>

#include <atomic>
#include <cstdint>
#include <string>

namespace farland::app {

struct SessionOptions {
    unsigned frames_per_second = 30;
    server::BitmapCodec codec = server::BitmapCodec::planar;
    /// Seconds a client may take from TCP accept to an active connection.
    unsigned activation_timeout = 30;
};

/// Runs one client connection on a connected socket until it ends or `stop`
/// becomes true: TLS over the socket, the sans-IO `server::Connection` in
/// between, and the synthetic test backend behind it. Closes `fd`.
void run_session(int fd, std::string peer, const auth::TlsIdentity& identity, const SessionOptions& options,
                 const std::atomic<bool>& stop);

}  // namespace farland::app
