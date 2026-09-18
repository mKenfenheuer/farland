// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include "desktop.hpp"
#include "headless_options.hpp"

#include <memory>

namespace farland::app {

/// A headless GNOME desktop through Mutter's own D-Bus API (docs/PLAN.md
/// §3.3, ROADMAP M7), as gnome-remote-desktop drives it: no portal, no
/// permission dialog. Each screen is a virtual monitor of Mutter's
/// (ScreenCast RecordVirtual, is-platform), created at the client monitor's
/// size, resized through PipeWire format negotiation when the client's
/// window changes, and added or removed as the client's monitors come and
/// go (Desktop::screens_follow_monitors()). Input goes through libei
/// (ConnectToEIS), the clipboard through Mutter's RemoteDesktop clipboard,
/// and `options.keymap_layout` becomes the desktop's keymap (SetKeymap, where
/// farland has libxkbcommon).
///
/// With `options.attach`, it uses the GNOME session this process runs in
/// (its DBUS_SESSION_BUS_ADDRESS; a session GDM started, say) and leaves the
/// compositor running when the Desktop goes. Otherwise it launches a bare
/// headless GNOME Shell on a private session bus (platform::mutter::
/// HeadlessShell) and stops it with the Desktop.
///
/// Blocks until the first virtual monitor delivered its first frame, up to
/// `options.timeout` for each step. Linux only; built with the Mutter
/// backend (FARLAND_HAVE_MUTTER).
[[nodiscard]] Result<std::unique_ptr<Desktop>> start_gnome_headless(const HeadlessOptions& options);

}  // namespace farland::app
