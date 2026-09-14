// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/platform/backend.hpp>

#include <vector>

namespace farland::app {

/// A desktop that sessions show and control (docs/PLAN.md §3.3): frames, the
/// cursor and an input sink, plus the descriptors the session loop polls.
/// Without one, sessions show the synthetic test pattern.
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
};

}  // namespace farland::app
