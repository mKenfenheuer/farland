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
struct wl_output;
struct wl_registry;
struct wl_seat;

/// A Wayland client connection for the compositor backends (docs/PLAN.md
/// §3.3): the registry's globals, the outputs and the first seat. Nothing in
/// it is specific to KWin.
///
/// Threading: everything runs on the owner's thread (the session thread):
/// poll fd() and call dispatch() when it becomes readable.
namespace farland::platform::kwin {

struct WaylandGlobal {
    std::uint32_t name = 0;
    std::string interface;
    std::uint32_t version = 0;
};

/// A wl_output as the compositor describes it (wl_output version 4).
struct WaylandOutput {
    std::uint32_t global = 0;
    wl_output* proxy = nullptr;
    /// The connector name ("Virtual-0", "DP-1").
    std::string name;
    std::string description;
    /// The position in the compositor's space.
    std::int32_t x = 0;
    std::int32_t y = 0;
    /// The current mode, in pixels.
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t scale = 1;
    /// The compositor finished describing it (the first done event).
    bool ready = false;
    /// Done events so far.
    std::uint64_t descriptions = 0;
};

/// A no-op for listener events the client does not need. libwayland calls
/// every member of a listener without checking it, so each must be set;
/// IgnoreEvent converts to any callback that returns nothing.
struct IgnoreEvent {
    template <typename... Args>
    using Callback = void (*)(Args...);

    template <typename... Args>
    constexpr operator Callback<Args...>() const noexcept  // NOLINT(google-explicit-constructor)
    {
        return [](Args...) {};
    }
};
inline constexpr IgnoreEvent ignore_event{};

class WaylandConnection {
public:
    /// Connects to the socket `display` (a name in $XDG_RUNTIME_DIR or an
    /// absolute path), or to $WAYLAND_DISPLAY when it is empty, and reads
    /// the globals and outputs.
    [[nodiscard]] static Result<std::unique_ptr<WaylandConnection>> connect(const std::string& display,
                                                                            std::chrono::milliseconds timeout);
    /// Takes ownership of a connected socket (tests).
    [[nodiscard]] static Result<std::unique_ptr<WaylandConnection>> connect_fd(int fd,
                                                                               std::chrono::milliseconds timeout);

    WaylandConnection(const WaylandConnection&) = delete;
    WaylandConnection& operator=(const WaylandConnection&) = delete;
    WaylandConnection(WaylandConnection&&) = delete;
    WaylandConnection& operator=(WaylandConnection&&) = delete;
    ~WaylandConnection();

    [[nodiscard]] wl_display* display() const noexcept { return display_; }
    [[nodiscard]] int fd() const noexcept;

    [[nodiscard]] const std::vector<WaylandGlobal>& globals() const noexcept { return globals_; }
    [[nodiscard]] const WaylandGlobal* find_global(std::string_view interface) const noexcept;
    /// Binds `global` at the lower of its version and `max_version`.
    [[nodiscard]] void* bind(const WaylandGlobal& global, const wl_interface* interface,
                             std::uint32_t max_version) const;
    /// Called for globals announced later, and before a global goes away.
    void set_global_listeners(std::function<void(const WaylandGlobal&)> added,
                              std::function<void(const WaylandGlobal&)> removed);

    /// Waits until the compositor handled every request sent so far,
    /// dispatching events meanwhile. False on a time-out or a broken
    /// connection.
    [[nodiscard]] bool roundtrip(std::chrono::milliseconds timeout);
    /// Reads and dispatches the events that arrived, without blocking, and
    /// sends pending requests.
    void dispatch();
    /// Sends pending requests.
    void flush();
    /// The compositor went away or reported a protocol error. Final.
    [[nodiscard]] bool broken() const noexcept { return broken_; }

    /// The outputs in the order the compositor announced them.
    [[nodiscard]] const std::vector<std::unique_ptr<WaylandOutput>>& outputs() const noexcept { return outputs_; }
    [[nodiscard]] const WaylandOutput* find_output(std::string_view name) const noexcept;
    /// Changes whenever an output is added, removed or described anew.
    [[nodiscard]] std::uint64_t outputs_generation() const noexcept { return outputs_generation_; }
    /// The first seat; null when the compositor has none.
    [[nodiscard]] wl_seat* seat() const noexcept { return seat_; }

    struct Listeners;

private:
    explicit WaylandConnection(wl_display* display) noexcept : display_(display) {}
    [[nodiscard]] static Result<std::unique_ptr<WaylandConnection>> finish(wl_display* display,
                                                                           std::chrono::milliseconds timeout);
    void global_added(wl_registry* registry, std::uint32_t name, const char* interface, std::uint32_t version);
    void global_removed(std::uint32_t name);
    void check_error();

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_seat* seat_ = nullptr;
    std::uint32_t seat_global_ = 0;
    std::vector<WaylandGlobal> globals_;
    std::vector<std::unique_ptr<WaylandOutput>> outputs_;
    std::uint64_t outputs_generation_ = 0;
    std::function<void(const WaylandGlobal&)> on_added_;
    std::function<void(const WaylandGlobal&)> on_removed_;
    bool broken_ = false;
};

}  // namespace farland::platform::kwin
