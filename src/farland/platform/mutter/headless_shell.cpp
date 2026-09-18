// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/mutter/headless_shell.hpp>
#include <farland/platform/portal/portal_bus.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <poll.h>
#include <spawn.h>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

// POSIX declares it nowhere else; the children inherit a copy of it.
extern char** environ;  // NOLINT(readability-redundant-declaration,cppcoreguidelines-avoid-non-const-global-variables)

namespace farland::platform::mutter {

namespace {

using Clock = std::chrono::steady_clock;
using portal::detail::fail;

constexpr std::string_view log_component = "platform.mutter";
/// How long the shell may take to exit after SIGTERM.
constexpr auto stop_timeout = std::chrono::seconds(10);
/// The descriptor on which dbus-daemon prints its address.
constexpr int address_fd = 3;

/// This process's environment for the bus daemon and the shell: without
/// what belongs to another session (its bus, display and systemd socket),
/// with the runtime directory, a Wayland session type and, for the shell,
/// the private bus.
std::vector<std::string> child_environment(const std::string& runtime_dir, const std::string& bus_address)
{
    static constexpr std::array dropped{
        "DBUS_SESSION_BUS_ADDRESS=",
        "DBUS_STARTER_ADDRESS=",
        "DBUS_STARTER_BUS_TYPE=",
        "WAYLAND_DISPLAY=",
        "WAYLAND_SOCKET=",
        "DISPLAY=",
        "XAUTHORITY=",
        "XDG_SESSION_TYPE=",
        "XDG_RUNTIME_DIR=",
        "NOTIFY_SOCKET=",
        "GNOME_SETUP_DISPLAY=",
        "LISTEN_FDS=",
        "LISTEN_PID=",
        "LISTEN_FDNAMES=",
    };
    std::vector<std::string> env;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): environ is a null-terminated C array
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string_view text = *entry;
        if (std::ranges::none_of(dropped, [&](std::string_view name) { return text.starts_with(name); })) {
            env.emplace_back(text);
        }
    }
    env.push_back("XDG_RUNTIME_DIR=" + runtime_dir);
    env.emplace_back("XDG_SESSION_TYPE=wayland");
    if (!bus_address.empty()) {
        env.push_back("DBUS_SESSION_BUS_ADDRESS=" + bus_address);
    }
    return env;
}

std::vector<char*> pointers(std::vector<std::string>& strings)
{
    std::vector<char*> result;
    result.reserve(strings.size() + 1);
    for (auto& s : strings) {
        result.push_back(s.data());
    }
    result.push_back(nullptr);
    return result;
}

/// Spawns `argv` (searched in PATH) in a process group of its own; with
/// `print_fd` >= 0, that descriptor becomes the child's descriptor 3.
MutterResult<pid_t> spawn(std::vector<std::string> argv, std::vector<std::string> env, int print_fd)
{
    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    if (print_fd >= 0) {
        posix_spawn_file_actions_adddup2(&actions, print_fd, address_fd);
    }
    posix_spawnattr_t attributes{};
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    posix_spawnattr_setpgroup(&attributes, 0);
    sigset_t none{};
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attributes, &none);
    sigset_t defaults{};
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);  // farland ignores it; the children should not inherit that
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGTERM);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    auto args = pointers(argv);
    auto envp = pointers(env);
    pid_t pid = -1;
    const int error = ::posix_spawnp(&pid, args.front(), &actions, &attributes, args.data(), envp.data());
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (error != 0) {
        return fail(PortalErrc::unavailable, std::format("cannot start {}: {}", argv.front(), std::strerror(error)));
    }
    return pid;
}

/// One line from `fd`, waiting up to `timeout`; empty on EOF or timeout.
std::string read_line(int fd, std::chrono::milliseconds timeout)
{
    std::string line;
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) {
            return {};
        }
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        if (::poll(&pfd, 1, static_cast<int>(left)) <= 0) {
            continue;
        }
        char c = 0;
        const auto n = ::read(fd, &c, 1);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n != 1) {
            return {};
        }
        if (c == '\n') {
            return line;
        }
        line += c;
    }
}

/// SIGTERM to the process group `pid` leads; SIGKILL when its leader is
/// still there after `timeout`. `reaped`: the leader was waited for already.
void end_group(pid_t pid, bool reaped, std::chrono::milliseconds timeout)
{
    if (pid <= 0) {
        return;
    }
    ::kill(-pid, SIGTERM);
    if (!reaped) {
        const auto deadline = Clock::now() + timeout;
        int status = 0;
        while (::waitpid(pid, &status, WNOHANG) == 0) {
            if (Clock::now() > deadline) {
                log::warn(log_component, "process {} did not end after SIGTERM; killing it", pid);
                ::kill(-pid, SIGKILL);
                ::waitpid(pid, &status, 0);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    ::kill(-pid, SIGTERM);  // whatever is left in the group
}

}  // namespace

std::string user_runtime_dir()
{
    std::string dir;
    if (const char* env = std::getenv("XDG_RUNTIME_DIR"); env != nullptr && *env != '\0') {
        dir = env;
    } else {
        dir = std::format("/run/user/{}", ::geteuid());
    }
    struct stat st{};
    if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() ||
        (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return {};
    }
    return dir;
}

MutterResult<std::unique_ptr<HeadlessShell>> HeadlessShell::launch(const HeadlessShellOptions& options)
{
    const std::string runtime_dir = user_runtime_dir();
    if (runtime_dir.empty()) {
        return fail(PortalErrc::unavailable, "no runtime directory: $XDG_RUNTIME_DIR (or /run/user/UID) must be a "
                                             "directory of this user with mode 0700, as logind creates it");
    }
    std::unique_ptr<HeadlessShell> shell(new HeadlessShell());

    // The private session bus, as dbus-run-session starts it.
    std::array<int, 2> pipe{-1, -1};
    if (::pipe2(pipe.data(), O_CLOEXEC) != 0) {
        return fail(PortalErrc::failed, std::format("pipe: {}", std::strerror(errno)));
    }
    const portal::UniqueFd read_end(pipe[0]);
    // Above 3, so that dup2 onto 3 in the child clears close-on-exec.
    const portal::UniqueFd write_end(
        ::fcntl(pipe[1], F_DUPFD_CLOEXEC, 10));  // NOLINT(cppcoreguidelines-pro-type-vararg)
    ::close(pipe[1]);
    if (!write_end.valid()) {
        return fail(PortalErrc::failed, std::format("fcntl: {}", std::strerror(errno)));
    }
    FARLAND_TRY(shell->bus_pid_, spawn({"dbus-daemon", "--session", "--nofork", "--nopidfile",
                                        std::format("--print-address={}", address_fd)},
                                       child_environment(runtime_dir, {}), write_end.get()));
    shell->bus_address_ = read_line(read_end.get(), options.timeout);
    if (shell->bus_address_.empty()) {
        return fail(PortalErrc::unavailable, "dbus-daemon did not print its address");
    }

    std::vector<std::string> command = options.command;
    if (command.empty()) {
        // Without Xwayland: with it, GNOME Shell sets DISPLAY for itself,
        // and its volume control (libpulse) connects to that X server while
        // starting, which Mutter only starts from the main loop that call
        // blocks (GNOME 50).
        command = {"gnome-shell", "--headless", "--no-x11"};
    }
    FARLAND_TRY(shell->shell_pid_, spawn(command, child_environment(runtime_dir, shell->bus_address_), -1));
    log::info(log_component, "started {} (pid {}) on a private session bus", command.front(), shell->shell_pid_);
    return shell;
}

bool HeadlessShell::running()
{
    if (shell_pid_ <= 0 || exited_) {
        return false;
    }
    int status = 0;
    if (::waitpid(shell_pid_, &status, WNOHANG) != shell_pid_) {
        return true;
    }
    exited_ = true;
    if (WIFEXITED(status)) {
        log::warn(log_component, "the headless GNOME Shell exited with status {}", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        log::warn(log_component, "the headless GNOME Shell was killed by signal {}", WTERMSIG(status));
    }
    return false;
}

HeadlessShell::~HeadlessShell()
{
    if (shell_pid_ > 0) {
        log::info(log_component, "stopping the headless GNOME Shell (pid {})", shell_pid_);
    }
    end_group(shell_pid_, exited_, stop_timeout);
    end_group(bus_pid_, false, stop_timeout);
}

}  // namespace farland::platform::mutter
