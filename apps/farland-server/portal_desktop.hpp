// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include "desktop.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace farland::app {

struct PortalDesktopOptions {
    /// Share a new virtual monitor instead of the existing ones, where the
    /// portal offers that. It takes the size of the client's monitor where
    /// the compositor allows (Mutter; KWin's stays 1920x1080).
    bool virtual_monitor = false;
    /// Where the portal's restore token is kept so that later starts skip the
    /// dialog; nullopt: $XDG_STATE_HOME/farland/portal-restore-token (with
    /// virtual_monitor: portal-restore-token-virtual). A token the portal no
    /// longer honours is dropped and the dialog shown again.
    std::optional<std::filesystem::path> restore_token_file;
    /// How long the user may take to confirm the portal dialog.
    std::chrono::seconds timeout{300};
    /// DRM render node whose GPU imports the captured dmabufs: the H.264
    /// encoder's, so that the compositor hands out buffers in a layout it
    /// takes. Empty: the first render node that opens.
    std::string render_node;
    /// Ask the portal for clipboard access too (Desktop::clipboard()).
    bool clipboard = true;
};

/// Shares the running desktop through xdg-desktop-portal (docs/PLAN.md §3.3):
/// a RemoteDesktop + ScreenCast session, PipeWire capture of every stream (a
/// screen each: the monitors the user picked, or one virtual monitor that
/// takes the size of the client's monitor), input through libei (or the
/// portal's Notify* methods where the portal has no ConnectToEIS), and the
/// clipboard through the portal where it grants access. Blocks until the user
/// confirmed the portal dialog and every stream delivered its first frame.
/// Linux only; built with the portal backend.
[[nodiscard]] Result<std::unique_ptr<Desktop>> start_portal_desktop(const PortalDesktopOptions& options);

}  // namespace farland::app
