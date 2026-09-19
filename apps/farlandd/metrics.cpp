// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "metrics.hpp"

#include <farland/base/log.hpp>
#include <farland/base/unique_fd.hpp>

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <charconv>
#include <cstring>
#include <format>
#include <netinet/in.h>
#include <poll.h>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace farland::daemon {

namespace {

constexpr std::string_view log_component = "daemon.metrics";
/// A scrape is a request line and a few headers; anything longer is not one.
constexpr std::size_t max_request = 4096;
/// How long one scrape may take from accept to close.
constexpr int scrape_timeout_ms = 5000;

/// A label value in the text format: backslash, double quote and newline are
/// the three that have to be escaped (Prometheus exposition format 0.0.4).
[[nodiscard]] std::string escaped(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

void metric(std::string& out, std::string_view name, std::string_view help, std::string_view type)
{
    out += std::format("# HELP {} {}\n# TYPE {} {}\n", name, help, name, type);
}

/// The labels every per-session metric carries.
[[nodiscard]] std::string labels_of(const SessionMetrics& s)
{
    return std::format("{{session=\"{}\",account=\"{}\"}}", s.id, escaped(s.account));
}

}  // namespace

std::string render_metrics(const MetricsSnapshot& snapshot)
{
    std::string out;

    metric(out, "farland_sessions", "Sessions farlandd is running, by state.", "gauge");
    for (const std::string_view state : {"starting", "running", "ending"}) {
        const auto count =
            std::ranges::count_if(snapshot.sessions, [state](const SessionMetrics& s) { return s.state == state; });
        out += std::format("farland_sessions{{state=\"{}\"}} {}\n", state, count);
    }

    metric(out, "farland_sessions_connected", "Sessions with a client connected.", "gauge");
    out += std::format("farland_sessions_connected {}\n",
                       std::ranges::count_if(snapshot.sessions, &SessionMetrics::connected));

    metric(out, "farland_connections_total", "Connections handed to a session since farlandd started.", "counter");
    out += std::format("farland_connections_total {}\n", snapshot.connections_total);

    metric(out, "farland_sessions_started_total", "Sessions started since farlandd started.", "counter");
    out += std::format("farland_sessions_started_total {}\n", snapshot.sessions_started_total);

    metric(out, "farland_sessions_ended_total", "Sessions ended, by reason.", "counter");
    for (const auto& [reason, count] : snapshot.sessions_ended_total) {
        out += std::format("farland_sessions_ended_total{{reason=\"{}\"}} {}\n", escaped(reason), count);
    }

    metric(out, "farland_connections_refused_total", "Connections refused, by reason.", "counter");
    for (const auto& [reason, count] : snapshot.connections_refused_total) {
        out += std::format("farland_connections_refused_total{{reason=\"{}\"}} {}\n", escaped(reason), count);
    }

    metric(out, "farland_session_uptime_seconds", "Seconds since the session started.", "gauge");
    for (const auto& s : snapshot.sessions) {
        out += std::format("farland_session_uptime_seconds{} {}\n", labels_of(s), s.uptime_seconds);
    }
    metric(out, "farland_session_idle_seconds", "Seconds since the client last sent input.", "gauge");
    for (const auto& s : snapshot.sessions) {
        out += std::format("farland_session_idle_seconds{} {}\n", labels_of(s), s.idle_seconds);
    }
    metric(out, "farland_session_frames_sent_total", "Graphics frames sent to the session's clients.", "counter");
    for (const auto& s : snapshot.sessions) {
        out += std::format("farland_session_frames_sent_total{} {}\n", labels_of(s), s.frames_sent);
    }
    metric(out, "farland_session_bytes_sent_total", "Bytes sent to the session's clients.", "counter");
    for (const auto& s : snapshot.sessions) {
        out += std::format("farland_session_bytes_sent_total{} {}\n", labels_of(s), s.bytes_sent);
    }
    metric(out, "farland_session_bytes_received_total", "Bytes received from the session's clients.", "counter");
    for (const auto& s : snapshot.sessions) {
        out += std::format("farland_session_bytes_received_total{} {}\n", labels_of(s), s.bytes_received);
    }
    metric(out, "farland_session_round_trip_seconds", "Round trip to the client; 0 when not measured.", "gauge");
    for (const auto& s : snapshot.sessions) {
        out += std::format("farland_session_round_trip_seconds{} {:.3f}\n", labels_of(s),
                           static_cast<double>(s.rtt_ms) / 1000.0);
    }
    metric(out, "farland_session_bandwidth_bits_per_second", "Bandwidth auto-detect measured; 0 when not measured.",
           "gauge");
    for (const auto& s : snapshot.sessions) {
        out += std::format("farland_session_bandwidth_bits_per_second{} {}\n", labels_of(s),
                           std::uint64_t{s.bandwidth_kbps} * 1000);
    }
    return out;
}

void Metrics::publish(std::vector<SessionMetrics> sessions)
{
    const std::scoped_lock lock(mutex_);
    state_.sessions = std::move(sessions);
}

void Metrics::connection()
{
    const std::scoped_lock lock(mutex_);
    ++state_.connections_total;
}

void Metrics::session_started()
{
    const std::scoped_lock lock(mutex_);
    ++state_.sessions_started_total;
}

void Metrics::session_ended(std::string reason)
{
    const std::scoped_lock lock(mutex_);
    ++state_.sessions_ended_total[std::move(reason)];
}

void Metrics::connection_refused(std::string reason)
{
    const std::scoped_lock lock(mutex_);
    ++state_.connections_refused_total[std::move(reason)];
}

MetricsSnapshot Metrics::snapshot() const
{
    const std::scoped_lock lock(mutex_);
    return state_;
}

Result<std::pair<std::string, std::uint16_t>> split_listen_address(const std::string& address)
{
    std::string host;
    std::string port;
    if (address.starts_with('[')) {
        const auto close = address.find("]:");
        if (close == std::string::npos) {
            return fail(Errc::invalid_value, "an IPv6 listen address is [address]:port");
        }
        host = address.substr(1, close - 1);
        port = address.substr(close + 2);
    } else {
        const auto colon = address.rfind(':');
        if (colon == std::string::npos) {
            return fail(Errc::invalid_value, "a listen address is host:port");
        }
        host = address.substr(0, colon);
        port = address.substr(colon + 1);
        // "::1:9128" is itself a valid IPv6 address, so there is no telling
        // which colon separates the port. Brackets are how that is said.
        if (host.contains(':')) {
            return fail(Errc::invalid_value, "an IPv6 listen address is [address]:port");
        }
    }
    std::uint32_t number = 0;
    const auto* end = port.data() + port.size();
    const auto parsed = std::from_chars(port.data(), end, number);
    if (parsed.ec != std::errc{} || parsed.ptr != end || number == 0 || number > 65535) {
        return fail(Errc::invalid_value, "the port of a listen address is 1 to 65535");
    }
    return std::pair{host, static_cast<std::uint16_t>(number)};
}

struct MetricsServer::Impl {
    explicit Impl(const Metrics& source) : metrics(&source) {}
    ~Impl()
    {
        // The thread polls the listener with a timeout, so it notices `stop`
        // on its own within a quarter of a second. It has to be joined before
        // the socket is closed: closing an fd another thread is polling is a
        // race with whatever opens the next one.
        stop = true;
        if (thread.joinable()) {
            thread.join();
        }
        listener.reset();
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    const Metrics* metrics;
    UniqueFd listener;
    std::thread thread;
    std::atomic<bool> stop{false};
};

namespace {

/// Reads the request line, answers it and closes. Nothing here keeps state
/// between connections, and nothing here parses more than it has to.
void serve_one(int fd, const Metrics& metrics)
{
    std::string request;
    const auto deadline_poll = [fd](int timeout) {
        pollfd p{fd, POLLIN, 0};
        return ::poll(&p, 1, timeout) > 0 && (p.revents & POLLIN) != 0;
    };
    while (request.size() < max_request && request.find("\r\n") == std::string::npos) {
        if (!deadline_poll(scrape_timeout_ms)) {
            return;
        }
        std::array<char, 1024> chunk{};
        const auto got = ::read(fd, chunk.data(), chunk.size());
        if (got <= 0) {
            return;
        }
        request.append(chunk.data(), static_cast<std::size_t>(got));
    }
    const auto line_end = request.find("\r\n");
    const std::string_view line =
        std::string_view(request).substr(0, line_end == std::string::npos ? request.size() : line_end);

    std::string body;
    std::string status;
    if (line.starts_with("GET /metrics ") || line == "GET /metrics") {
        status = "200 OK";
        body = render_metrics(metrics.snapshot());
    } else if (line.starts_with("GET ")) {
        status = "404 Not Found";
        body = "farlandd serves /metrics\n";
    } else {
        status = "400 Bad Request";
        body = "farlandd serves GET /metrics\n";
    }
    const std::string answer = std::format("HTTP/1.1 {}\r\nContent-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
                                           "Content-Length: {}\r\nConnection: close\r\n\r\n{}",
                                           status, body.size(), body);
    std::size_t sent = 0;
    while (sent < answer.size()) {
        const auto wrote = ::write(fd, answer.data() + sent, answer.size() - sent);
        if (wrote <= 0) {
            return;
        }
        sent += static_cast<std::size_t>(wrote);
    }
}

}  // namespace

MetricsServer::MetricsServer(const Metrics& metrics) : impl_(std::make_unique<Impl>(metrics)) {}

MetricsServer::~MetricsServer() = default;

Result<void> MetricsServer::start(const std::string& address)
{
    FARLAND_TRY(const auto where, split_listen_address(address));
    const auto& [host, port] = where;
    const bool v6 = host.find(':') != std::string::npos || host.empty();
    UniqueFd fd(::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd.valid()) {
        return fail(Errc::io, "cannot create the metrics socket");
    }
    int on = 1;
    static_cast<void>(::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)));
    int bound = -1;
    if (v6) {
        sockaddr_in6 in6{};
        in6.sin6_family = AF_INET6;
        in6.sin6_port = htons(port);
        in6.sin6_addr = in6addr_any;
        if (!host.empty() && ::inet_pton(AF_INET6, host.c_str(), &in6.sin6_addr) != 1) {
            return fail(Errc::invalid_value, "the metrics listen address is not an address");
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API takes a generic sockaddr
        bound = ::bind(fd.get(), reinterpret_cast<const sockaddr*>(&in6), sizeof(in6));
    } else {
        sockaddr_in in{};
        in.sin_family = AF_INET;
        in.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &in.sin_addr) != 1) {
            return fail(Errc::invalid_value, "the metrics listen address is not an address");
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API takes a generic sockaddr
        bound = ::bind(fd.get(), reinterpret_cast<const sockaddr*>(&in), sizeof(in));
    }
    if (bound != 0 || ::listen(fd.get(), 4) != 0) {
        return fail(Errc::io, "cannot listen on the metrics address");
    }
    impl_->listener = std::move(fd);
    impl_->thread = std::thread([impl = impl_.get()] {
        while (!impl->stop.load()) {
            pollfd p{impl->listener.get(), POLLIN, 0};
            if (::poll(&p, 1, 250) <= 0 || (p.revents & POLLIN) == 0) {
                continue;
            }
            UniqueFd client(::accept4(impl->listener.get(), nullptr, nullptr, SOCK_CLOEXEC));
            if (!client.valid()) {
                continue;
            }
            serve_one(client.get(), *impl->metrics);
        }
    });
    log::info(log_component, "serving metrics on http://{}/metrics", address);
    return {};
}

}  // namespace farland::daemon
