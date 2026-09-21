// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/platform/backend.hpp>
#include <farland/platform/clipboard.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace farland::app {

/// Why a desktop stopped being ours. Each one means something different to
/// a client, and the difference decides whether it reconnects.
enum class DesktopEnd : std::uint8_t {
    /// The session itself is over: the user logged out, and the compositor
    /// with them. There is nothing left to come back to.
    logged_out,
    /// The desktop is still running, but this connection may no longer have
    /// it: the user stopped sharing, or the stream was closed.
    sharing_stopped,
    /// Somebody at the machine logged in and took the session back
    /// ([policy] seat_takeover). It is theirs now.
    taken_at_the_machine,
};

/// A desktop that sessions show and control (docs/PLAN.md §3.3): frames, the
/// cursor and an input sink, plus the descriptors the session loop polls.
/// Without one, sessions show the synthetic test pattern.
///
/// A desktop has one or more screens (monitors), each with its own frames
/// and cursor; frames() and cursor() are the first. The session puts them on
/// the client's monitors (server::DisplayLayout).
///
/// One session at a time uses a desktop, on the session's thread.
class Desktop {
public:
    Desktop() = default;
    Desktop(const Desktop&) = delete;
    Desktop& operator=(const Desktop&) = delete;
    Desktop(Desktop&&) = delete;
    Desktop& operator=(Desktop&&) = delete;
    virtual ~Desktop() = default;

    [[nodiscard]] virtual platform::FrameSource& frames() = 0;
    /// Null when the backend cannot deliver the cursor apart from the frames.
    [[nodiscard]] virtual platform::CursorSource* cursor() = 0;
    [[nodiscard]] virtual platform::InputSink& input() = 0;
    /// Descriptors, besides the sources' wake fds, whose readiness calls for
    /// dispatch() (the input connection, the portal's D-Bus connection).
    [[nodiscard]] virtual std::vector<int> dispatch_fds() const = 0;
    virtual void dispatch() = 0;
    /// The desktop went away: the user stopped sharing or the stream closed.
    [[nodiscard]] virtual bool closed() const = 0;
    /// Why it went away, so that the client can be told in the one way RDP
    /// has of saying it. A client that is told nothing assumes the network
    /// dropped and reconnects by itself, which is precisely wrong when
    /// somebody has just logged out or stopped sharing on purpose.
    [[nodiscard]] virtual DesktopEnd end_reason() const { return DesktopEnd::sharing_stopped; }

    /// The screens, at least one, ordered left to right as the compositor
    /// arranges them. The count stays the same while the desktop is open,
    /// unless screens_follow_monitors().
    [[nodiscard]] virtual std::size_t screen_count() const { return 1; }
    /// Frames of screen `index` (< screen_count()); screen 0 is frames().
    [[nodiscard]] virtual platform::FrameSource& screen_frames(std::size_t index)
    {
        static_cast<void>(index);
        return frames();
    }
    /// The cursor as screen `index` sees it, positions in its pixels; null
    /// as for cursor().
    [[nodiscard]] virtual platform::CursorSource* screen_cursor(std::size_t index)
    {
        static_cast<void>(index);
        return cursor();
    }
    /// True when the screens take the size the client asks for (virtual
    /// monitors). Otherwise they keep theirs and the session scales and
    /// centres them on the client's monitors.
    [[nodiscard]] virtual bool resizable() const { return false; }
    /// True when the desktop adds and removes screens (virtual monitors) to
    /// have one per client monitor. request_screen_sizes() then gets a size
    /// for every client monitor, and screen_count() may change, but only
    /// during request_screen_sizes() and dispatch(): screens keep their
    /// index, new ones come last (once they have a frame), and removed ones
    /// are the last.
    [[nodiscard]] virtual bool screens_follow_monitors() const { return false; }
    /// The size of the client monitor each screen shows on, in screen order.
    /// A resizable desktop resizes its screens to match; the new size shows
    /// in the frames once the compositor followed.
    virtual void request_screen_sizes(std::span<const std::pair<std::uint32_t, std::uint32_t>> sizes)
    {
        static_cast<void>(sizes);
    }
    /// Where each screen shows on the client's desktop (nullopt: nowhere),
    /// for absolute pointer motion: the desktop's input then takes positions
    /// in client desktop pixels and finds the screen under them. False when
    /// the desktop cannot do that; its input then takes positions in the
    /// first screen's pixels.
    virtual bool set_screen_targets(std::span<const std::optional<platform::Rect>> targets)
    {
        static_cast<void>(targets);
        return false;
    }
    /// Whether the desktop is worth keeping while no client holds it. A
    /// desktop that shows a session on a local seat is not: it is given back
    /// whole, so that the seat has its session as it was, and the next
    /// connection takes it again.
    [[nodiscard]] virtual bool keep_when_released() const { return true; }
    /// Whether a client holds the desktop now. A desktop attached to a
    /// session on a local seat takes the session from the seat while a
    /// client holds it, and gives it back when none does, so that the screen
    /// at the machine never keeps showing what a client took away.
    virtual void set_held(bool held) { static_cast<void>(held); }
    /// [policy] seat_takeover: somebody logging in at the machine was asked
    /// about, and this is what came of it. A refusal matters to a desktop
    /// that took the session from a seat: the display manager gives up the
    /// login screen it was showing, so the seat falls back to this session
    /// for a moment, and that must not count as the seat taking it back.
    virtual void seat_takeover_decided(bool allowed) { static_cast<void>(allowed); }
    /// The desktop's clipboard; null when the backend has none or was not
    /// granted access.
    [[nodiscard]] virtual platform::Clipboard* clipboard() { return nullptr; }
};

}  // namespace farland::app
