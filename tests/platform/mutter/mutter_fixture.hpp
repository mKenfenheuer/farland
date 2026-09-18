// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/platform/mutter/mutter_session.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace farland::test {

/// A private D-Bus daemon with mock_mutter.py on it.
class MockMutter {
public:
    MockMutter() = default;
    MockMutter(const MockMutter&) = delete;
    MockMutter& operator=(const MockMutter&) = delete;
    MockMutter(MockMutter&&) = delete;
    MockMutter& operator=(MockMutter&&) = delete;
    ~MockMutter();

    [[nodiscard]] const std::string& address() const noexcept { return address_; }
    /// The calls the mock saw (see mock_mutter.py); empty if it cannot be
    /// reached. Safe on any thread: no Catch2 assertions.
    [[nodiscard]] std::vector<std::string> calls() const;
    /// Waits until the mock saw a call starting with `prefix`, dispatching
    /// `session` meanwhile; false after five seconds.
    [[nodiscard]] bool wait_for_call(const std::string& prefix,
                                     platform::mutter::MutterSession* session = nullptr) const;
    /// Mutter closes every session; Mutter leaves the bus.
    [[nodiscard]] bool close_sessions() const;
    [[nodiscard]] bool vanish() const;

    /// The desktop side of the clipboard, as in MockPortal.
    [[nodiscard]] bool copy(const std::vector<std::string>& mime_types, const std::vector<std::string>& contents) const;
    [[nodiscard]] std::optional<std::uint32_t> paste(const std::string& mime_type) const;
    [[nodiscard]] std::optional<std::pair<bool, std::string>> written(std::uint32_t serial) const;

    /// Options that talk to this bus.
    [[nodiscard]] platform::mutter::MutterOptions options() const;

private:
    friend std::unique_ptr<MockMutter> start_mock_mutter(const std::vector<std::string>& args, bool with_mutter);

    pid_t bus_pid_ = -1;
    pid_t mock_pid_ = -1;
    std::string address_;
};

/// Starts the bus and, when `with_mutter`, the mock with `args`. SKIPs the
/// test when dbus-daemon, python3-dbus or PyGObject is missing.
[[nodiscard]] std::unique_ptr<MockMutter> start_mock_mutter(const std::vector<std::string>& args = {},
                                                            bool with_mutter = true);

/// The path of mock_mutter.py.
[[nodiscard]] std::string mock_mutter_script();

}  // namespace farland::test
