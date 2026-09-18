// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/base/unique_fd.hpp>
#include <farland/platform/kwin/kwin_launcher.hpp>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <iterator>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

// Not declared by every libc header; the environment is global by nature.
// NOLINTNEXTLINE(readability-redundant-declaration,cppcoreguidelines-avoid-non-const-global-variables)
extern char** environ;

namespace farland::platform::kwin {

namespace {

constexpr std::string_view log_component = "platform.kwin";
constexpr const char* desktop_file_name = "org.farland.headless-kwin.desktop";
constexpr auto poll_interval = std::chrono::milliseconds(50);
using Clock = std::chrono::steady_clock;

bool plain_path(std::string_view path)
{
    return std::ranges::all_of(path, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' || c == '.' ||
               c == '_' || c == '-' || c == '+';
    });
}

/// ::open() in one place, with the NOLINT its varargs need.
UniqueFd open_file(const char* path, int flags, mode_t mode = 0)
{
    return UniqueFd(::open(path, flags, mode));  // NOLINT(cppcoreguidelines-pro-type-vararg)
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// The last lines of KWin's log, for errors.
void log_tail(const std::filesystem::path& path)
{
    const auto text = read_file(path);
    std::size_t start = text.size();
    for (int lines = 0; lines < 15 && start > 0; ++lines) {
        start = text.rfind('\n', start - 1);
        if (start == std::string::npos) {
            start = 0;
            break;
        }
    }
    if (start < text.size()) {
        log::error(log_component, "the end of {}:\n{}", path.string(), text.substr(start));
    }
}

/// Starts argv[0] (searched in PATH) with `environment` in a new session,
/// its output going to `log_fd`.
Result<int> spawn(const std::vector<std::string>& argv, const std::vector<std::string>& environment, int log_fd)
{
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& a : argv) {
        args.push_back(const_cast<char*>(a.c_str()));  // NOLINT(cppcoreguidelines-pro-type-const-cast): exec's API
    }
    args.push_back(nullptr);
    std::vector<char*> env;
    env.reserve(environment.size() + 1);
    for (const auto& e : environment) {
        env.push_back(const_cast<char*>(e.c_str()));  // NOLINT(cppcoreguidelines-pro-type-const-cast): exec's API
    }
    env.push_back(nullptr);
    const UniqueFd null_fd = open_file("/dev/null", O_RDONLY | O_CLOEXEC);

    const pid_t pid = ::fork();
    if (pid < 0) {
        return fail(Errc::io, "cannot fork");
    }
    if (pid == 0) {
        // Only async-signal-safe calls from here on.
        ::setsid();
        if (null_fd.valid()) {
            ::dup2(null_fd.get(), STDIN_FILENO);
        }
        if (log_fd >= 0) {
            ::dup2(log_fd, STDOUT_FILENO);
            ::dup2(log_fd, STDERR_FILENO);
        }
        ::close_range(3, ~0U, 0);
        ::execvpe(args[0], args.data(), env.data());
        ::_exit(127);
    }
    return pid;
}

std::vector<std::string> build_environment(const LaunchPlan& plan)
{
    std::vector<std::string> result;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): environ is a C array
    for (const char* const* e = environ; e != nullptr && *e != nullptr; ++e) {
        const std::string_view entry(*e);
        const auto name = entry.substr(0, entry.find('='));
        if (std::ranges::none_of(plan.environment, [name](const auto& change) { return change.first == name; })) {
            result.emplace_back(entry);
        }
    }
    for (const auto& [name, value] : plan.environment) {
        if (value) {
            result.push_back(name + "=" + *value);
        }
    }
    return result;
}

bool socket_accepts(const std::filesystem::path& path)
{
    const UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto& native = path.native();
    if (!fd.valid() || native.size() >= sizeof(address.sun_path)) {
        return false;
    }
    std::ranges::copy(native, std::begin(address.sun_path));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API takes a sockaddr
    return ::connect(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
}

/// The Wayland socket of the kwin_wayland the wrapper `wrapper` started,
/// from its command line.
std::optional<std::string> find_kwin_socket(int wrapper)
{
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
        const auto name = entry.path().filename().string();
        if (name.empty() || !std::ranges::all_of(name, [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        const auto stat = read_file(entry.path() / "stat");
        // pid (comm) state ppid ...; comm may contain spaces and parentheses.
        const auto close = stat.rfind(')');
        if (close == std::string::npos) {
            continue;
        }
        std::istringstream fields(stat.substr(close + 1));
        std::string state;
        int ppid = 0;
        if (!(fields >> state >> ppid) || ppid != wrapper) {
            continue;
        }
        const auto cmdline = read_file(entry.path() / "cmdline");
        std::vector<std::string> argv;
        std::size_t begin = 0;
        while (begin < cmdline.size()) {
            auto end = cmdline.find('\0', begin);
            if (end == std::string::npos) {
                end = cmdline.size();
            }
            argv.push_back(cmdline.substr(begin, end - begin));
            begin = end + 1;
        }
        if (auto socket = socket_argument(argv)) {
            return socket;
        }
    }
    return std::nullopt;
}

Result<void> write_desktop_file(const LaunchPlan& plan)
{
    std::error_code ec;
    std::filesystem::create_directories(plan.desktop_file.parent_path(), ec);
    if (read_file(plan.desktop_file) == plan.desktop_file_contents) {
        return {};  // unchanged: KDE's application cache stays valid
    }
    auto temporary = plan.desktop_file;
    temporary += std::format(".{}", ::getpid());
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << plan.desktop_file_contents;
        if (!out) {
            return fail(Errc::io, "cannot write KWin's desktop file");
        }
    }
    std::filesystem::rename(temporary, plan.desktop_file, ec);
    if (ec) {
        return fail(Errc::io, "cannot write KWin's desktop file");
    }
    return {};
}

}  // namespace

std::string screencast_desktop_file(const std::filesystem::path& executable)
{
    const auto path = executable.string();
    // KWin compares the canonical path of Exec's first word with the
    // client's /proc/PID/exe, splitting Exec like QProcess::splitCommand.
    const auto exec = plain_path(path) ? path : std::format("\"{}\"", path);
    return std::format("[Desktop Entry]\n"
                       "Type=Application\n"
                       "Name=farland\n"
                       "Comment=Remote desktop server: screen casting of headless Plasma sessions\n"
                       "Exec={}\n"
                       "NoDisplay=true\n"
                       "X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1\n",
                       exec);
}

std::pair<std::string, std::string> split_xkb_layout(std::string_view layout)
{
    if (const auto open = layout.find('('); open != std::string_view::npos && layout.ends_with(')')) {
        return {std::string(layout.substr(0, open)), std::string(layout.substr(open + 1, layout.size() - open - 2))};
    }
    if (const auto colon = layout.find(':'); colon != std::string_view::npos) {
        return {std::string(layout.substr(0, colon)), std::string(layout.substr(colon + 1))};
    }
    return {std::string(layout), {}};
}

LaunchPlan plan_plasma_launch(const PlasmaLaunchOptions& options, const GetEnv& getenv)
{
    LaunchPlan plan;
    plan.bus_socket = options.runtime_dir / "bus";
    plan.bus_address = "unix:path=" + plan.bus_socket.string();
    plan.dbus_argv = {"dbus-daemon", "--session",  "--nofork",
                      "--nopidfile", "--nosyslog", "--address=" + plan.bus_address};

    plan.kwin_argv = {"kwin_wayland_wrapper",        "--virtual", "--width",
                      std::to_string(options.width), "--height",  std::to_string(options.height)};
    if (options.output_count > 1) {
        plan.kwin_argv.insert(plan.kwin_argv.end(), {"--output-count", std::to_string(options.output_count)});
    }
    plan.kwin_argv.insert(plan.kwin_argv.end(), {"--no-lockscreen", "--xwayland"});
    if (!options.session_command.empty()) {
        plan.kwin_argv.push_back("--exit-with-session=" + options.session_command);
    }

    const auto applications = options.data_dir / "applications";
    plan.desktop_file = applications / desktop_file_name;
    plan.desktop_file_contents = screencast_desktop_file(options.client_executable);

    auto& env = plan.environment;
    env.emplace_back("DBUS_SESSION_BUS_ADDRESS", plan.bus_address);
    // KWin must not become a nested compositor of the session farland runs in.
    for (const char* name : {"WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY"}) {
        env.emplace_back(name, std::nullopt);
    }
    auto data_dirs = getenv("XDG_DATA_DIRS").value_or("");
    if (data_dirs.empty()) {
        data_dirs = "/usr/local/share:/usr/share";  // the XDG default
    }
    const auto own = options.data_dir.string();
    if (std::format(":{}:", data_dirs).find(std::format(":{}:", own)) == std::string::npos) {
        data_dirs += ":" + own;
    }
    env.emplace_back("XDG_DATA_DIRS", data_dirs);
    // What startplasma sets up for a Plasma session.
    env.emplace_back("XDG_CURRENT_DESKTOP", "KDE");
    env.emplace_back("XDG_SESSION_DESKTOP", "KDE");
    env.emplace_back("XDG_SESSION_TYPE", "wayland");
    env.emplace_back("KDE_FULL_SESSION", "true");
    env.emplace_back("KDE_SESSION_VERSION", "6");
    env.emplace_back("XDG_MENU_PREFIX", "plasma-");
    if (!options.keymap_layout.empty()) {
        // KWin then takes the layout from these instead of kxkbrc, which a
        // local session shares.
        const auto [layout, variant] = split_xkb_layout(options.keymap_layout);
        env.emplace_back("KWIN_XKB_DEFAULT_KEYMAP", "1");
        env.emplace_back("XKB_DEFAULT_LAYOUT", layout);
        env.emplace_back("XKB_DEFAULT_VARIANT",
                         variant.empty() ? std::optional<std::string>{} : std::optional<std::string>{variant});
    }
    return plan;
}

std::optional<std::string> socket_argument(std::span<const std::string> argv)
{
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--socket=")) {
            return std::string(arg.substr(9));
        }
        if ((arg == "--socket" || arg == "-s") && i + 1 < argv.size()) {
            return argv[i + 1];
        }
    }
    return std::nullopt;
}

Result<std::unique_ptr<PlasmaProcesses>> PlasmaProcesses::start(const LaunchPlan& plan,
                                                                std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;
    std::unique_ptr<PlasmaProcesses> processes(new PlasmaProcesses());
    processes->runtime_dir_ = plan.bus_socket.parent_path();
    processes->bus_address_ = plan.bus_address;

    std::error_code ec;
    std::filesystem::create_directories(processes->runtime_dir_, ec);
    std::filesystem::permissions(processes->runtime_dir_, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, ec);
    std::filesystem::remove(plan.bus_socket, ec);
    FARLAND_TRY_VOID(write_desktop_file(plan));

    const auto environment = build_environment(plan);
    const auto log_path = processes->runtime_dir_ / "kwin.log";
    const UniqueFd log_fd = open_file(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

    FARLAND_TRY(processes->dbus_pid_, spawn(plan.dbus_argv, environment, log_fd.get()));
    processes->dbus_group_ = processes->dbus_pid_;
    while (!socket_accepts(plan.bus_socket)) {
        if (Clock::now() > deadline || ::waitpid(processes->dbus_pid_, nullptr, WNOHANG) != 0) {
            log_tail(log_path);
            return fail(Errc::io, "the private D-Bus daemon did not start (is dbus-daemon installed?)");
        }
        std::this_thread::sleep_for(poll_interval);
    }

    FARLAND_TRY(processes->kwin_pid_, spawn(plan.kwin_argv, environment, log_fd.get()));
    processes->kwin_group_ = processes->kwin_pid_;
    log::info(log_component, "started KWin (process {}), its output goes to {}", processes->kwin_pid_,
              log_path.string());
    for (;;) {
        if (auto socket = find_kwin_socket(processes->kwin_pid_)) {
            processes->wayland_display_ = *socket;
            break;
        }
        if (processes->exited() || Clock::now() > deadline) {
            log_tail(log_path);
            return fail(Errc::io, "KWin did not start (is kwin_wayland_wrapper installed?)");
        }
        std::this_thread::sleep_for(poll_interval);
    }
    return processes;
}

PlasmaProcesses::~PlasmaProcesses()
{
    stop(kwin_pid_, kwin_group_, std::chrono::seconds(5));
    stop(dbus_pid_, dbus_group_, std::chrono::seconds(2));
    std::error_code ec;
    std::filesystem::remove_all(runtime_dir_, ec);
}

void PlasmaProcesses::log_output_tail() const
{
    log_tail(runtime_dir_ / "kwin.log");
}

bool PlasmaProcesses::exited()
{
    if (kwin_pid_ <= 0) {
        return true;
    }
    if (::waitpid(kwin_pid_, nullptr, WNOHANG) == kwin_pid_) {
        kwin_pid_ = -1;
        return true;
    }
    return false;
}

void PlasmaProcesses::stop(int& pid, int group, std::chrono::milliseconds grace)
{
    if (group <= 0) {
        return;
    }
    // The whole process group: the session's programs are in it too.
    ::kill(-group, SIGTERM);
    const auto deadline = Clock::now() + grace;
    while (Clock::now() < deadline) {
        if (pid > 0 && ::waitpid(pid, nullptr, WNOHANG) == pid) {
            pid = -1;
        }
        if (pid <= 0 && ::kill(-group, 0) != 0) {
            return;  // the group is empty
        }
        std::this_thread::sleep_for(poll_interval);
    }
    ::kill(-group, SIGKILL);
    if (pid > 0) {
        ::waitpid(pid, nullptr, 0);
        pid = -1;
    }
}

}  // namespace farland::platform::kwin
