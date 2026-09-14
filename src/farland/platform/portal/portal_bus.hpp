// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/platform/portal/portal_session.hpp>
#include <farland/platform/portal/sd_bus.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

/// Internal: the sd-bus connection of a PortalSession and helpers to build
/// and read portal messages. Not part of the backend's API.
namespace farland::platform::portal::detail {

using Clock = std::chrono::steady_clock;

inline constexpr const char* portal_service = "org.freedesktop.portal.Desktop";
inline constexpr const char* portal_path = "/org/freedesktop/portal/desktop";
inline constexpr const char* remote_desktop_interface = "org.freedesktop.portal.RemoteDesktop";
inline constexpr const char* screen_cast_interface = "org.freedesktop.portal.ScreenCast";
inline constexpr const char* request_interface = "org.freedesktop.portal.Request";
inline constexpr const char* session_interface = "org.freedesktop.portal.Session";

/// One entry of an a{sv} options dictionary we send.
struct Option {
    std::string key;
    std::variant<std::string, std::uint32_t, bool> value;
};
using Options = std::vector<Option>;

[[nodiscard]] PortalError make_error(PortalErrc code, std::string message);
[[nodiscard]] std::unexpected<PortalError> fail(PortalErrc code, std::string message);
/// Maps a D-Bus error reply (or a failed sd-bus call, `r` < 0) to a PortalError.
[[nodiscard]] std::unexpected<PortalError> fail_call(std::string_view what, const sd_bus_error* error, int r);

/// Appends arguments, remembering the first failure.
class MessageWriter {
public:
    explicit MessageWriter(sd_bus_message* message) noexcept : message_(message) {}

    MessageWriter& object_path(const std::string& value);
    MessageWriter& string(const std::string& value);
    MessageWriter& u32(std::uint32_t value);
    MessageWriter& i32(std::int32_t value);
    MessageWriter& f64(double value);
    MessageWriter& options(const Options& options);
    [[nodiscard]] int status() const noexcept { return status_; }

private:
    void append(char type, const void* value);

    sd_bus_message* message_;
    int status_ = 0;
};

/// Reads arguments. Every method returns false on a type mismatch or a
/// truncated message; the message's read position is then unspecified.
class MessageReader {
public:
    explicit MessageReader(sd_bus_message* message) noexcept : message_(message) {}

    [[nodiscard]] bool u32(std::uint32_t& value);
    [[nodiscard]] bool boolean(bool& value);
    /// Accepts both 's' and 'o'.
    [[nodiscard]] bool string(std::string& value);
    [[nodiscard]] bool int_pair(std::pair<std::int32_t, std::int32_t>& value);
    [[nodiscard]] bool fd(int& value);
    /// Iterates an a{sv}. For each entry, `visit(key, signature)` is called
    /// with the message inside the variant; it reads the value and returns
    /// true, or returns false to have the value skipped. A `visit` that
    /// fails to read must make the whole call fail by setting `ok` to false.
    [[nodiscard]] bool
    vardict(const std::function<bool(std::string_view key, std::string_view signature, bool& ok)>& visit);
    [[nodiscard]] bool enter(char type, const char* contents);
    [[nodiscard]] bool exit();
    [[nodiscard]] sd_bus_message* get() const noexcept { return message_; }

private:
    sd_bus_message* message_;
};

/// The connection. All methods run on the owner's thread.
class Bus {
public:
    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;
    Bus(Bus&&) = delete;
    Bus& operator=(Bus&&) = delete;
    ~Bus() = default;

    /// Connects to `address`, or to the session bus when it is empty.
    [[nodiscard]] static PortalResult<std::unique_ptr<Bus>> open(const std::string& address);

    [[nodiscard]] sd_bus* get() const noexcept { return bus_.get(); }
    [[nodiscard]] const std::string& unique_name() const noexcept { return unique_name_; }

    /// Dispatches messages until `done()` is true. Fails at `deadline`, when
    /// `cancel_fd` becomes readable, or when the connection breaks.
    [[nodiscard]] PortalResult<void> run_until(const std::function<bool()>& done, Clock::time_point deadline,
                                               int cancel_fd);
    /// Dispatches whatever is pending without blocking.
    [[nodiscard]] PortalResult<void> process_pending();

    [[nodiscard]] PortalResult<MessagePtr> new_call(const char* interface, const char* member,
                                                    const char* path = portal_path,
                                                    const char* destination = portal_service);
    /// Sends `message` and waits for the reply as run_until() does. D-Bus
    /// errors become PortalErrors naming `what`.
    [[nodiscard]] PortalResult<MessagePtr> call(sd_bus_message* message, std::string_view what,
                                                Clock::time_point deadline, int cancel_fd);
    /// Sends `message` without waiting; a failure reply is logged when it is
    /// dispatched later.
    void send(sd_bus_message* message);

private:
    Bus() = default;
    static int on_send_reply(sd_bus_message* reply, void* userdata, sd_bus_error* error);

    BusPtr bus_;
    std::string unique_name_;
    std::uint64_t failed_sends_ = 0;
};

}  // namespace farland::platform::portal::detail
