// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct wl_display;
struct wl_interface;
struct wl_registry;

/// A generic Wayland client connection for backends that drive a compositor
/// directly (docs/PLAN.md §3.3): the display, its globals, and dispatching
/// that fits farland's poll loop.
///
/// Threading: one thread at a time (the session's), like the backend
/// interfaces. Nothing here blocks except roundtrip() and wait_until(),
/// which are meant for setting up.
namespace farland::platform::wayland {

struct Global {
    std::uint32_t name = 0;
    std::string interface;
    std::uint32_t version = 0;
};

class Connection {
public:
    /// Connects to the compositor's socket at `path` (an absolute path), or,
    /// when `path` is empty, where $WAYLAND_DISPLAY and $XDG_RUNTIME_DIR
    /// say, and waits up to `timeout` for the globals.
    [[nodiscard]] static Result<std::unique_ptr<Connection>> connect(const std::string& path,
                                                                     std::chrono::milliseconds timeout);

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(Connection&&) = delete;
    ~Connection();

    [[nodiscard]] wl_display* display() const noexcept { return display_; }
    /// Readable when events arrived: poll it, then dispatch().
    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] const std::vector<Global>& globals() const noexcept { return globals_; }
    /// The first global with this interface; null when there is none.
    [[nodiscard]] const Global* find(std::string_view interface) const noexcept;
    /// Binds `global` at `version`, or at the compositor's version if lower.
    [[nodiscard]] void* bind(const Global& global, const wl_interface* interface, std::uint32_t version) const;
    template <class T>
    [[nodiscard]] T* bind(const Global& global, const wl_interface* interface, std::uint32_t version) const
    {
        return static_cast<T*>(bind(global, interface, version));
    }

    /// Reads what arrived without blocking, dispatches it and sends what
    /// is queued.
    void dispatch();
    /// Sends the queued requests (as far as the socket takes them).
    void flush();
    /// Dispatches until the compositor has handled every request sent so
    /// far. False when the connection failed or `timeout` passed.
    [[nodiscard]] bool roundtrip(std::chrono::milliseconds timeout);
    /// Dispatches, blocking up to `timeout`, until `done()` is true. False
    /// when the connection failed or the time ran out first.
    [[nodiscard]] bool wait_until(const std::function<bool()>& done, std::chrono::milliseconds timeout);

    /// The compositor went away or raised a protocol error.
    [[nodiscard]] bool closed() const noexcept { return closed_; }
    /// Why the connection closed; empty while it is open.
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    /// Called with the name of each global the compositor removes.
    void on_global_removed(std::function<void(std::uint32_t name)> callback);

private:
    explicit Connection(wl_display* display);
    static void handle_global(void* data, wl_registry* registry, std::uint32_t name, const char* interface,
                              std::uint32_t version);
    static void handle_global_remove(void* data, wl_registry* registry, std::uint32_t name);
    /// Dispatches what is queued; false once the connection failed.
    bool dispatch_pending();
    void close_with(std::string_view what);

    wl_display* display_;
    wl_registry* registry_ = nullptr;
    std::vector<Global> globals_;
    std::vector<std::function<void(std::uint32_t)>> removed_callbacks_;
    bool closed_ = false;
    std::string error_;
};

}  // namespace farland::platform::wayland
