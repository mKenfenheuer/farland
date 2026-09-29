// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/unique_fd.hpp>

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <stdexcept>
#include <unistd.h>

namespace farland::app {

/// A descriptor another thread makes readable: poll() waits for it, notify()
/// wakes the waiter from any thread, drain() resets it.
class WakePipe {
public:
    WakePipe()
    {
        std::array<int, 2> fds{-1, -1};
        if (::pipe(fds.data()) != 0) {
            throw std::runtime_error(std::format("pipe: {}", std::strerror(errno)));
        }
        read_end_.reset(fds[0]);
        write_end_.reset(fds[1]);
        for (const int fd : fds) {
            // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg): fcntl
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
            // NOLINTEND(cppcoreguidelines-pro-type-vararg)
        }
    }

    [[nodiscard]] int fd() const noexcept { return read_end_.get(); }

    void notify() const noexcept
    {
        // A pipe that is full already wakes the waiter.
        const char byte = 1;
        [[maybe_unused]] const auto written = ::write(write_end_.get(), &byte, 1);
    }

    void drain() const noexcept
    {
        std::array<char, 64> bytes{};
        while (::read(read_end_.get(), bytes.data(), bytes.size()) > 0) {
        }
    }

private:
    UniqueFd read_end_;
    UniqueFd write_end_;
};

}  // namespace farland::app
