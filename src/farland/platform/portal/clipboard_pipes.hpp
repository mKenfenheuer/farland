// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/platform/clipboard.hpp>
#include <farland/platform/portal/portal_session.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string_view>
#include <vector>

/// The pipe side of a desktop clipboard, shared by PortalClipboard and the
/// Mutter backend's clipboard: both hand the data over through pipes the
/// compositor creates (SelectionRead, SelectionWrite).
namespace farland::platform::portal {

struct ClipboardTransferOptions {
    /// Local data larger than this fails a read.
    std::size_t max_read_size = std::size_t{64} * 1024 * 1024;
    /// How long the desktop application may take to hand over its data,
    /// and to take ours.
    std::chrono::milliseconds read_timeout{15'000};
    std::chrono::milliseconds write_timeout{15'000};
};

/// A non-blocking copy of the fd in `reply` (the message keeps its own).
[[nodiscard]] PortalResult<UniqueFd> take_reply_fd(sd_bus_message* reply, std::string_view what);

/// Reads and writes in flight, served without blocking.
class ClipboardPipes {
public:
    /// Called when a write is over: its data went out and the pipe was
    /// closed (true), or it failed or timed out (false).
    using WriteDone = std::function<void(std::uint32_t serial, bool success)>;

    ClipboardPipes(ClipboardTransferOptions options, WriteDone done);
    ClipboardPipes(const ClipboardPipes&) = delete;
    ClipboardPipes& operator=(const ClipboardPipes&) = delete;
    ClipboardPipes(ClipboardPipes&&) = delete;
    ClipboardPipes& operator=(ClipboardPipes&&) = delete;
    ~ClipboardPipes() = default;

    /// Reads the desktop's data for read `id` from `fd`; pump() queues a
    /// ReadFinished once it is all in.
    void read(std::uint64_t id, UniqueFd fd);
    /// Writes `data` to `fd`, as much as fits at once, the rest in pump().
    void write(std::uint32_t serial, UniqueFd fd, std::vector<std::byte> data);
    /// Serves the pipes; finished reads go to `events`.
    void pump(std::deque<ClipboardEvent>& events);
    /// Ends every write still in progress as failed.
    void cancel_writes();

    [[nodiscard]] std::vector<PollFd> poll_fds() const;
    [[nodiscard]] bool reading() const noexcept { return !reads_.empty(); }

private:
    using Clock = std::chrono::steady_clock;

    struct Read {
        std::uint64_t id = 0;
        UniqueFd fd;
        std::vector<std::byte> data;
        Clock::time_point deadline;
    };
    struct Write {
        std::uint32_t serial = 0;
        UniqueFd fd;
        std::vector<std::byte> data;
        std::size_t offset = 0;
        Clock::time_point deadline;
    };

    /// Reads or writes what the pipe takes; true when the transfer is over.
    [[nodiscard]] bool pump(Read& read, std::deque<ClipboardEvent>& events) const;
    [[nodiscard]] bool pump(Write& write);

    ClipboardTransferOptions options_;
    WriteDone done_;
    std::vector<Read> reads_;
    std::vector<Write> writes_;
};

}  // namespace farland::platform::portal
