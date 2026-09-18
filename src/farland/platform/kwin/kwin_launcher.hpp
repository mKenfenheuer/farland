// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// Starting a headless Plasma session (docs/PLAN.md §3.5, ROADMAP M7).
///
/// KWin runs with its virtual backend (kwin_wayland_wrapper --virtual) and
/// starts plasma_session in it (--exit-with-session). Everything runs on a
/// private D-Bus session bus of its own, for three reasons:
/// - a local Plasma session of the same user already owns org.kde.KWin,
///   org.kde.plasmashell and the other unique names on the user's bus, so
///   plasmashell would refuse to start and connectToEIS would reach the
///   local KWin;
/// - kwin_wayland_wrapper and plasma_session push WAYLAND_DISPLAY and
///   DISPLAY into the bus's activation environment and systemd's; on the
///   user's bus that would redirect the local session's newly started
///   services into the headless one (the private bus has no systemd);
/// - plasma_session starts its own compositor unless org.kde.KWinWrapper is
///   on the bus, which the wrapper we start provides.
/// Plasma's systemd boot (startplasma-wayland with plasma-*.service units) is
/// not used: the user's systemd instance is shared with a local session,
/// whose units are already running.
///
/// KWin grants zkde_screencast_unstable_v1 only to executables named in a
/// desktop file's X-KDE-Wayland-Interfaces, found through XDG_DATA_DIRS:
/// the plan writes one for the client executable into a data directory it
/// appends to KWin's XDG_DATA_DIRS.
namespace farland::platform::kwin {

struct PlasmaLaunchOptions {
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    std::uint32_t output_count = 1;
    /// XKB layout for KWin's keymap: "de", "de(nodeadkeys)" or "de:nodeadkeys";
    /// empty keeps the user's keyboard configuration.
    std::string keymap_layout;
    /// A private directory for this launch: the bus socket and the logs.
    std::filesystem::path runtime_dir;
    /// Where the desktop file goes (in applications/); kept stable between
    /// launches because KDE caches its application database per
    /// XDG_DATA_DIRS.
    std::filesystem::path data_dir;
    /// The executable KWin grants screen casting to (this program).
    std::filesystem::path client_executable;
    /// What runs inside KWin; empty: KWin alone.
    std::string session_command = "plasma_session";
};

struct LaunchPlan {
    std::vector<std::string> dbus_argv;
    std::vector<std::string> kwin_argv;
    /// Changes to this process's environment for both: a value sets the
    /// variable, nullopt removes it.
    std::vector<std::pair<std::string, std::optional<std::string>>> environment;
    std::string bus_address;
    std::filesystem::path bus_socket;
    std::filesystem::path desktop_file;
    std::string desktop_file_contents;
};

/// Looks up a variable of the environment the plan starts from.
using GetEnv = std::function<std::optional<std::string>(const char* name)>;

/// The desktop file granting `executable` KWin's screen casting.
[[nodiscard]] std::string screencast_desktop_file(const std::filesystem::path& executable);
/// "de(nodeadkeys)" and "de:nodeadkeys" -> {"de", "nodeadkeys"}; "de" -> {"de", ""}.
[[nodiscard]] std::pair<std::string, std::string> split_xkb_layout(std::string_view layout);
[[nodiscard]] LaunchPlan plan_plasma_launch(const PlasmaLaunchOptions& options, const GetEnv& getenv);
/// The value of --socket (or --socket=) in a kwin_wayland command line.
[[nodiscard]] std::optional<std::string> socket_argument(std::span<const std::string> argv);

/// The processes of a launch: the private dbus-daemon and KWin (with the
/// session inside it), each in a process group of its own. Destroying it
/// ends both groups: SIGTERM, then SIGKILL after a grace period.
class PlasmaProcesses {
public:
    /// Writes the desktop file, starts the bus and KWin, and waits until
    /// KWin's Wayland socket is known. KWin's output goes to kwin.log in
    /// the runtime directory.
    [[nodiscard]] static Result<std::unique_ptr<PlasmaProcesses>> start(const LaunchPlan& plan,
                                                                        std::chrono::milliseconds timeout);

    PlasmaProcesses(const PlasmaProcesses&) = delete;
    PlasmaProcesses& operator=(const PlasmaProcesses&) = delete;
    PlasmaProcesses(PlasmaProcesses&&) = delete;
    PlasmaProcesses& operator=(PlasmaProcesses&&) = delete;
    ~PlasmaProcesses();

    /// KWin's socket name in $XDG_RUNTIME_DIR.
    [[nodiscard]] const std::string& wayland_display() const noexcept { return wayland_display_; }
    [[nodiscard]] const std::string& bus_address() const noexcept { return bus_address_; }
    /// True once KWin's wrapper exited (the session ended).
    [[nodiscard]] bool exited();
    /// Logs the end of KWin's output as an error (when something failed).
    void log_output_tail() const;

private:
    PlasmaProcesses() = default;
    static void stop(int& pid, int group, std::chrono::milliseconds grace);

    int dbus_pid_ = -1;
    int kwin_pid_ = -1;
    int dbus_group_ = -1;
    int kwin_group_ = -1;
    std::string wayland_display_;
    std::string bus_address_;
    std::filesystem::path runtime_dir_;
};

}  // namespace farland::platform::kwin
