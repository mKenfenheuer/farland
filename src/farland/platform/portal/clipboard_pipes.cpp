// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/portal/clipboard_pipes.hpp>
#include <farland/platform/portal/portal_bus.hpp>

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <poll.h>
#include <span>
#include <unistd.h>

namespace farland::platform::portal {

namespace {

constexpr std::string_view log_component = "platform.clipboard";
constexpr std::size_t read_chunk = std::size_t{64} * 1024;

}  // namespace

PortalResult<UniqueFd> take_reply_fd(sd_bus_message* reply, std::string_view what)
{
    int borrowed = -1;
    if (!detail::MessageReader(reply).fd(borrowed)) {
        return detail::fail(PortalErrc::protocol, std::format("{}: the reply has no file descriptor", what));
    }
    UniqueFd fd(::fcntl(borrowed, F_DUPFD_CLOEXEC, 3));  // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (!fd.valid()) {
        return detail::fail(PortalErrc::protocol,
                            std::format("{}: cannot duplicate the fd: {}", what, std::strerror(errno)));
    }
    const int flags = ::fcntl(fd.get(), F_GETFL);    // NOLINT(cppcoreguidelines-pro-type-vararg)
    ::fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK);  // NOLINT(cppcoreguidelines-pro-type-vararg)
    return fd;
}

ClipboardPipes::ClipboardPipes(ClipboardTransferOptions options, WriteDone done)
    : options_(options), done_(std::move(done))
{
}

void ClipboardPipes::read(std::uint64_t id, UniqueFd fd)
{
    reads_.push_back(Read{id, std::move(fd), {}, Clock::now() + options_.read_timeout});
}

void ClipboardPipes::write(std::uint32_t serial, UniqueFd fd, std::vector<std::byte> data)
{
    writes_.push_back(Write{serial, std::move(fd), std::move(data), 0, Clock::now() + options_.write_timeout});
    if (pump(writes_.back())) {
        writes_.pop_back();
    }
}

void ClipboardPipes::pump(std::deque<ClipboardEvent>& events)
{
    std::erase_if(reads_, [&](Read& read) { return pump(read, events); });
    std::erase_if(writes_, [this](Write& write) { return pump(write); });
}

void ClipboardPipes::cancel_writes()
{
    for (auto& write : writes_) {
        write.fd.reset();
        done_(write.serial, false);
    }
    writes_.clear();
}

std::vector<PollFd> ClipboardPipes::poll_fds() const
{
    std::vector<PollFd> fds;
    fds.reserve(reads_.size() + writes_.size());
    for (const auto& read : reads_) {
        fds.push_back(PollFd{read.fd.get(), POLLIN});
    }
    for (const auto& write : writes_) {
        fds.push_back(PollFd{write.fd.get(), POLLOUT});
    }
    return fds;
}

bool ClipboardPipes::pump(Read& read, std::deque<ClipboardEvent>& events) const
{
    std::array<std::byte, read_chunk> buffer{};
    for (;;) {
        const auto n = ::read(read.fd.get(), buffer.data(), buffer.size());
        if (n > 0) {
            if (read.data.size() + static_cast<std::size_t>(n) > options_.max_read_size) {
                log::warn(log_component, "the desktop's clipboard data is larger than {} bytes",
                          options_.max_read_size);
                events.emplace_back(clipboard_event::ReadFinished{read.id, std::nullopt});
                return true;
            }
            const auto got = std::span(buffer).first(static_cast<std::size_t>(n));
            read.data.insert(read.data.end(), got.begin(), got.end());
            continue;
        }
        if (n == 0) {
            events.emplace_back(clipboard_event::ReadFinished{read.id, std::move(read.data)});
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN) {  // EWOULDBLOCK on Linux
            if (Clock::now() < read.deadline) {
                return false;
            }
            log::warn(log_component, "the desktop did not hand over its clipboard data in time");
        }
        events.emplace_back(clipboard_event::ReadFinished{read.id, std::nullopt});
        return true;
    }
}

bool ClipboardPipes::pump(Write& write)
{
    while (write.offset < write.data.size()) {
        const auto rest = std::span(write.data).subspan(write.offset);
        const auto n = ::write(write.fd.get(), rest.data(), rest.size());
        if (n > 0) {
            write.offset += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && errno == EAGAIN && Clock::now() < write.deadline) {
            return false;
        }
        write.fd.reset();
        done_(write.serial, false);
        return true;
    }
    write.fd.reset();  // EOF for the reader
    done_(write.serial, true);
    return true;
}

}  // namespace farland::platform::portal
