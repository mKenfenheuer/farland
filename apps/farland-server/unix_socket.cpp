// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "unix_socket.hpp"

#include <farland/base/assert.hpp>
#include <farland/base/log.hpp>
#include <farland/base/reader.hpp>

#include "transport.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

namespace farland::app {

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::string_view log_component = "app.unix";
constexpr std::size_t length_prefix = 4;
/// Room for more descriptors than a message may carry, so that extra ones
/// are received (and closed) rather than silently dropped by the kernel.
constexpr std::size_t max_fds_per_read = 4;

#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;  // macOS: SO_NOSIGPIPE, set in send_message
#endif
#ifdef MSG_CMSG_CLOEXEC
constexpr int receive_flags = MSG_CMSG_CLOEXEC;
#else
constexpr int receive_flags = 0;  // macOS: FD_CLOEXEC right after recvmsg
#endif

/// Waits until `socket` is ready for `events`, or the deadline passes.
Result<void> wait_for(int socket, short events, std::optional<Clock::time_point> deadline)
{
    while (true) {
        int timeout = -1;
        if (deadline) {
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(*deadline - Clock::now()).count();
            if (left <= 0) {
                return fail(Errc::io, "timed out on a local socket");
            }
            timeout = static_cast<int>(std::min<std::int64_t>(left, std::numeric_limits<int>::max()));
        }
        pollfd pfd{socket, events, 0};
        const int rc = ::poll(&pfd, 1, timeout);
        if (rc > 0) {
            return {};
        }
        if (rc < 0 && errno != EINTR) {
            log::debug(log_component, "poll: {}", std::strerror(errno));
            return fail(Errc::io, "cannot wait on a local socket");
        }
    }
}

/// One recvmsg into `into`. A descriptor that arrives is stored in `fd`; a
/// second one for the same message is an error. Returns the byte count (0 at
/// end of file).
Result<std::size_t> receive_some(int socket, std::span<std::byte> into, UniqueFd& fd,
                                 std::optional<Clock::time_point> deadline)
{
    while (true) {
        if (deadline) {
            FARLAND_TRY_VOID(wait_for(socket, POLLIN, deadline));
        }
        iovec iov{into.data(), into.size()};
        alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * max_fds_per_read)> control{};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control.data();
        msg.msg_controllen = static_cast<decltype(msg.msg_controllen)>(control.size());
        const ssize_t received = ::recvmsg(socket, &msg, receive_flags);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (would_block(errno)) {
                FARLAND_TRY_VOID(wait_for(socket, POLLIN, deadline));
                continue;
            }
            log::debug(log_component, "recvmsg: {}", std::strerror(errno));
            return fail(Errc::io, "cannot receive on a local socket");
        }

        // Own every descriptor first, so that none leaks on an error below.
        std::vector<UniqueFd> fds;
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
                continue;
            }
            const std::size_t count = (static_cast<std::size_t>(c->cmsg_len) - CMSG_LEN(0)) / sizeof(int);
            const unsigned char* data = CMSG_DATA(c);
            for (std::size_t i = 0; i < count; ++i) {
                int value = -1;
                // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): CMSG_DATA is a raw pointer; i <
                // count, which cmsg_len bounds
                std::memcpy(&value, data + (i * sizeof(int)), sizeof(int));
                fds.emplace_back(value);
            }
        }
        if ((static_cast<unsigned>(msg.msg_flags) & static_cast<unsigned>(MSG_CTRUNC)) != 0) {
            return fail(Errc::limit_exceeded, "control data truncated; descriptors were dropped");
        }
        if (!fds.empty()) {
            if (fds.size() > 1 || fd.valid()) {
                return fail(Errc::invalid_value, "more than one descriptor with a message");
            }
#ifndef MSG_CMSG_CLOEXEC
            ::fcntl(fds.front().get(), F_SETFD, FD_CLOEXEC);
#endif
            fd = std::move(fds.front());
        }
        return static_cast<std::size_t>(received);
    }
}

}  // namespace

Result<void> send_message(int socket, std::span<const std::byte> frame, int fd)
{
    FARLAND_ASSERT(frame.size() > length_prefix);
#ifdef SO_NOSIGPIPE
    const int one = 1;
    ::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    std::size_t sent = 0;
    while (sent < frame.size()) {
        const auto rest = frame.subspan(sent);
        // sendmsg takes a non-const iovec but only reads through it.
        iovec iov{const_cast<std::byte*>(rest.data()), rest.size()};  // NOLINT(cppcoreguidelines-pro-type-const-cast)
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control{};
        // The descriptor goes with the first sendmsg that transfers anything;
        // one that failed before sending a byte did not send it either.
        if (fd >= 0 && sent == 0) {
            msg.msg_control = control.data();
            msg.msg_controllen = static_cast<decltype(msg.msg_controllen)>(control.size());
            cmsghdr* c = CMSG_FIRSTHDR(&msg);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = static_cast<decltype(c->cmsg_len)>(CMSG_LEN(sizeof(int)));
            std::memcpy(CMSG_DATA(c), &fd, sizeof(int));
        }
        const ssize_t written = ::sendmsg(socket, &msg, send_flags);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (would_block(errno)) {
                FARLAND_TRY_VOID(wait_for(socket, POLLOUT, std::nullopt));
                continue;
            }
            log::debug(log_component, "sendmsg: {}", std::strerror(errno));
            return fail(Errc::io, "cannot send on a local socket");
        }
        sent += static_cast<std::size_t>(written);
    }
    return {};
}

Result<std::optional<ReceivedMessage>> receive_message(int socket, std::size_t max_body, int timeout_ms)
{
    std::optional<Clock::time_point> deadline;
    if (timeout_ms >= 0) {
        deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    }
    ReceivedMessage message;
    message.frame.resize(length_prefix);
    std::size_t have = 0;
    bool sized = false;
    while (true) {
        if (!sized && have == length_prefix) {
            Reader prefix(message.frame);
            FARLAND_TRY(const std::uint32_t length, prefix.u32le());
            if (length == 0 || length > max_body) {
                return fail(Errc::limit_exceeded, "local message size out of range");
            }
            message.frame.resize(length_prefix + length);
            sized = true;
        }
        if (sized && have == message.frame.size()) {
            return std::optional<ReceivedMessage>(std::move(message));
        }
        FARLAND_TRY(const std::size_t received,
                    receive_some(socket, std::span(message.frame).subspan(have), message.fd, deadline));
        if (received == 0) {
            if (have == 0 && !message.fd.valid()) {
                return std::optional<ReceivedMessage>{};
            }
            return fail(Errc::truncated, "local socket closed inside a message");
        }
        have += received;
    }
}

Result<PeerCredentials> peer_credentials(int socket)
{
#ifdef __linux__
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    if (::getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 || size != sizeof(credentials)) {
        log::debug(log_component, "SO_PEERCRED: {}", std::strerror(errno));
        return fail(Errc::io, "cannot read the peer credentials of a local socket");
    }
    return PeerCredentials{credentials.uid, credentials.gid, credentials.pid};
#else
    PeerCredentials peer;
    if (::getpeereid(socket, &peer.uid, &peer.gid) != 0) {
        log::debug(log_component, "getpeereid: {}", std::strerror(errno));
        return fail(Errc::io, "cannot read the peer credentials of a local socket");
    }
#ifdef LOCAL_PEERPID
    pid_t pid = 0;
    socklen_t size = sizeof(pid);
    if (::getsockopt(socket, SOL_LOCAL, LOCAL_PEERPID, &pid, &size) == 0 && size == sizeof(pid)) {
        peer.pid = pid;
    }
#endif
    return peer;
#endif
}

}  // namespace farland::app
