// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/platform/portal/portal_session.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace farland::test {

/// A private D-Bus daemon with mock_portal.py on it.
class MockPortal {
public:
    MockPortal() = default;
    MockPortal(const MockPortal&) = delete;
    MockPortal& operator=(const MockPortal&) = delete;
    MockPortal(MockPortal&&) = delete;
    MockPortal& operator=(MockPortal&&) = delete;
    ~MockPortal();

    [[nodiscard]] const std::string& address() const noexcept { return address_; }
    /// The calls the mock saw (see mock_portal.py). Main thread only: it
    /// asserts with Catch2, which is not thread-safe.
    [[nodiscard]] std::vector<std::string> calls() const;
    /// Waits until the mock saw at least `count` calls, dispatching `session`
    /// meanwhile; returns the calls (fewer on timeout or when the mock cannot
    /// be reached). Safe on any thread: no Catch2 assertions.
    [[nodiscard]] std::vector<std::string> wait_for_calls(std::size_t count,
                                                          platform::portal::PortalSession* session = nullptr) const;
    /// Makes the mock emit Session.Closed for every session; false if the mock
    /// could not be reached. Safe on any thread.
    [[nodiscard]] bool close_sessions() const;

    /// The desktop side of the clipboard, safe on any thread: someone copies
    /// `contents` (one per type); someone pastes `mime_type` (the transfer's
    /// serial, nullopt if the mock cannot be reached); what the session wrote
    /// for a paste (success and data) once it is finished.
    [[nodiscard]] bool copy(const std::vector<std::string>& mime_types, const std::vector<std::string>& contents) const;
    [[nodiscard]] std::optional<std::uint32_t> paste(const std::string& mime_type) const;
    [[nodiscard]] std::optional<std::pair<bool, std::string>> written(std::uint32_t serial) const;

    /// Portal options that talk to this bus.
    [[nodiscard]] platform::portal::PortalOptions options() const;

private:
    friend std::unique_ptr<MockPortal> start_mock_portal(const std::vector<std::string>& args, bool with_portal);

    pid_t bus_pid_ = -1;
    pid_t mock_pid_ = -1;
    std::string address_;
};

/// Starts the bus and, when `with_portal`, the mock with `args`. SKIPs the
/// test when dbus-daemon, python3-dbus or PyGObject is missing.
[[nodiscard]] std::unique_ptr<MockPortal> start_mock_portal(const std::vector<std::string>& args = {},
                                                            bool with_portal = true);

/// Reads what is in the pipe behind `fd`.
[[nodiscard]] std::string read_all(int fd);

}  // namespace farland::test
