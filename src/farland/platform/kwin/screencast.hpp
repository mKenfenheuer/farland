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

    /// stream_virtual_output arrived in version 2, and
    /// stream_virtual_output_with_description in version 4.
    static constexpr std::uint32_t virtual_output_version = 2;
    static constexpr std::uint32_t description_version = 4;

    /// Binds the manager; fails when KWin does not offer it to this process.
    [[nodiscard]] static Result<std::unique_ptr<Screencast>> create(WaylandConnection& connection);

    Screencast(const Screencast&) = delete;
    Screencast& operator=(const Screencast&) = delete;
    Screencast(Screencast&&) = delete;
    Screencast& operator=(Screencast&&) = delete;
    ~Screencast();

    /// Streams `output`; the stream's state changes as KWin answers.
    [[nodiscard]] std::unique_ptr<ScreencastStream> stream_output(wl_output* output, Cursor cursor);

    /// A new virtual output of `width` x `height` logical pixels, streamed:
    /// KWin creates an output of its own, which shows up as a wl_output named
    /// `name` and is laid out beside the real ones. It goes away when the
    /// stream closes. Null where KWin is older than virtual_output_version;
    /// `description` is dropped below description_version.
    [[nodiscard]] std::unique_ptr<ScreencastStream> stream_virtual_output(const std::string& name,
                                                                          const std::string& description,
                                                                          std::int32_t width, std::int32_t height,
                                                                          double scale, Cursor cursor);

    /// The version of the interface that was bound: what KWin offers, capped
    /// at what the protocol copy in protocols/ describes.
    [[nodiscard]] std::uint32_t version() const noexcept { return version_; }
    /// KWin takes stream_virtual_output.
    [[nodiscard]] bool has_virtual_outputs() const noexcept { return version_ >= virtual_output_version; }

private:
    Screencast(WaylandConnection& connection, zkde_screencast_unstable_v1* manager, std::uint32_t version) noexcept
        : connection_(connection), manager_(manager), version_(version)
    {
    }

    WaylandConnection& connection_;
    zkde_screencast_unstable_v1* manager_ = nullptr;
    std::uint32_t version_ = 1;
};

}  // namespace farland::platform::kwin
