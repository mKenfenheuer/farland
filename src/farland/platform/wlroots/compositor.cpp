// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/base/unique_fd.hpp>
#include <farland/platform/wlroots/compositor.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <memory>
#include <spawn.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace farland::platform::wlroots {

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::string_view log_component = "platform.wlroots.compositor";
constexpr auto stop_timeout = std::chrono::seconds(5);
constexpr auto poll_interval = std::chrono::milliseconds(20);

/// NAME of a NAME=value entry.
std::string_view variable_name(std::string_view entry)
{
    return entry.substr(0, entry.find('='));
}

std::vector<std::string> current_environment()
{
    std::vector<std::string> entries;
    // environ (unistd.h with _GNU_SOURCE) is a null-terminated array.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    for (char* const* entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        entries.emplace_back(*entry);
    }
    return entries;
}

/// The socket inodes a process has open.
std::vector<std::uint64_t> socket_inodes(pid_t pid)
{
    std::vector<std::uint64_t> inodes;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(std::format("/proc/{}/fd", pid), ec)) {
        const auto target = std::filesystem::read_symlink(entry.path(), ec).string();
        // "socket:[12345]"
        if (!target.starts_with("socket:[") || !target.ends_with(']')) {
            continue;
        }
        const std::string_view digits = std::string_view(target).substr(8, target.size() - 9);
        std::uint64_t inode = 0;
        if (std::from_chars(std::to_address(digits.begin()), std::to_address(digits.end()), inode).ec == std::errc{}) {
            inodes.push_back(inode);
        }
    }
    return inodes;
}

std::string read_file(const std::filesystem::path& path)
{
    const std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

/// $XDG_RUNTIME_DIR when it is a directory; empty otherwise.
std::string user_runtime_dir()
{
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    std::error_code ec;
    if (dir == nullptr || *dir == '\0' || !std::filesystem::is_directory(dir, ec)) {
        return {};
    }
    return dir;
}

std::vector<char*> c_strings(std::vector<std::string>& strings)
{
    std::vector<char*> pointers;
    pointers.reserve(strings.size() + 1);
    for (auto& s : strings) {
        pointers.push_back(s.data());
    }
    pointers.push_back(nullptr);
    return pointers;
}

}  // namespace

std::string_view to_string(CompositorKind kind) noexcept
{
    switch (kind) {
    case CompositorKind::sway:
        return "sway";
    case CompositorKind::labwc:
        return "labwc";
    case CompositorKind::cage:
        return "cage";
    }
    return "unknown";
}

std::vector<std::string> compositor_command(const CompositorLaunch& launch)
{
    switch (launch.kind) {
    case CompositorKind::sway:
        // --unsupported-gpu: sway refuses to start while NVIDIA's driver is
        // loaded, even though the headless backend never touches it.
        return {"sway", "--unsupported-gpu"};
    case CompositorKind::labwc:
        return {"labwc"};
    case CompositorKind::cage: {
        std::vector<std::string> command{"cage", "--"};
        command.insert(command.end(), launch.command.begin(), launch.command.end());
        return command;
    }
    }
    return {};
}

std::vector<std::string> compositor_environment(const CompositorLaunch& launch,
                                                std::span<const std::string> environment,
                                                const std::string& runtime_dir)
{
    static constexpr std::array<std::string_view, 9> replaced{
        "WAYLAND_DISPLAY", "WAYLAND_SOCKET",          "DISPLAY",
        "WLR_BACKENDS",    "WLR_LIBINPUT_NO_DEVICES", "WLR_HEADLESS_OUTPUTS",
        "WLR_RENDERER",    "WLR_RENDER_DRM_DEVICE",   "XDG_RUNTIME_DIR",
    };
    std::vector<std::string> result;
    for (const auto& entry : environment) {
        if (std::ranges::find(replaced, variable_name(entry)) == replaced.end()) {
            result.push_back(entry);
        }
    }
    result.emplace_back("WLR_BACKENDS=headless");
    result.emplace_back("WLR_LIBINPUT_NO_DEVICES=1");
    result.push_back(std::format("WLR_HEADLESS_OUTPUTS={}", std::max<std::uint32_t>(launch.outputs, 1)));
    if (launch.render_node.empty()) {
        result.emplace_back("WLR_RENDERER=pixman");
    } else {
        result.push_back("WLR_RENDER_DRM_DEVICE=" + launch.render_node);
    }
    result.push_back("XDG_RUNTIME_DIR=" + runtime_dir);
    return result;
}

std::optional<std::string> find_wayland_socket(std::string_view proc_net_unix, std::span<const std::uint64_t> inodes,
                                               std::string_view runtime_dir)
{
    // "Num RefCount Protocol Flags Type St Inode Path", one socket per line.
    std::istringstream lines{std::string(proc_net_unix)};
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string num;
        std::string refcount;
        std::string protocol;
        std::string flags;
        std::string type;
        std::string state;
        std::uint64_t inode = 0;
        std::string path;
        if (!(fields >> num >> refcount >> protocol >> flags >> type >> state >> inode >> path)) {
            continue;
        }
        if (std::ranges::find(inodes, inode) == inodes.end()) {
            continue;
        }
        const std::filesystem::path p(path);
        const auto name = p.filename().string();
        if (p.parent_path() != std::filesystem::path(runtime_dir) || !name.starts_with("wayland-") ||
            name.ends_with(".lock")) {
            continue;
        }
        return path;
    }
    return std::nullopt;
}

Result<std::unique_ptr<CompositorProcess>> CompositorProcess::launch(const CompositorLaunch& launch)
{
    if (launch.kind == CompositorKind::cage && launch.command.empty()) {
        return fail(Errc::invalid_value, "cage needs an application to run");
    }
    std::string runtime_dir = user_runtime_dir();
    std::filesystem::path private_dir;
    if (runtime_dir.empty()) {
        std::string pattern = (std::filesystem::temp_directory_path() / "farland-runtime-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr) {
            return fail(Errc::io, "cannot create a runtime directory for the compositor");
        }
        runtime_dir = pattern;
        private_dir = pattern;
        log::info(log_component, "XDG_RUNTIME_DIR is not set; the compositor runs in {}", runtime_dir);
    }

    static std::atomic<unsigned> launches{0};
    const std::filesystem::path log_path =
        std::filesystem::path(runtime_dir) /
        std::format("farland-{}-{}-{}.log", to_string(launch.kind), ::getpid(), launches.fetch_add(1));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const UniqueFd log_fd(::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
    if (!log_fd.valid()) {
        std::error_code ec;
        std::filesystem::remove_all(private_dir, ec);
        return fail(Errc::io, "cannot create the compositor's log file");
    }

    auto argv_strings = compositor_command(launch);
    auto env_strings = compositor_environment(launch, current_environment(), runtime_dir);
    auto argv = c_strings(argv_strings);
    auto envp = c_strings(env_strings);

    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, log_fd.get(), STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, log_fd.get(), STDERR_FILENO);
    posix_spawnattr_init(&attributes);
    // A process group of its own, so stopping it reaches its clients too;
    // and the signals farland ignores or blocks back to their defaults.
    sigset_t none{};
    sigemptyset(&none);
    sigset_t defaults{};
    sigemptyset(&defaults);
    for (const int signal : {SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGCHLD}) {
        sigaddset(&defaults, signal);
    }
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    posix_spawnattr_setpgroup(&attributes, 0);
    posix_spawnattr_setsigmask(&attributes, &none);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    pid_t pid = 0;
    const int spawned = ::posix_spawnp(&pid, argv.front(), &actions, &attributes, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (spawned != 0) {
        log::error(log_component, "cannot start {}: {}", argv_strings.front(), std::strerror(spawned));
        std::error_code ec;
        std::filesystem::remove_all(private_dir, ec);
        return fail(Errc::io, "cannot start the compositor");
    }
    log::info(log_component, "started {} (pid {}), log in {}", to_string(launch.kind), pid, log_path.string());
    std::unique_ptr<CompositorProcess> process(new CompositorProcess(pid, log_path, private_dir));

    const auto deadline = Clock::now() + launch.timeout;
    while (true) {
        if (process->exited()) {
            log::error(log_component, "{} exited while starting:\n{}", to_string(launch.kind), process->log_tail());
            return fail(Errc::io, "the compositor exited while starting");
        }
        const auto inodes = socket_inodes(pid);
        if (!inodes.empty()) {
            if (auto socket = find_wayland_socket(read_file("/proc/net/unix"), inodes, runtime_dir)) {
                process->socket_ = std::move(*socket);
                break;
            }
        }
        if (Clock::now() > deadline) {
            log::error(log_component, "{} opened no Wayland socket in time:\n{}", to_string(launch.kind),
                       process->log_tail());
            return fail(Errc::io, "the compositor did not open its Wayland socket");
        }
        std::this_thread::sleep_for(poll_interval);
    }
    log::debug(log_component, "{} listens on {}", to_string(launch.kind), process->socket_);
    return process;
}

CompositorProcess::CompositorProcess(pid_t pid, std::filesystem::path log, std::filesystem::path private_runtime_dir)
    : pid_(pid), log_(std::move(log)), private_runtime_dir_(std::move(private_runtime_dir))
{
}

CompositorProcess::~CompositorProcess()
{
    if (!exited()) {
        ::kill(-pid_, SIGTERM);
        const auto deadline = Clock::now() + stop_timeout;
        while (!exited() && Clock::now() < deadline) {
            std::this_thread::sleep_for(poll_interval);
        }
        if (!exited()) {
            log::warn(log_component, "the compositor (pid {}) ignored SIGTERM; killing it", pid_);
            ::kill(-pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
            exited_ = true;
        }
    }
    // Clients the compositor left behind in its group.
    ::kill(-pid_, SIGTERM);
    if (!private_runtime_dir_.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(private_runtime_dir_, ec);
    }
}

bool CompositorProcess::exited()
{
    if (!exited_) {
        int status = 0;
        const pid_t result = ::waitpid(pid_, &status, WNOHANG);
        exited_ = result == pid_ || (result < 0 && errno == ECHILD);
    }
    return exited_;
}

std::string CompositorProcess::log_tail(std::size_t lines) const
{
    std::ifstream in(log_);
    std::deque<std::string> tail;
    std::string line;
    while (std::getline(in, line)) {
        tail.push_back(std::move(line));
        if (tail.size() > lines) {
            tail.pop_front();
        }
    }
    std::string text;
    for (const auto& l : tail) {
        text += l;
        text += '\n';
    }
    return text;
}

}  // namespace farland::platform::wlroots
