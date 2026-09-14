// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/unique_fd.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <sys/types.h>
#include <vector>

/// Local control sockets between farland's processes (farlandd, the network
/// process, farland-agent): framed messages that may carry one descriptor
/// (SCM_RIGHTS), and the credentials of the process at the other end.
///
/// The framing is the one privsep and the broker protocol share: a u32le
/// length of what follows, then the message. A descriptor travels with the
/// first byte of its message. The receiver reads exactly one message at a
/// time, never past its end, so a descriptor that arrives always belongs to
/// the message being read: a stream socket may otherwise hand back the tail
/// of one message together with the start (and descriptor) of the next.
namespace farland::app {

struct ReceivedMessage {
    /// The whole frame, length prefix included (what broker::decode takes).
    std::vector<std::byte> frame;
    /// The descriptor that came with it, close-on-exec; invalid if none did.
    UniqueFd fd;
};

/// Sends one framed message (`frame` starts with its length prefix) with
/// `fd` attached, or none for -1. The kernel duplicates the descriptor; the
/// caller still owns `fd`. Retries after EINTR, finishes partial writes and
/// waits on a non-blocking socket. Never raises SIGPIPE.
[[nodiscard]] Result<void> send_message(int socket, std::span<const std::byte> frame, int fd = -1);

/// Receives exactly one framed message whose body (after the prefix) is at
/// most `max_body` bytes, and the descriptor sent with it. nullopt: the peer
/// closed the socket before a new message began. Errors for end of file
/// inside a message, a length of zero or over the limit, more than one
/// descriptor, truncated control data (the kernel dropped descriptors), and
/// when `timeout_ms` (negative: no limit) passes first. Any descriptor
/// received with a failing message is closed.
[[nodiscard]] Result<std::optional<ReceivedMessage>> receive_message(int socket, std::size_t max_body,
                                                                     int timeout_ms = -1);

/// The process at the other end of a connected AF_UNIX socket, as of
/// connect() or socketpair().
struct PeerCredentials {
    uid_t uid = 0;
    gid_t gid = 0;
    /// Linux always reports it; macOS through LOCAL_PEERPID.
    std::optional<pid_t> pid;
};

/// SO_PEERCRED on Linux; getpeereid() (and LOCAL_PEERPID) on macOS and the BSDs.
[[nodiscard]] Result<PeerCredentials> peer_credentials(int socket);

}  // namespace farland::app
