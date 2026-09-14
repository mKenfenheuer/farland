// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <unistd.h>
#include <utility>

namespace farland {

/// Owns a POSIX file descriptor and closes it.
///
/// platform/portal has an identical class from M4; it can switch to this one
/// once the M6 channel work there has landed.
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    ~UniqueFd() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    /// Gives up ownership without closing.
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }
    /// Closes the current descriptor and owns `fd` instead.
    void reset(int fd = -1) noexcept
    {
        if (fd_ >= 0 && fd_ != fd) {
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

}  // namespace farland
