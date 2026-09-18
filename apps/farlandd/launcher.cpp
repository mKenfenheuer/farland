// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "launcher.hpp"

#include <farland/base/log.hpp>
#include <farland/base/text.hpp>

#include "agent_token.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef FARLAND_HAVE_PAM
#include <grp.h>
#include <security/pam_appl.h>
#endif

extern char** environ;  // NOLINT(readability-redundant-declaration): POSIX leaves its declaration to the caller

namespace farland::daemon {

namespace {

constexpr std::string_view log_component = "daemon.launch";

/// NULL-terminated pointers into `strings`, for exec and posix_spawn.
std::vector<char*> pointers(std::span<const std::string> strings)
{
    std::vector<char*> out;
    out.reserve(strings.size() + 1);
    for (const auto& s : strings) {
        out.push_back(const_cast<char*>(s.c_str()));  // NOLINT(cppcoreguidelines-pro-type-const-cast): exec only reads
    }
    out.push_back(nullptr);
    return out;
}

/// A pipe holding the token (and nothing else), whose read end the child
/// gets at descriptor 3. Returns the read end.
int token_pipe(const server::broker::Token& token)
{
    std::array<int, 2> fds{-1, -1};
    if (::pipe(fds.data()) != 0) {
        return -1;
    }
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    std::string text = app::token_to_hex(token) + "\n";
    const bool written = ::write(fds[1], text.data(), text.size()) == static_cast<ssize_t>(text.size());
    secure_zero(std::as_writable_bytes(std::span(text)));
    ::close(fds[1]);
    if (!written) {
        ::close(fds[0]);
        return -1;
    }
    return fds[0];
}

Result<pid_t> spawn(const std::vector<std::string>& args, std::span<const std::string> environment,
                    const server::broker::Token& token)
{
    const int token_fd = token_pipe(token);
    if (token_fd < 0) {
        return fail(Errc::io, "cannot pass the session token");
    }
    auto argv = pointers(args);
    auto envp = pointers(environment);
    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_adddup2(&actions, token_fd, agent_token_fd);
    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, args.front().c_str(), &actions, nullptr, argv.data(), envp.data());
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(token_fd);
    if (rc != 0) {
        log::error(log_component, "cannot start {}: {}", args.front(), std::strerror(rc));
        return fail(Errc::io, "cannot start the process");
    }
    return pid;
}

std::vector<std::string> current_environment()
{
    std::vector<std::string> out;
    for (char** e = environ; e != nullptr && *e != nullptr;
         ++e) {  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic): environ is a C array
        if (!std::string_view(*e).starts_with(app::agent_token_variable)) {
            out.emplace_back(*e);
        }
    }
    return out;
}

bool has_variable(std::span<const std::string> environment, std::string_view name)
{
    return std::ranges::any_of(environment, [name](const std::string& e) {
        return e.size() > name.size() && e.starts_with(name) && e[name.size()] == '=';
    });
}

std::optional<std::string> variable(std::span<const std::string> environment, std::string_view name)
{
    for (const auto& e : environment) {
        if (e.size() > name.size() && e.starts_with(name) && e[name.size()] == '=') {
            return e.substr(name.size() + 1);
        }
    }
    return std::nullopt;
}

}  // namespace

std::vector<std::string> agent_arguments(const AgentLaunch& launch)
{
    std::vector<std::string> args{launch.agent.string(),
                                  "--socket",
                                  launch.socket,
                                  "--session-id",
                                  std::to_string(launch.session_id),
                                  "--desktop",
                                  std::string(to_string(launch.desktop)),
                                  "--log-level",
                                  launch.log_level};
    if (launch.token_on_fd) {
        args.emplace_back("--token-fd");
        args.push_back(std::to_string(agent_token_fd));
    }
    if (launch.attach) {
        args.emplace_back("--attach");
    }
    if (launch.desktop == DesktopKind::cage && !launch.cage_command.empty()) {
        args.emplace_back("--");
        args.insert(args.end(), launch.cage_command.begin(), launch.cage_command.end());
    }
    return args;
}

std::optional<Account> lookup_account(const std::string& name)
{
    passwd entry{};
    passwd* found = nullptr;
    std::vector<char> buffer(16384);
    if (::getpwnam_r(name.c_str(), &entry, buffer.data(), buffer.size(), &found) != 0 || found == nullptr) {
        return std::nullopt;
    }
    return Account{entry.pw_name, entry.pw_uid, entry.pw_gid, entry.pw_dir != nullptr ? entry.pw_dir : "/",
                   entry.pw_shell != nullptr ? entry.pw_shell : "/bin/sh"};
}

std::string_view session_desktop_name(DesktopKind desktop) noexcept
{
    switch (desktop) {
    case DesktopKind::gnome:
        return "GNOME";
    case DesktopKind::plasma:
        return "KDE";
    case DesktopKind::sway:
        return "sway";
    case DesktopKind::labwc:
        return "labwc";
    case DesktopKind::cage:
        return "cage";
    case DesktopKind::test:
        break;
    }
    return "farland";
}

std::vector<std::string> pam_session_variables(DesktopKind desktop)
{
    return {"XDG_SESSION_TYPE=wayland", "XDG_SESSION_CLASS=user",
            "XDG_SESSION_DESKTOP=" + std::string(session_desktop_name(desktop))};
}

std::vector<std::string> agent_environment(std::span<const std::string> pam_environment, const Account& account,
                                           DesktopKind desktop)
{
    std::vector<std::string> env;
    for (const auto& e : pam_environment) {
        if (e.find('=') != std::string::npos && !e.starts_with(app::agent_token_variable)) {
            env.push_back(e);
        }
    }
    const auto add = [&env](std::string_view name, const std::string& value) {
        if (!has_variable(env, name)) {
            env.push_back(std::string(name) + "=" + value);
        }
    };
    add("HOME", account.home);
    add("USER", account.name);
    add("LOGNAME", account.name);
    add("SHELL", account.shell);
    add("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
    for (const auto& v : pam_session_variables(desktop)) {
        const auto equals = v.find('=');
        add(std::string_view(v).substr(0, equals), v.substr(equals + 1));
    }
    add("XDG_CURRENT_DESKTOP", std::string(session_desktop_name(desktop)));
    if (const auto runtime = variable(env, "XDG_RUNTIME_DIR")) {
        add("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + *runtime + "/bus");
    }
    return env;
}

std::string remote_host(std::string_view peer)
{
    if (peer.starts_with('[')) {
        const auto close = peer.find(']');
        return std::string(peer.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1));
    }
    const auto colon = peer.rfind(':');
    return std::string(colon == std::string_view::npos ? peer : peer.substr(0, colon));
}

Result<pid_t> spawn_agent(const AgentLaunch& launch, const server::broker::Token& token,
                          std::span<const std::string> environment)
{
    auto launch_fd = launch;
    launch_fd.token_on_fd = true;
    const auto env =
        environment.empty() ? current_environment() : std::vector<std::string>(environment.begin(), environment.end());
    return spawn(agent_arguments(launch_fd), env, token);
}

Result<pid_t> spawn_session_helper(const std::filesystem::path& self, const AgentLaunch& launch,
                                   const std::string& account, const std::string& rhost,
                                   const server::broker::Token& token)
{
    auto launch_fd = launch;
    launch_fd.token_on_fd = true;
    std::vector<std::string> args{self.string(), "--session-helper",
                                  "--user",      account,
                                  "--desktop",   std::string(to_string(launch.desktop)),
                                  "--rhost",     rhost,
                                  "--log-level", launch.log_level,
                                  "--"};
    const auto agent = agent_arguments(launch_fd);
    args.insert(args.end(), agent.begin(), agent.end());
    const std::vector<std::string> env{"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"};
    return spawn(args, env, token);
}

#ifdef FARLAND_HAVE_PAM

namespace {

std::atomic<pid_t> helper_child{-1};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): signal state

extern "C" void forward_signal(int signal)
{
    const pid_t child = helper_child.load();
    if (child > 0) {
        ::kill(child, signal);
    }
}

/// No prompts: NLA authenticated the user, and PAM must not ask anything.
extern "C" int no_conversation(int /*count*/, const pam_message** /*messages*/, pam_response** /*responses*/,
                               void* /*data*/)
{
    return PAM_CONV_ERR;
}

class PamSession {
public:
    PamSession() = default;
    PamSession(const PamSession&) = delete;
    PamSession& operator=(const PamSession&) = delete;
    PamSession(PamSession&&) = delete;
    PamSession& operator=(PamSession&&) = delete;
    ~PamSession()
    {
        if (handle_ == nullptr) {
            return;
        }
        if (opened_) {
            const int rc = pam_close_session(handle_, 0);
            log::info(log_component, "PAM session closed{}", rc == PAM_SUCCESS ? "" : ": " + error(rc));
        }
        if (credentials_) {
            pam_setcred(handle_, PAM_DELETE_CRED);
        }
        pam_end(handle_, last_);
    }

    bool open(const std::string& user, const std::string& rhost, DesktopKind desktop)
    {
        const pam_conv conversation{no_conversation, nullptr};
        last_ = pam_start("farland", user.c_str(), &conversation, &handle_);
        if (last_ != PAM_SUCCESS) {
            log::error(log_component, "pam_start for {}: {}", user, error(last_));
            return false;
        }
        if (!rhost.empty()) {
            pam_set_item(handle_, PAM_RHOST, rhost.c_str());
        }
        pam_set_item(handle_, PAM_RUSER, user.c_str());
        for (const auto& v : pam_session_variables(desktop)) {
            pam_putenv(handle_, v.c_str());
        }
        if ((last_ = pam_acct_mgmt(handle_, 0)) != PAM_SUCCESS) {
            log::error(log_component, "PAM refuses the account {}: {}", user, error(last_));
            return false;
        }
        if ((last_ = pam_setcred(handle_, PAM_ESTABLISH_CRED)) != PAM_SUCCESS) {
            log::error(log_component, "PAM credentials for {}: {}", user, error(last_));
            return false;
        }
        credentials_ = true;
        if ((last_ = pam_open_session(handle_, 0)) != PAM_SUCCESS) {
            log::error(log_component, "PAM session for {}: {}", user, error(last_));
            return false;
        }
        opened_ = true;
        return true;
    }

    [[nodiscard]] std::vector<std::string> environment() const
    {
        std::vector<std::string> out;
        char** list = pam_getenvlist(handle_);
        if (list == nullptr) {
            return out;
        }
        for (char** e = list; *e != nullptr;
             ++e) {  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic): a C array
            out.emplace_back(*e);
            std::free(*e);  // NOLINT(cppcoreguidelines-no-malloc): PAM allocates with malloc
        }
        std::free(static_cast<void*>(list));  // NOLINT(cppcoreguidelines-no-malloc)
        return out;
    }

private:
    [[nodiscard]] std::string error(int rc) const { return pam_strerror(handle_, rc); }

    pam_handle_t* handle_ = nullptr;
    int last_ = PAM_SUCCESS;
    bool credentials_ = false;
    bool opened_ = false;
};

}  // namespace

bool pam_supported() noexcept
{
    return true;
}

int run_session_helper(std::span<char*> args)
{
    std::string user;
    std::string rhost;
    DesktopKind desktop = DesktopKind::test;
    std::vector<std::string> agent;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const bool has_value = i + 1 < args.size();
        if (arg == "--session-helper") {
            continue;
        }
        if (arg == "--user" && has_value) {
            user = args[++i];
        } else if (arg == "--rhost" && has_value) {
            rhost = args[++i];
        } else if (arg == "--desktop" && has_value) {
            const std::string_view name = args[++i];
            for (auto kind : {DesktopKind::gnome, DesktopKind::plasma, DesktopKind::sway, DesktopKind::labwc,
                              DesktopKind::cage, DesktopKind::test}) {
                if (to_string(kind) == name) {
                    desktop = kind;
                }
            }
        } else if (arg == "--log-level" && has_value) {
            ++i;  // farlandd's main set it
        } else if (arg == "--") {
            agent.assign(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
            break;
        }
    }
    const auto account = lookup_account(user);
    if (!account || agent.empty() || ::geteuid() != 0) {
        log::error(log_component, "session helper: needs root, an existing --user and the agent's command line");
        return 2;
    }

    PamSession pam;
    if (!pam.open(account->name, rhost, desktop)) {
        return 1;
    }
    const auto environment = agent_environment(pam.environment(), *account, desktop);
    const auto session_id = variable(environment, "XDG_SESSION_ID");
    if (!session_id) {
        // pam_systemd gives the user a session only where the daemon is one
        // of systemd's own (a service, or systemd-run). Started from a login
        // shell, it registers nothing, and the desktop then runs without
        // logind's runtime directory and without the user's service manager.
        log::warn(log_component,
                  "PAM opened no logind session for {} (no XDG_SESSION_ID): run farlandd as a systemd service. "
                  "The desktop gets a temporary runtime directory and no user service manager",
                  account->name);
    }
    log::info(log_component, "PAM session {} open for {} ({}), desktop {}", session_id.value_or("?"), account->name,
              rhost.empty() ? "local" : rhost, to_string(desktop));

    auto argv = pointers(agent);
    auto envp = pointers(environment);
    // This process is single-threaded, so fork() is safe here.
    const pid_t child = ::fork();
    if (child == 0) {
        // The user's process: its groups and ids, its home, the token on 3.
        if (::initgroups(account->name.c_str(), account->gid) != 0 || ::setgid(account->gid) != 0 ||
            ::setuid(account->uid) != 0 || ::setuid(0) == 0) {
            ::_exit(126);
        }
        if (::chdir(account->home.c_str()) != 0 && ::chdir("/") != 0) {
            ::_exit(126);
        }
        ::execve(argv.front(), argv.data(), envp.data());
        ::_exit(127);
    }
    ::close(agent_token_fd);
    if (child < 0) {
        log::error(log_component, "cannot fork the agent: {}", std::strerror(errno));
        return 1;
    }
    helper_child = child;
    std::signal(SIGTERM, forward_signal);
    std::signal(SIGINT, forward_signal);
    std::signal(SIGHUP, forward_signal);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    helper_child = -1;
    if (WIFEXITED(status)) {
        log::info(log_component, "agent of {} exited with status {}", account->name, WEXITSTATUS(status));
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        log::info(log_component, "agent of {} ended by signal {}", account->name, WTERMSIG(status));
    }
    return 1;
}

#else

bool pam_supported() noexcept
{
    return false;
}

int run_session_helper(std::span<char*> /*args*/)
{
    log::error(log_component, "this farlandd was built without PAM");
    return 2;
}

#endif

}  // namespace farland::daemon
