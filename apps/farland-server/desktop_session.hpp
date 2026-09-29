// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "clipboard_relay.hpp"
#include "desktop.hpp"
#include "session.hpp"
#include "wake_pipe.hpp"

#include <farland/channels/dvc_server.hpp>
#include <farland/channels/rdpei_server.hpp>
#include <farland/proto/gcc.hpp>
#include <farland/proto/input.hpp>
#include <farland/proto/pointer.hpp>
#include <farland/server/connection.hpp>
#include <farland/server/display_layout.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace farland::app {

class InputThread;

/// What the desktop thread has the connection thread do.
namespace desktop_output {
/// A message for the graphics channel.
struct Graphics {
    std::uint32_t channel = 0;
    std::vector<std::byte> message;
};
/// Close the graphics channel: the pipeline is gone.
struct CloseGraphics {
    std::uint32_t channel = 0;
};
struct Pointer {
    proto::PointerUpdate update;
};
/// A bitmap update, for clients without the Graphics Pipeline.
struct Bitmap {
    std::vector<std::byte> update;
};
/// Reactivate the connection at this desktop size (bitmap updates take the
/// size from the capability exchange).
struct Reactivate {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
};
/// The layout the desktop shows now, for Display Control.
struct LayoutShown {
    server::DisplayLayout layout;
};
/// The desktop went away: end the connection, telling the client why.
struct Disconnect {
    std::uint32_t error_info = 0;
};
}  // namespace desktop_output

using DesktopOutput = std::variant<desktop_output::Graphics, desktop_output::CloseGraphics, desktop_output::Pointer,
                                   desktop_output::Bitmap, desktop_output::Reactivate, desktop_output::LayoutShown,
                                   desktop_output::Disconnect>;

/// The picture side of a session, on a thread that does nothing else: the
/// shared desktop (or the synthetic test pattern), its screens and cursor,
/// where they show on the client's monitors, and the encoding -- the Graphics
/// Pipeline with its encoders, or bitmap updates. The desktop's clipboard
/// backend runs here too, because it shares the desktop's connection
/// (ClipboardRelay).
///
/// The connection thread owns the RDP connection and its channels. It tells
/// this thread what the client did through the methods below, which return at
/// once, and carries out what take_output() returns when wake_fd() becomes
/// readable. Only choose_desktop_size() waits for an answer.
///
/// The screens (the desktop's monitors, or one test pattern per client
/// monitor) are put on the client's monitors (server::DisplayLayout): GFX
/// clients get a surface per screen, bitmap updates one composed picture.
/// The layout comes from CS_MONITOR, and later from the Display Control
/// channel; a shared desktop whose screens keep their size shows them at
/// their size to single-monitor clients until they ask for another layout.
class DesktopSession {
public:
    /// Starts the thread. `options.desktop` (may be null: the test pattern)
    /// must outlive it.
    DesktopSession(std::string peer, const SessionOptions& options);
    /// stop().
    ~DesktopSession();
    DesktopSession(const DesktopSession&) = delete;
    DesktopSession& operator=(const DesktopSession&) = delete;
    DesktopSession(DesktopSession&&) = delete;
    DesktopSession& operator=(DesktopSession&&) = delete;

    /// Ends the thread; the desktop's producers get their buffers back and
    /// the next session starts from CPU frames. Once.
    void stop();

    /// The desktop size for the connection, from the client's monitors: its
    /// layout, except that a shared desktop whose screens keep their size
    /// shows them side by side at their size to a client with one monitor.
    /// Waits for the desktop thread.
    [[nodiscard]] std::pair<std::uint16_t, std::uint16_t> choose_desktop_size(const proto::gcc::ClientData& data);
    /// The connection became active (again). `input` is where the desktop's
    /// input goes, null when it runs here (input()).
    void activated(server::Session session, std::size_t max_update_size, bool reactivation,
                   std::shared_ptr<InputThread> input);
    void refresh(std::vector<proto::Rectangle16> areas);
    void suppress_output(bool suppressed);
    /// The client can run the Graphics Pipeline: its channel, which the
    /// connection thread opened.
    void start_graphics(std::uint32_t channel);
    /// An event of the graphics channel.
    void graphics_event(channels::DvcEvent event);
    /// The client asked for another layout (Display Control).
    void client_layout(server::DisplayLayout layout);
    /// Input for a desktop whose input stays here, or for the test pattern.
    void input(std::vector<proto::InputEvent> events);
    void touch(std::vector<channels::rdpei::Contact> contacts);
    /// What the connection measured, for the quality ladder; from the
    /// connection thread whenever it changes.
    void set_network(const server::NetworkEstimate& network, std::uint64_t bytes_sent);

    /// The desktop's clipboard for the cliprdr channel; null when it has
    /// none (the connection thread runs a loopback for the test pattern).
    [[nodiscard]] platform::Clipboard* clipboard() noexcept { return relay_ ? &relay_->proxy() : nullptr; }
    /// The layout shown last, once there is one; any thread.
    [[nodiscard]] std::optional<server::DisplayLayout> current_layout() const;

    /// Readable while take_output() has something.
    [[nodiscard]] int wake_fd() const noexcept { return outbox_wake_.fd(); }
    [[nodiscard]] std::vector<DesktopOutput> take_output();

private:
    class Picture;

    void post(std::function<void(Picture&)> job);
    void run();

    std::unique_ptr<Picture> picture_;  ///< the desktop thread's
    std::unique_ptr<ClipboardRelay> relay_;

    mutable std::mutex mutex_;
    std::deque<std::function<void(Picture&)>> jobs_;
    bool quit_ = false;
    std::vector<DesktopOutput> outbox_;
    std::optional<server::DisplayLayout> current_layout_;
    WakePipe jobs_wake_;
    WakePipe outbox_wake_;
    std::thread thread_;  ///< last: starts once the rest is there
};

}  // namespace farland::app
