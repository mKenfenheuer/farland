// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace farland::app {

/// The compositor a headless session runs (docs/PLAN.md §3.5, ROADMAP M7).
enum class HeadlessKind { gnome, plasma, sway, labwc, cage };

/// How to start (or attach to) a headless desktop for the user this process
/// runs as. Each backend has a factory taking it and returning a Desktop
/// whose screens are virtual monitors, so resizable() is true:
///
///   start_gnome_headless()   (gnome_headless.hpp: Mutter's D-Bus API)
///   start_plasma_headless()  (plasma_headless.hpp: KWin)
///   start_wlroots_headless() (wlroots_headless.hpp: sway, labwc, cage)
///
/// The per-user agent keeps the Desktop between connections, so the session
/// survives a disconnect.
struct HeadlessOptions {
    HeadlessKind kind = HeadlessKind::gnome;
    /// The first virtual monitor's size; the disp channel resizes it later.
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    /// XKB layout for the compositor's keymap (from CS_CORE keyboardLayout);
    /// empty: the compositor's default.
    std::string keymap_layout;
    /// DRM render node for the compositor and the capture; empty: the
    /// compositor's choice.
    std::string render_node;
    /// Attach to the compositor already running in this user session (one
    /// GDM started, or a local session with on_local_session = attach)
    /// instead of launching one.
    bool attach = false;
    /// The application cage runs (cage only).
    std::vector<std::string> cage_command;
    /// How long to wait for the compositor and for the first frame.
    std::chrono::seconds timeout{30};
    /// Asks a privileged helper to put a login screen on the seat, for the
    /// case this process may not do it itself: a greeter is already on the
    /// seat and logind refuses this user the Activate that would switch to
    /// it. farland-agent points this at farlandd, which runs as root and
    /// calls the same switch_seat_to_greeter() -- so it still only creates a
    /// greeter where the seat has none. Unset (farland-server standalone):
    /// the backend makes do with what it can do itself. Called on the
    /// agent's main thread; there is no answer to wait for.
    std::function<void()> ask_for_greeter;
};

}  // namespace farland::app
