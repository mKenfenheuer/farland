// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include "desktop.hpp"
#include "headless_options.hpp"

#include <memory>

namespace farland::app {

/// A headless Plasma desktop through KWin (docs/PLAN.md §3.5, ROADMAP M7),
/// without xdg-desktop-portal and its dialog.
///
/// Launched (options.attach false): KWin's virtual backend with one output
/// of options.width x options.height and a Plasma session in it
/// (plasma_session), on a private D-Bus bus, for the user this process runs
/// as; a local session of the same user is left alone. Destroying the
/// desktop ends KWin and the session. Attached (options.attach true): the
/// KWin of the session this process runs in ($WAYLAND_DISPLAY and the
/// session bus), which is never stopped.
///
/// The screens are KWin's outputs, streamed with zkde_screencast_unstable_v1
/// into PipeWire, and resizable through custom modes of
/// kde_output_management_v2. Input goes through KWin's EIS
/// (org.kde.KWin.EIS.RemoteDesktop), the clipboard through
/// ext-data-control-v1.
///
/// Attached, the screens are virtual outputs of this connection's own, one
/// per client monitor (stream_virtual_output_with_description, since version
/// 2 of the protocol), never the seat's screen: the client sees its own
/// desktop, and the screen at the machine shows the display manager's login
/// screen while a client holds the session (set_held()). The session is laid
/// out around the client's screens, with the first of them primary, so that
/// the panel and new windows are where the client looks; logging in at the
/// machine takes the session back and ends the connection. A KWin without
/// virtual outputs in its screen casting falls back to mirroring the seat's
/// screen, where the client sees what a person at the machine sees.
/// See docs/PLASMA-TAKEOVER.md.
///
/// KWin grants the screen casting only to an executable named in a desktop
/// file's X-KDE-Wayland-Interfaces: a launched KWin finds one this function
/// writes under $XDG_RUNTIME_DIR/farland; an attached KWin needs one
/// installed (data/org.farland.server.desktop.in). Linux only; built with
/// the KWin backend.
[[nodiscard]] Result<std::unique_ptr<Desktop>> start_plasma_headless(const HeadlessOptions& options);

}  // namespace farland::app
