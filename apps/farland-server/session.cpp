// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "session.hpp"

#include <farland/auth/tls.hpp>
#include <farland/base/log.hpp>
#include <farland/server/connection.hpp>
#include <farland/server/test_pattern.hpp>

#include <cerrno>
#include <chrono>
#include <optional>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace farland::app {

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::string_view log_component = "app.session";
constexpr std::size_t receive_buffer_size = 64 * 1024;
#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL;  // a vanished peer must not raise SIGPIPE
#else
constexpr int send_flags = 0;  // macOS: SO_NOSIGPIPE is set on the socket instead
#endif

/// Writes everything, waiting for the socket to drain. False on error.
bool send_all(int fd, std::span<const std::byte> data)
{
    while (!data.empty()) {
        const ssize_t sent = ::send(fd, data.data(), data.size(), send_flags);
        if (sent > 0) {
            data = data.subspan(static_cast<std::size_t>(sent));
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pfd{fd, POLLOUT, 0};
            ::poll(&pfd, 1, 1000);
            continue;
        }
        return false;
    }
    return true;
}

class SessionRunner {
public:
    SessionRunner(int fd, std::string peer, const auth::TlsIdentity& identity, const SessionOptions& options)
        : fd_(fd), peer_(std::move(peer)), identity_(identity), options_(options),
          frame_interval_(std::chrono::microseconds(1'000'000 / std::max(options.frames_per_second, 1U)))
    {
    }

    void run(const std::atomic<bool>& stop)
    {
        const auto started = Clock::now();
        std::vector<std::byte> buffer(receive_buffer_size);
        while (running_) {
            if (stop.load()) {
                connection_.disconnect(proto::errinfo::rpc_initiated_disconnect);
                pump();
                break;
            }
            if (!connection_.active() && Clock::now() - started > std::chrono::seconds(options_.activation_timeout)) {
                log::warn(log_component, "{}: no active connection after {} s, dropping it", peer_,
                          options_.activation_timeout);
                break;
            }
            pollfd pfd{fd_, POLLIN, 0};
            ::poll(&pfd, 1, poll_timeout_ms());
            if ((pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                const ssize_t received = ::recv(fd_, buffer.data(), buffer.size(), 0);
                if (received == 0 || (received < 0 && errno != EINTR && errno != EAGAIN)) {
                    log::info(log_component, "{}: connection closed by peer", peer_);
                    break;
                }
                if (received > 0) {
                    on_network(std::span(buffer).first(static_cast<std::size_t>(received)));
                }
            }
            if (running_) {
                send_frame_if_due();
            }
        }
        if (tls_ && tls_->handshake_complete()) {
            tls_->close();
            send_all(fd_, tls_->take_ciphertext());
        }
        ::close(fd_);
    }

private:
    int poll_timeout_ms() const
    {
        if (!connection_.active() || suppressed_) {
            return 250;
        }
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_frame_ - Clock::now()).count();
        return static_cast<int>(std::clamp<std::int64_t>(wait, 0, 250));
    }

    void on_network(std::span<const std::byte> bytes)
    {
        if (!tls_) {
            connection_.receive(bytes);
            pump();
            return;
        }
        tls_->receive(bytes);
        std::vector<std::byte> plaintext;
        const auto processed = tls_->process(plaintext);
        if (!send_all(fd_, tls_->take_ciphertext())) {
            running_ = false;
            return;
        }
        if (!processed) {
            log::warn(log_component, "{}: TLS failed: {}", peer_, tls_->last_error());
            running_ = false;
            return;
        }
        if (!tls_notified_ && tls_->handshake_complete()) {
            tls_notified_ = true;
            log::info(log_component, "{}: {} with {}", peer_, tls_->protocol_version(), tls_->cipher());
            connection_.tls_established();
        }
        if (!plaintext.empty()) {
            connection_.receive(plaintext);
        }
        pump();
        if (tls_->peer_closed()) {
            running_ = false;
        }
    }

    /// Output first, then events (server::Connection's contract).
    void pump()
    {
        const auto output = connection_.take_output();
        if (!output.empty()) {
            if (tls_ && tls_->handshake_complete()) {
                tls_->send(output);
                if (!send_all(fd_, tls_->take_ciphertext())) {
                    running_ = false;
                }
            } else if (!send_all(fd_, output)) {
                running_ = false;
            }
        }
        while (auto event = connection_.poll_event()) {
            std::visit([this](auto& e) { on_event(e); }, *event);
        }
    }

    void on_event(server::event::StartTls& /*e*/) { tls_.emplace(identity_); }

    void on_event(server::event::ClientInfo& e)
    {
        log::info(log_component, "{}: user '{}'{}", peer_, e.user_name, e.password.empty() ? "" : " (password sent)");
    }

    void on_event(server::event::Activated& e)
    {
        const auto& session = connection_.session();
        const auto codec = session.bits_per_pixel == 32 ? options_.codec : server::BitmapCodec::uncompressed;
        if (!pattern_) {
            pattern_.emplace(session.desktop_width, session.desktop_height);
        } else {
            pattern_->resize(session.desktop_width, session.desktop_height);
        }
        encoder_.emplace(session.desktop_width, session.desktop_height, session.bits_per_pixel, codec,
                         session.no_bitmap_compression_header);
        next_frame_ = Clock::now();
        log::info(log_component, "{}: {} {}x{}, {} bitmaps", peer_, e.reactivation ? "reactivated" : "active",
                  session.desktop_width, session.desktop_height,
                  codec == server::BitmapCodec::planar ? "planar" : "uncompressed");
    }

    void on_event(server::event::Input& e)
    {
        for (const auto& input : e.events) {
            if (pattern_) {
                pattern_->apply(input);
            }
            log::debug(log_component, "{}: input {}", peer_, server::TestPattern::describe(input));
        }
    }

    void on_event(server::event::RefreshRequested& e)
    {
        if (encoder_) {
            for (const auto& area : e.areas) {
                encoder_->invalidate(area);
            }
        }
    }

    void on_event(server::event::OutputSuppressed& e)
    {
        suppressed_ = e.suppressed;
        if (!suppressed_ && encoder_) {
            encoder_->invalidate_all();
        }
    }

    void on_event(server::event::ChannelData& e)
    {
        log::debug(log_component, "{}: {} bytes on channel {} (no channel handlers yet)", peer_, e.data.size(),
                   e.channel_id);
    }

    void on_event(server::event::ShutdownRequested& /*e*/)
    {
        log::info(log_component, "{}: client requested shutdown", peer_);
        connection_.disconnect(proto::errinfo::none);
    }

    void on_event(server::event::Closed& e)
    {
        log::info(log_component, "{}: {}{}", peer_, e.error ? "protocol error: " : "", e.reason);
        running_ = false;
    }

    void send_frame_if_due()
    {
        if (!connection_.active() || suppressed_ || !encoder_ || !pattern_) {
            return;
        }
        const auto now = Clock::now();
        if (now < next_frame_) {
            return;
        }
        const auto image = pattern_->render(frame_++);
        for (const auto& update : encoder_->encode(image, connection_.max_update_size())) {
            connection_.send_bitmap_update(update);
        }
        pump();
        next_frame_ += frame_interval_;
        if (next_frame_ < now) {
            next_frame_ = now + frame_interval_;  // fell behind: drop frames rather than burst
        }
    }

    int fd_;
    std::string peer_;
    const auth::TlsIdentity& identity_;
    SessionOptions options_;
    Clock::duration frame_interval_;
    server::Connection connection_;
    std::optional<auth::TlsServer> tls_;
    bool tls_notified_ = false;
    bool running_ = true;
    bool suppressed_ = false;
    std::optional<server::TestPattern> pattern_;
    std::optional<server::FrameEncoder> encoder_;
    std::uint64_t frame_ = 0;
    Clock::time_point next_frame_;
};

}  // namespace

void run_session(int fd, std::string peer, const auth::TlsIdentity& identity, const SessionOptions& options,
                 const std::atomic<bool>& stop)
{
#ifdef SO_NOSIGPIPE
    const int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
    SessionRunner runner(fd, std::move(peer), identity, options);
    runner.run(stop);
}

}  // namespace farland::app
