// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "transport.hpp"

#include <farland/base/log.hpp>

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace farland::app {

namespace {

constexpr std::string_view log_component = "app.transport";
constexpr std::size_t receive_buffer_size = 64 * 1024;
#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL;  // a vanished peer must not raise SIGPIPE
#else
constexpr int send_flags = 0;  // macOS: SO_NOSIGPIPE is set on the socket instead
#endif

/// Reads once into `buffer`. The count, 0 for "nothing yet", or nullopt when
/// the peer is gone.
std::optional<std::size_t> receive_some(int fd, std::vector<std::byte>& buffer)
{
    const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (received > 0) {
        return static_cast<std::size_t>(received);
    }
    if (received < 0 && (errno == EINTR || would_block(errno))) {
        return 0;
    }
    return std::nullopt;
}

}  // namespace

bool would_block(int error) noexcept
{
#if EAGAIN == EWOULDBLOCK
    return error == EAGAIN;
#else
    return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

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
        if (sent < 0 && would_block(errno)) {
            pollfd pfd{fd, POLLOUT, 0};
            ::poll(&pfd, 1, 1000);
            continue;
        }
        return false;
    }
    return true;
}

void prepare_socket(int fd)
{
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    const int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
}

// NetworkStage ---------------------------------------------------------------------

NetworkStage::NetworkStage(int fd, std::string peer, const auth::TlsIdentity& identity,
                           const server::PreAuthConfig& config, server::PreAuth::NlaFactory make_nla)
    : fd_(fd), peer_(std::move(peer)), identity_(identity), preauth_(config, std::move(make_nla)),
      buffer_(receive_buffer_size)
{
}

NetworkStage::~NetworkStage()
{
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

bool NetworkStage::read(std::vector<std::byte>& data)
{
    const auto received = receive_some(fd_, buffer_);
    if (!received) {
        log::info(log_component, "{}: connection closed by peer", peer_);
        return false;
    }
    const auto bytes = std::span(buffer_).first(*received);
    if (bytes.empty()) {
        return true;
    }
    if (!tls_) {
        preauth_.receive(bytes);
        return pump(data);
    }
    tls_->receive(bytes);
    std::vector<std::byte> plaintext;
    const auto processed = tls_->process(plaintext);
    if (!send_all(fd_, tls_->take_ciphertext())) {
        return false;
    }
    if (!processed) {
        log::warn(log_component, "{}: TLS failed: {}", peer_, tls_->last_error());
        return false;
    }
    if (!tls_notified_ && tls_->handshake_complete()) {
        tls_notified_ = true;
        log::info(log_component, "{}: {} with {}", peer_, tls_->protocol_version(), tls_->cipher());
        preauth_.tls_established();
        if (!pump(data)) {
            return false;
        }
    }
    if (!plaintext.empty()) {
        if (ready_) {
            data.insert(data.end(), plaintext.begin(), plaintext.end());
        } else {
            preauth_.receive(plaintext);
        }
    }
    if (!pump(data)) {
        return false;
    }
    return !tls_->peer_closed();
}

bool NetworkStage::pump(std::vector<std::byte>& data)
{
    if (ready_) {
        return true;
    }
    if (!send(preauth_.take_output())) {
        return false;
    }
    bool ok = true;
    while (auto event = preauth_.poll_event()) {
        std::visit(
            [&](auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, server::preauth_event::StartTls>) {
                    tls_.emplace(identity_);
                } else if constexpr (std::is_same_v<T, server::preauth_event::Authenticated>) {
                    log::info(log_component, "{}: NLA user '{}'{}{}", peer_, e.identity.user,
                              e.identity.domain.empty() ? "" : " in domain '" + e.identity.domain + "'",
                              e.credentials ? " (credentials delegated)" : "");
                } else if constexpr (std::is_same_v<T, server::preauth_event::Ready>) {
                    ready_ = true;
                    const auto rest = preauth_.take_remaining_input();
                    data.insert(data.end(), rest.begin(), rest.end());
                } else if constexpr (std::is_same_v<T, server::preauth_event::Failed>) {
                    log::info(log_component, "{}: pre-authentication failed: {}", peer_, e.reason);
                    ok = false;
                }
            },
            *event);
    }
    return ok;
}

bool NetworkStage::send(std::span<const std::byte> data)
{
    if (data.empty()) {
        return true;
    }
    if (tls_ && tls_->handshake_complete()) {
        tls_->send(data);
        return send_all(fd_, tls_->take_ciphertext());
    }
    return send_all(fd_, data);
}

void NetworkStage::close()
{
    if (fd_ < 0) {
        return;
    }
    if (tls_ && tls_->handshake_complete() && !tls_->failed()) {
        tls_->close();
        (void)send_all(fd_, tls_->take_ciphertext());
    }
    ::close(fd_);
    fd_ = -1;
}

// PlainTransport -------------------------------------------------------------------

PlainTransport::PlainTransport(int fd, server::Negotiation negotiation)
    : fd_(fd), negotiation_(std::move(negotiation)), buffer_(receive_buffer_size)
{
}

PlainTransport::~PlainTransport()
{
    close();
}

bool PlainTransport::read(std::vector<std::byte>& data)
{
    const auto received = receive_some(fd_, buffer_);
    if (!received) {
        return false;
    }
    const auto bytes = std::span(buffer_).first(*received);
    data.insert(data.end(), bytes.begin(), bytes.end());
    return true;
}

bool PlainTransport::send(std::span<const std::byte> data)
{
    return data.empty() || send_all(fd_, data);
}

void PlainTransport::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace farland::app
