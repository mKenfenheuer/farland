// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/platform/backend.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

/// The video half of the portal backend (docs/PLAN.md §3.3): consumes one
/// ScreenCast PipeWire stream and turns it into BGRX frames and cursor updates.
namespace farland::platform::portal {

struct PipeWireCaptureOptions {
    /// Offer dmabuf buffers: LINEAR ones (mmapped) and, when farland was built
    /// with GBM and EGL and a render node works, every modifier the GPU imports.
    /// Shared memory is always offered as the fallback.
    bool dmabuf = true;
    /// DRM render node for importing tiled dmabufs; empty picks the first
    /// /dev/dri/renderD* that opens.
    std::string render_node;
    /// Upper end of the frame rate range offered to the producer.
    std::uint32_t max_framerate = 60;
    /// Cursor bitmaps have premultiplied alpha (mutter and KWin do this).
    bool cursor_premultiplied = true;
    /// node.name of the capture stream, for pw-dump and logs.
    std::string stream_name = "farland-capture";
};

enum class CaptureState : std::uint8_t {
    connecting,  ///< Waiting to be linked to the producer and to agree on a format.
    paused,      ///< Linked, but the producer is not sending.
    streaming,   ///< Frames flow.
    closed,      ///< Final: the stream failed, the producer node went away or PipeWire hung up.
};

[[nodiscard]] std::string_view to_string(CaptureState state) noexcept;

/// A PipeWire video consumer. It runs its own pw_thread_loop; conversion to
/// BGRX happens there, into buffers the capture owns, and the session thread
/// picks up the results through frames() and cursor(), which follow the
/// threading model of backend.hpp.
///
/// When the stream closes, both wake fds stay readable (like a socket at end
/// of file) and state() returns CaptureState::closed; the session should then
/// stop polling them and end or restart the portal session.
class PipeWireCapture {
public:
    /// Connects to the stream `node_id`. `pipewire_fd` is the remote from the
    /// ScreenCast portal's OpenPipeWireRemote (the capture duplicates it, the
    /// caller keeps its own), or -1 to connect to the default daemon.
    [[nodiscard]] static Result<std::unique_ptr<PipeWireCapture>> create(int pipewire_fd, std::uint32_t node_id,
                                                                         const PipeWireCaptureOptions& options = {});

    PipeWireCapture(const PipeWireCapture&) = delete;
    PipeWireCapture& operator=(const PipeWireCapture&) = delete;
    PipeWireCapture(PipeWireCapture&&) = delete;
    PipeWireCapture& operator=(PipeWireCapture&&) = delete;
    ~PipeWireCapture();

    /// The frames. Each wake_fd is its own eventfd.
    [[nodiscard]] FrameSource& frames() noexcept;
    /// Cursor shape, position and visibility from SPA_META_Cursor. The cursor
    /// is never part of the frames (the portal's cursor mode must be
    /// "metadata"; with "embedded" no cursor updates arrive).
    [[nodiscard]] CursorSource& cursor() noexcept;

    [[nodiscard]] CaptureState state() const;
    [[nodiscard]] bool closed() const { return state() == CaptureState::closed; }
    /// Why the stream closed; empty while it is open.
    [[nodiscard]] std::string error() const;
    /// The capture stream's own node id (SPA_ID_INVALID until PipeWire
    /// assigned one), for logs and for linking it by hand.
    [[nodiscard]] std::uint32_t node_id() const;
    /// Buffers the producer delivered so far (frames, cursor-only and corrupt
    /// ones alike), counted once their frame and cursor updates are out.
    [[nodiscard]] std::uint64_t buffers_received() const noexcept;

    struct Impl;

private:
    explicit PipeWireCapture(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace farland::platform::portal
