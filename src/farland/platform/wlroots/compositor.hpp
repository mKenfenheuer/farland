// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace farland::platform::wlroots {

enum class CompositorKind : std::uint8_t { sway, labwc, cage };

[[nodiscard]] std::string_view to_string(CompositorKind kind) noexcept;

struct CompositorLaunch {
    CompositorKind kind = CompositorKind::sway;
    /// cage's application and its arguments (required for cage).
    std::vector<std::string> command;
    /// Headless outputs to create.
    std::uint32_t outputs = 1;
    /// DRM render node to render on; empty: the pixman software renderer.
    std::string render_node;
    /// How long the compositor may take to open its socket.
    std::chrono::milliseconds timeout{30'000};
};

/// The command line that starts the compositor.
[[nodiscard]] std::vector<std::string> compositor_command(const CompositorLaunch& launch);

/// The compositor's environment: `environment` (NAME=value entries) with the
/// headless backend (WLR_BACKENDS=headless, WLR_LIBINPUT_NO_DEVICES=1,
/// WLR_HEADLESS_OUTPUTS), the renderer (WLR_RENDERER=pixman, or
/// WLR_RENDER_DRM_DEVICE) and `runtime_dir` as XDG_RUNTIME_DIR, and without
/// WAYLAND_DISPLAY, WAYLAND_SOCKET and DISPLAY, which would name another
/// display.
[[nodiscard]] std::vector<std::string> compositor_environment(const CompositorLaunch& launch,
                                                              std::span<const std::string> environment,
                                                              const std::string& runtime_dir);

/// The Wayland socket among a process's sockets: the path of a listening
/// socket in /proc/net/unix (`proc_net_unix`, its text) whose inode is one of
/// `inodes` and whose name is wayland-N in `runtime_dir`.
[[nodiscard]] std::optional<std::string> find_wayland_socket(std::string_view proc_net_unix,
                                                             std::span<const std::uint64_t> inodes,
                                                             std::string_view runtime_dir);

/// A headless wlroots compositor (sway, labwc or cage) that farland started
/// for the user it runs as (docs/ROADMAP.md M7). It runs in a process group
/// of its own, in $XDG_RUNTIME_DIR (a private 0700 directory when that is
/// unset, removed at the end), with its output in a log file there. Its
/// Wayland socket is the one it listens on (found through /proc, so several
/// compositors can start at once).
///
/// Destroying it stops the compositor and everything in its process group:
/// SIGTERM, then SIGKILL after five seconds.
class CompositorProcess {
public:
    /// Starts the compositor and waits for its socket.
    [[nodiscard]] static Result<std::unique_ptr<CompositorProcess>> launch(const CompositorLaunch& launch);
    CompositorProcess(const CompositorProcess&) = delete;
    CompositorProcess& operator=(const CompositorProcess&) = delete;
    CompositorProcess(CompositorProcess&&) = delete;
    CompositorProcess& operator=(CompositorProcess&&) = delete;
    ~CompositorProcess();

    [[nodiscard]] const std::string& socket_path() const noexcept { return socket_; }
    [[nodiscard]] const std::filesystem::path& log_path() const noexcept { return log_; }
    [[nodiscard]] pid_t pid() const noexcept { return pid_; }
    /// True once the compositor exited (checked without blocking).
    [[nodiscard]] bool exited();
    /// The last lines of the compositor's log, for error messages.
    [[nodiscard]] std::string log_tail(std::size_t lines = 10) const;

private:
    CompositorProcess(pid_t pid, std::filesystem::path log, std::filesystem::path private_runtime_dir);

    pid_t pid_;
    bool exited_ = false;
    std::string socket_;
    std::filesystem::path log_;
    /// The runtime directory farland made; empty when it used the user's.
    std::filesystem::path private_runtime_dir_;
};

}  // namespace farland::platform::wlroots
