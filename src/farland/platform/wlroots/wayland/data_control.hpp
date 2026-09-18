// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/platform/clipboard.hpp>

#include <chrono>
#include <cstddef>
#include <memory>

struct wl_seat;

namespace farland::platform::wayland {

class Connection;

struct DataControlOptions {
    /// Local data larger than this fails a read.
    std::size_t max_read_size = std::size_t{64} * 1024 * 1024;
    /// How long the desktop application may take to hand over its data,
    /// and to take ours.
    std::chrono::milliseconds read_timeout{15'000};
    std::chrono::milliseconds write_timeout{15'000};
};

/// The seat's clipboard through a data-control protocol, which lets a client
/// without a surface own and read the selection: ext-data-control-v1 where
/// the compositor has it, else zwlr-data-control-unstable-v1 (wlroots
/// before 0.19). The primary selection is left alone.
///
/// The desktop's selection arrives as an offer whose types become
/// OwnerChanged; read() receives one of them through a pipe. set_selection()
/// offers the client's types from a data source, and each paste by a desktop
/// application becomes TransferRequested with the pipe to write to. Pipes
/// are served without blocking in dispatch(); the Wayland events come in
/// through the connection's own dispatch.
///
/// Returns null when the compositor has neither protocol (cage). The
/// clipboard must not outlive the connection; the process must ignore
/// SIGPIPE.
[[nodiscard]] std::unique_ptr<Clipboard> create_data_control_clipboard(Connection& connection, wl_seat* seat,
                                                                       const DataControlOptions& options = {});

}  // namespace farland::platform::wayland
