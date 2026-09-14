// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include "desktop.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>

namespace farland::app {

struct PortalDesktopOptions {
    /// Share a new virtual monitor instead of an existing one, where the
    /// portal offers that.
    bool virtual_monitor = false;
    /// Where the portal's restore token is kept so that later starts skip the
    /// dialog; nullopt: $XDG_STATE_HOME/farland/portal-restore-token (with
    /// virtual_monitor: portal-restore-token-virtual). A token the portal no
    /// longer honours is dropped and the dialog shown again.
    std::optional<std::filesystem::path> restore_token_file;
    /// How long the user may take to confirm the portal dialog.
    std::chrono::seconds timeout{300};
};

/// Shares the running desktop through xdg-desktop-portal (docs/PLAN.md §3.3):
/// a RemoteDesktop + ScreenCast session, PipeWire capture of its first stream,
/// and input through libei (or the portal's Notify* methods where the portal
/// has no ConnectToEIS). Blocks until the user confirmed the portal dialog and
/// the first frame arrived. Linux only; built with the portal backend.
[[nodiscard]] Result<std::unique_ptr<Desktop>> start_portal_desktop(const PortalDesktopOptions& options);

}  // namespace farland::app
