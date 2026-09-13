// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// farland-server, M1: an RDP server over TLS with the synthetic test backend.
// Wayland capture (M4) and NLA (M2) come later.

#include <farland/auth/tls_identity.hpp>
#include <farland/base/log.hpp>

#include "session.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

namespace log = farland::log;
using farland::app::SessionOptions;
constexpr std::string_view log_component = "app";

std::atomic<bool> stop_requested{
    false};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): signal handler state
std::atomic<int> active_sessions{0};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

extern "C" void on_signal(int /*signal*/)
{
    stop_requested.store(true);
}

struct Options {
    std::string bind = "0.0.0.0";
    std::string port = "3389";
    std::filesystem::path cert;
    std::filesystem::path key;
    std::string hostname;
    bool print_fingerprint = false;
    int max_sessions = 4;
    SessionOptions session;
};

std::filesystem::path config_dir()
{
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "farland";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".config" / "farland";
    }
    return std::filesystem::current_path() / ".farland";
}

std::string local_hostname()
{
    std::string name(256, '\0');
    if (::gethostname(name.data(), name.size()) != 0) {
        return "localhost";
    }
    name.resize(name.find('\0'));
    return name.empty() ? "localhost" : name;
}

void usage()
{
    std::cout << "usage: farland-server [options]\n"
                 "  --bind ADDRESS        listen address (default 0.0.0.0)\n"
                 "  --port PORT           TCP port (default 3389)\n"
                 "  --cert FILE --key FILE  TLS certificate and key (PEM); created if both are missing\n"
                 "                        (default: $XDG_CONFIG_HOME/farland/tls/{cert,key}.pem)\n"
                 "  --hostname NAME       certificate host name (default: this host's name)\n"
                 "  --fps N               frame rate of the test pattern (default 30)\n"
                 "  --codec planar|raw    bitmap codec for 32 bpp sessions (default planar)\n"
                 "  --max-sessions N      concurrent connections (default 4)\n"
                 "  --log-level LEVEL     trace, debug, info, warn, error (default info)\n"
                 "  --fingerprint         print the certificate's SHA-256 fingerprint and exit\n";
}

bool parse_options(std::span<char*> args, Options& options)
{
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= args.size()) {
                throw std::runtime_error(std::string(arg) + " needs a value");
            }
            return args[++i];
        };
        if (arg == "--bind") {
            options.bind = value();
        } else if (arg == "--port") {
            options.port = value();
        } else if (arg == "--cert") {
            options.cert = value();
        } else if (arg == "--key") {
            options.key = value();
        } else if (arg == "--hostname") {
            options.hostname = value();
        } else if (arg == "--fps") {
            options.session.frames_per_second = static_cast<unsigned>(std::stoul(value()));
        } else if (arg == "--codec") {
            const auto codec = value();
            if (codec != "planar" && codec != "raw") {
                throw std::runtime_error("--codec must be planar or raw");
            }
            options.session.codec =
                codec == "planar" ? farland::server::BitmapCodec::planar : farland::server::BitmapCodec::uncompressed;
        } else if (arg == "--max-sessions") {
            options.max_sessions = std::stoi(value());
        } else if (arg == "--log-level") {
            const auto level = value();
            static constexpr std::array levels{"trace", "debug", "info", "warn", "error"};
            const auto* found = std::ranges::find(levels, level);
            if (found == levels.end()) {
                throw std::runtime_error("unknown log level " + level);
            }
            log::set_level(static_cast<log::Level>(found - levels.begin()));
        } else if (arg == "--fingerprint") {
            options.print_fingerprint = true;
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return false;
        } else {
            throw std::runtime_error("unknown option " + std::string(arg));
        }
    }
    if (options.cert.empty() != options.key.empty()) {
        throw std::runtime_error("--cert and --key go together");
    }
    if (options.cert.empty()) {
        options.cert = config_dir() / "tls" / "cert.pem";
        options.key = config_dir() / "tls" / "key.pem";
    }
    if (options.hostname.empty()) {
        options.hostname = local_hostname();
    }
    return true;
}

int listen_on(const Options& options)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* result = nullptr;
    if (const int rc = ::getaddrinfo(options.bind.c_str(), options.port.c_str(), &hints, &result); rc != 0) {
        throw std::runtime_error("cannot resolve " + options.bind + ": " + ::gai_strerror(rc));
    }
    const int fd = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    const int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    const bool ok = fd >= 0 && ::bind(fd, result->ai_addr, result->ai_addrlen) == 0 && ::listen(fd, 16) == 0;
    ::freeaddrinfo(result);
    if (!ok) {
        throw std::runtime_error("cannot listen on " + options.bind + ":" + options.port + ": " + std::strerror(errno));
    }
    return fd;
}

std::string peer_name(const sockaddr_storage& address)
{
    std::string host(NI_MAXHOST, '\0');
    std::string port(NI_MAXSERV, '\0');
    if (::getnameinfo(reinterpret_cast<const sockaddr*>(&address), sizeof(address), host.data(),
                      static_cast<socklen_t>(host.size()), port.data(), static_cast<socklen_t>(port.size()),
                      NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return "unknown peer";
    }
    host.resize(host.find('\0'));
    port.resize(port.find('\0'));
    return host + ":" + port;
}

}  // namespace

int main(int argc, char** argv)
{
    Options options;
    try {
        if (!parse_options(std::span(argv, static_cast<std::size_t>(argc)), options)) {
            return 0;
        }
    } catch (const std::exception& e) {
        std::cerr << "farland-server: " << e.what() << "\n";
        usage();
        return 2;
    }

    auto identity = farland::auth::TlsIdentity::load_or_create(options.cert, options.key, options.hostname);
    if (!identity) {
        std::cerr << "farland-server: cannot load or create the TLS identity: " << identity.error().message() << "\n";
        return 1;
    }
    if (options.print_fingerprint) {
        std::cout << identity->sha256_fingerprint() << "\n";
        return 0;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    int listener = -1;
    try {
        listener = listen_on(options);
    } catch (const std::exception& e) {
        std::cerr << "farland-server: " << e.what() << "\n";
        return 1;
    }
    log::info(log_component, "listening on {}:{}, certificate SHA-256 {}", options.bind, options.port,
              identity->sha256_fingerprint());

    while (!stop_requested.load()) {
        pollfd pfd{listener, POLLIN, 0};
        if (::poll(&pfd, 1, 500) <= 0) {
            continue;
        }
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        const int fd = ::accept(listener, reinterpret_cast<sockaddr*>(&address), &length);
        if (fd < 0) {
            continue;
        }
        const std::string peer = peer_name(address);
        if (active_sessions.load() >= options.max_sessions) {
            log::warn(log_component, "{}: refused, {} sessions already running", peer, options.max_sessions);
            ::close(fd);
            continue;
        }
        log::info(log_component, "{}: connected", peer);
        ++active_sessions;
        std::thread([fd, peer, &identity, &options] {
            farland::app::run_session(fd, peer, *identity, options.session, stop_requested);
            --active_sessions;
        }).detach();
    }

    log::info(log_component, "shutting down");
    ::close(listener);
    while (active_sessions.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return 0;
}
