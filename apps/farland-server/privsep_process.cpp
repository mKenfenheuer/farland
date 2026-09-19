// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "privsep_process.hpp"

#include <farland/base/log.hpp>
#include <farland/base/text.hpp>
#include <farland/server/privsep.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char** environ;  // NOLINT(readability-redundant-declaration): POSIX leaves its declaration to the caller

namespace farland::app {

namespace {

using Clock = std::chrono::steady_clock;
namespace privsep = server::privsep;
constexpr std::string_view log_component = "app.privsep";
/// How long the network process may take to answer, and the monitor to verify.
constexpr int control_timeout_ms = 10'000;
constexpr std::size_t max_cookie_report = 256;

/// A socket pair, both ends close-on-exec.
bool make_socket_pair(std::array<int, 2>& fds)
{
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) != 0) {
        log::error(log_component, "socketpair: {}", std::strerror(errno));
        return false;
    }
    prepare_socket(fds[0]);
    prepare_socket(fds[1]);
    return true;
}

/// Reads one framed privsep message into `buffer` (which may already hold
/// the start of it). The frame, or nullopt on EOF, timeout or a bad frame.
std::optional<std::vector<std::byte>> read_frame(int fd, std::vector<std::byte>& buffer, int timeout_ms)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::array<std::byte, 4096> chunk{};
    while (true) {
        const auto length = privsep::message_length(buffer);
        if (!length) {
            log::warn(log_component, "bad control message: {}", length.error().message());
            return std::nullopt;
        }
        if (*length) {
            std::vector<std::byte> frame(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(**length));
            secure_zero(std::span(buffer).first(**length));
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(**length));
            return frame;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) {
            log::warn(log_component, "control channel timed out");
            return std::nullopt;
        }
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, static_cast<int>(left)) <= 0) {
            continue;
        }
        const ssize_t received = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (received < 0 && (errno == EINTR || would_block(errno))) {
            continue;
        }
        if (received <= 0) {
            return std::nullopt;
        }
        buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + received);
    }
}

/// Logs an abnormal end of network process `pid`.
void report_exit(pid_t pid, int status)
{
    if (WIFSIGNALED(status)) {
        log::warn(log_component, "network process {} died of signal {}{}", pid, WTERMSIG(status),
                  WTERMSIG(status) == SIGSYS ? " (a system call the sandbox forbids)" : "");
    } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        log::warn(log_component, "network process {} exited with status {}", pid, WEXITSTATUS(status));
    }
}

/// Waits for the network process to exit, killing it if it lingers.
class ChildProcess {
public:
    explicit ChildProcess(pid_t pid) : pid_(pid) {}
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&&) = delete;
    ChildProcess& operator=(ChildProcess&&) = delete;

    ~ChildProcess()
    {
        int status = 0;
        for (int i = 0; i < 40; ++i) {
            const pid_t done = ::waitpid(pid_, &status, WNOHANG);
            if (done == pid_ || (done < 0 && errno != EINTR)) {
                report(status, done == pid_);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        ::kill(pid_, SIGKILL);
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        log::warn(log_component, "network process {} did not exit and was killed", pid_);
    }

private:
    void report(int status, bool reaped) const
    {
        if (reaped) {
            report_exit(pid_, status);
        }
    }

    pid_t pid_;
};

/// Starts the network process with the three descriptors at 3, 4 and 5.
pid_t spawn_child(const ChildLaunch& launch, const std::string& peer, std::array<int, 3> fds)
{
    // Move the descriptors out of the way first so that none of the dup2
    // targets is still needed as a source.
    std::array<int, 3> high{-1, -1, -1};
    for (std::size_t i = 0; i < fds.size(); ++i) {
        high.at(i) = ::fcntl(fds.at(i), F_DUPFD_CLOEXEC, 10);
    }
    const auto close_high = [&high] {
        for (const int fd : high) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    };
    if (std::ranges::find(high, -1) != high.end()) {
        log::error(log_component, "cannot duplicate descriptors for the network process: {}", std::strerror(errno));
        close_high();
        return -1;
    }
    std::vector<std::string> args{launch.executable.string(), "--privsep-child", "--peer", peer};
    args.insert(args.end(), launch.arguments.begin(), launch.arguments.end());
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_adddup2(&actions, high[0], child_client_fd);
    ::posix_spawn_file_actions_adddup2(&actions, high[1], child_control_fd);
    ::posix_spawn_file_actions_adddup2(&actions, high[2], child_plain_fd);
    posix_spawnattr_t attributes;
    ::posix_spawnattr_init(&attributes);
    // Its own process group, so a terminal's Ctrl+C reaches only the monitor,
    // which then ends the sessions in order.
    ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    ::posix_spawnattr_setpgroup(&attributes, 0);

    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, launch.executable.c_str(), &actions, &attributes, argv.data(), environ);
    ::posix_spawnattr_destroy(&attributes);
    ::posix_spawn_file_actions_destroy(&actions);
    close_high();
    if (rc != 0) {
        log::error(log_component, "cannot start the network process {}: {}", launch.executable.string(),
                   std::strerror(rc));
        return -1;
    }
    return pid;
}

/// Answers the network process until it reports a finished pre-authentication.
std::optional<server::Negotiation> await_authentication(int control, const std::string& peer,
                                                        auth::NtlmVerifier& verifier, const SessionOptions& options,
                                                        const std::atomic<bool>& stop, Clock::time_point started,
                                                        const auth::kerberos::Credential* kerberos)
{
    privsep::MonitorService service(verifier);
    if (kerberos != nullptr) {
        service.serve_kerberos([kerberos](std::span<const std::byte> oid) { return kerberos->accept(oid); });
    }
    std::vector<std::byte> buffer;
    const auto deadline = started + std::chrono::seconds(options.activation_timeout);
    while (!stop.load()) {
        if (Clock::now() > deadline) {
            log::warn(log_component, "{}: pre-authentication took longer than {} s", peer, options.activation_timeout);
            return std::nullopt;
        }
        pollfd pfd{control, POLLIN, 0};
        if (::poll(&pfd, 1, 250) <= 0) {
            continue;
        }
        auto frame = read_frame(control, buffer, control_timeout_ms);
        if (!frame) {
            return std::nullopt;  // the network process ended; it logged why
        }
        auto reply = service.handle(*frame);
        secure_zero(*frame);
        if (!reply) {
            log::error(log_component, "{}: network process misbehaved: {}", peer, reply.error().message());
            return std::nullopt;
        }
        if (*reply) {
            const bool sent = send_all(control, **reply);
            secure_zero(**reply);
            if (!sent) {
                return std::nullopt;
            }
        }
        if (auto negotiation = service.take_authenticated()) {
            return negotiation;
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<AuthenticatedClient> authenticate_monitored(int client_fd, const std::string& peer,
                                                          const ChildLaunch& launch, auth::NtlmVerifier& verifier,
                                                          const SessionOptions& options, const std::atomic<bool>& stop,
                                                          Clock::time_point started,
                                                          const auth::kerberos::Credential* kerberos)
{
    std::array<int, 2> control{-1, -1};
    std::array<int, 2> plain{-1, -1};
    if (!make_socket_pair(control) || !make_socket_pair(plain)) {
        for (const int fd : {client_fd, control[0], control[1]}) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
        return std::nullopt;
    }
    const pid_t pid = spawn_child(launch, peer, {client_fd, control[1], plain[1]});
    ::close(client_fd);
    ::close(control[1]);
    ::close(plain[1]);
    if (pid < 0) {
        ::close(control[0]);
        ::close(plain[0]);
        return std::nullopt;
    }
    log::debug(log_component, "{}: network process {}", peer, pid);

    auto negotiation = await_authentication(control[0], peer, verifier, options, stop, started, kerberos);
    ::close(control[0]);
    if (!negotiation) {
        ::close(plain[0]);
        const ChildProcess child(pid);
        return std::nullopt;
    }
    return AuthenticatedClient{std::move(*negotiation), UniqueFd(plain[0]), pid};
}

void wait_network_process(pid_t pid)
{
    int status = 0;
    pid_t done = -1;
    do {
        done = ::waitpid(pid, &status, 0);
    } while (done < 0 && errno == EINTR);
    if (done == pid) {
        report_exit(pid, status);
    }
}

void run_monitored_session(int client_fd, const std::string& peer, const ChildLaunch& launch,
                           auth::NtlmVerifier& verifier, const SessionOptions& options, const std::atomic<bool>& stop,
                           const auth::kerberos::Credential* kerberos)
{
    const auto started = Clock::now();
    auto client = authenticate_monitored(client_fd, peer, launch, verifier, options, stop, started, kerberos);
    if (!client) {
        return;
    }
    const ChildProcess child(client->network_process);  // reaped after the transport closed the stream
    PlainTransport transport(client->plain.release(), std::move(client->negotiation));
    run_session(transport, peer, options, stop, started);
}

int run_network_child(const std::string& peer, const auth::TlsIdentity& identity, const SessionOptions& options,
                      const NlaFactoryMaker& make_nla, bool kerberos, bool kerberos_only)
{
    prepare_socket(child_client_fd);
    prepare_socket(child_control_fd);
    prepare_socket(child_plain_fd);

    std::vector<std::byte> control_buffer;
    const privsep::Call ask = [&](std::span<const std::byte> request) -> Result<std::vector<std::byte>> {
        if (!send_all(child_control_fd, request)) {
            return fail(Errc::io, "the monitor is gone");
        }
        auto reply = read_frame(child_control_fd, control_buffer, control_timeout_ms);
        if (!reply) {
            return fail(Errc::io, "no answer from the monitor");
        }
        return std::move(*reply);
    };
    privsep::RemoteVerifier verifier(ask);
    const NlaBackends backends{.verifier = verifier,
                               .monitor = ask,
                               .kerberos = kerberos,
                               .credential = nullptr,
                               .kerberos_only = kerberos_only};
    NetworkStage network(child_client_fd, peer, identity, options.preauth,
                         make_nla ? make_nla(backends) : server::PreAuth::NlaFactory{});

    const auto started = Clock::now();
    std::vector<std::byte> data;
    std::array<std::byte, 64 * 1024> chunk{};
    bool reported = false;
    int status = 0;
    while (true) {
        if (!reported && Clock::now() - started > std::chrono::seconds(options.activation_timeout)) {
            log::warn(log_component, "{}: pre-authentication took longer than {} s", peer, options.activation_timeout);
            status = 1;
            break;
        }
        std::array<pollfd, 2> fds{pollfd{child_client_fd, POLLIN, 0},
                                  pollfd{child_plain_fd, static_cast<short>(reported ? POLLIN : 0), 0}};
        ::poll(fds.data(), fds.size(), 250);
        if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            data.clear();
            if (!network.read(data)) {
                break;
            }
            if (!reported && network.ready()) {
                auto negotiation = network.negotiation();
                if (negotiation.cookie.size() > max_cookie_report) {
                    negotiation.cookie.resize(max_cookie_report);
                }
                if (!send_all(child_control_fd, privsep::encode(privsep::Authenticated{negotiation}))) {
                    status = 1;
                    break;
                }
                reported = true;
            }
            if (!data.empty() && !send_all(child_plain_fd, data)) {
                break;
            }
        }
        if (reported && (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            const ssize_t received = ::recv(child_plain_fd, chunk.data(), chunk.size(), 0);
            if (received < 0 && (errno == EINTR || would_block(errno))) {
                continue;
            }
            if (received <= 0 || !network.send(std::span(chunk).first(static_cast<std::size_t>(received)))) {
                break;  // the session ended
            }
        }
    }
    network.close();
    ::close(child_control_fd);
    ::close(child_plain_fd);
    return status;
}

std::filesystem::path current_executable(const char* argv0)
{
#ifdef __linux__
    std::error_code ec;
    auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        return path;
    }
#elif defined(__APPLE__)
    std::array<char, 4096> buffer{};
    auto size = static_cast<std::uint32_t>(buffer.size());
    if (::_NSGetExecutablePath(buffer.data(), &size) == 0) {
        return std::filesystem::weakly_canonical(buffer.data());
    }
#endif
    return std::filesystem::absolute(argv0);
}

}  // namespace farland::app
