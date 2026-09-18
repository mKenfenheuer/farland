// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/platform/mutter/mutter_session.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

namespace farland::platform::mutter {

struct HeadlessShellOptions {
    /// The compositor and its arguments; empty: gnome-shell --headless
    /// --no-x11 (no X11 applications; see launch()).
    std::vector<std::string> command;
    /// How long the private bus may take to come up.
    std::chrono::milliseconds timeout = std::chrono::seconds(10);
};

/// A headless GNOME Shell for the user this process runs as, on a D-Bus
/// session bus of its own (as `dbus-run-session -- gnome-shell --headless`
/// does), so it neither meets nor disturbs a desktop session the user may
/// already have: its own org.gnome.Mutter.* services, its own Wayland socket
/// in the user's $XDG_RUNTIME_DIR, the user's PipeWire daemon. It is a bare
/// shell without gnome-session (whose systemd units allow one session per
/// user manager): no settings daemons, no autostart applications. Full
/// sessions come from GDM (HeadlessOptions::attach).
///
/// The shell and the bus daemon run in process groups of their own; the
/// destructor ends both, and whatever the shell started in its group.
class HeadlessShell {
public:
    /// Starts the bus daemon and the shell; does not wait for the shell
    /// (MutterSession::create does, with keep_waiting = running()).
    [[nodiscard]] static MutterResult<std::unique_ptr<HeadlessShell>> launch(const HeadlessShellOptions& options);

    HeadlessShell(const HeadlessShell&) = delete;
    HeadlessShell& operator=(const HeadlessShell&) = delete;
    HeadlessShell(HeadlessShell&&) = delete;
    HeadlessShell& operator=(HeadlessShell&&) = delete;
    /// SIGTERM to the shell's process group, SIGKILL after ten seconds, then
    /// the bus daemon.
    ~HeadlessShell();

    /// The private session bus, for MutterOptions::bus_address.
    [[nodiscard]] const std::string& bus_address() const noexcept { return bus_address_; }
    [[nodiscard]] pid_t pid() const noexcept { return shell_pid_; }
    /// True while the shell runs; reaps it once it exited.
    [[nodiscard]] bool running();

private:
    HeadlessShell() = default;

    pid_t bus_pid_ = -1;
    /// Also the shell's process group.
    pid_t shell_pid_ = -1;
    /// running() reaped the shell.
    bool exited_ = false;
    std::string bus_address_;
};

/// The user's runtime directory: $XDG_RUNTIME_DIR, else /run/user/UID;
/// empty unless it is a directory of this user that only they can access.
[[nodiscard]] std::string user_runtime_dir();

}  // namespace farland::platform::mutter
