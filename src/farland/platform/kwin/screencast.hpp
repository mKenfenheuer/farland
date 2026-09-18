// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/platform/kwin/wayland_connection.hpp>

#include <cstdint>
#include <memory>
#include <string>

struct wl_output;
struct zkde_screencast_stream_unstable_v1;
struct zkde_screencast_unstable_v1;

/// KWin's screen casting (zkde_screencast_unstable_v1, plasma-wayland-
/// protocols): KWin turns an output into a PipeWire stream on the user's
/// PipeWire daemon and names its node.
///
/// KWin grants the interface only to trusted clients: the executable must be
/// the Exec of a desktop file with X-KDE-Wayland-Interfaces listing it, in
/// the application directories of KWin's XDG_DATA_DIRS or XDG_DATA_HOME
/// (or KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 in KWin's environment).
namespace farland::platform::kwin {

/// One stream. Closing it (destroying the object) ends KWin's stream.
class ScreencastStream {
public:
    enum class State : std::uint8_t {
        pending,  ///< KWin has not answered yet.
        created,  ///< node() is the PipeWire node.
        failed,   ///< KWin refused; error() says why. Final.
        closed,   ///< KWin ended the stream. Final.
    };

    ScreencastStream(const ScreencastStream&) = delete;
    ScreencastStream& operator=(const ScreencastStream&) = delete;
    ScreencastStream(ScreencastStream&&) = delete;
    ScreencastStream& operator=(ScreencastStream&&) = delete;
    ~ScreencastStream();

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] std::uint32_t node() const noexcept { return node_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    struct Listener;

private:
    friend class Screencast;
    explicit ScreencastStream(zkde_screencast_stream_unstable_v1* proxy);

    zkde_screencast_stream_unstable_v1* proxy_ = nullptr;
    State state_ = State::pending;
    std::uint32_t node_ = 0;
    std::string error_;
};

class Screencast {
public:
    /// zkde_screencast_unstable_v1.pointer
    enum class Cursor : std::uint32_t { hidden = 1, embedded = 2, metadata = 4 };

    /// Binds the manager; fails when KWin does not offer it to this process.
    [[nodiscard]] static Result<std::unique_ptr<Screencast>> create(WaylandConnection& connection);

    Screencast(const Screencast&) = delete;
    Screencast& operator=(const Screencast&) = delete;
    Screencast(Screencast&&) = delete;
    Screencast& operator=(Screencast&&) = delete;
    ~Screencast();

    /// Streams `output`; the stream's state changes as KWin answers.
    [[nodiscard]] std::unique_ptr<ScreencastStream> stream_output(wl_output* output, Cursor cursor);

private:
    Screencast(WaylandConnection& connection, zkde_screencast_unstable_v1* manager) noexcept
        : connection_(connection), manager_(manager)
    {
    }

    WaylandConnection& connection_;
    zkde_screencast_unstable_v1* manager_ = nullptr;
};

}  // namespace farland::platform::kwin
