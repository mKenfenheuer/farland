// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/codec/image.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

/// Platform backends (docs/PLAN.md §3.3): where the session gets frames and
/// the cursor from, and where client input goes. The session only talks to
/// these interfaces; the portal backend (xdg-desktop-portal with PipeWire and
/// libei) and the test backend implement them.
///
/// Threading: a backend may run threads of its own (PipeWire's loop, say).
/// The session runs a single poll loop: each source has a `wake_fd()` that
/// becomes readable when something new is pending, and the session then calls
/// the `take_*` method on its own thread. Input goes the other way, always
/// from the session thread.
namespace farland::platform {

/// A rectangle in desktop pixels.
struct Rect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;

    friend bool operator==(const Rect&, const Rect&) = default;
};

/// One plane of a dmabuf, as PipeWire's spa_data describes it.
struct DmabufPlane {
    int fd = -1;
    std::uint32_t offset = 0;
    std::uint32_t pitch = 0;

    friend bool operator==(const DmabufPlane&, const DmabufPlane&) = default;
};

/// A captured frame in GPU memory (docs/PLAN.md §3.3), for encoders that
/// read it without a copy. The descriptors belong to the source.
struct Dmabuf {
    /// A DRM fourcc (drm_fourcc.h) of packed 32-bit RGB.
    std::uint32_t drm_format = 0;
    /// The DRM format modifier of all planes.
    std::uint64_t modifier = 0;
    /// The whole buffer, which is the whole frame.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// Planes in use, 1..4 (compressed layouts add metadata planes).
    std::uint32_t plane_count = 0;
    std::array<DmabufPlane, 4> planes{};
    /// Changes when the source replaced its buffers (PipeWire renegotiated
    /// them). Consumers that cache imports by buffer drop them then, since
    /// the kernel may reuse the old buffers' identities.
    std::uint64_t generation = 0;

    friend bool operator==(const Dmabuf&, const Dmabuf&) = default;
};

/// One captured frame: BGRX in CPU memory as codec::ImageView describes,
/// or a dmabuf, or both.
///
/// Lifetime: `image` and `dmabuf` stay valid until the next take_frame() or
/// release_frame() on the same source. While the consumer holds a frame
/// with a dmabuf, the producer cannot reuse that buffer, so a consumer holds
/// one frame at a time and takes the next as soon as it is pending.
struct Frame {
    /// Empty (no data) for a frame that came only as a dmabuf; map_frame()
    /// reads it then.
    codec::ImageView image;
    /// What changed since the previous frame taken; empty means everything.
    std::vector<Rect> damage;
    std::uint64_t sequence = 0;
    /// The frame in GPU memory; only with FrameAccess::dmabuf.
    std::optional<Dmabuf> dmabuf;
};

/// What the consumer of a FrameSource needs.
enum class FrameAccess : std::uint8_t {
    /// Every frame with its pixels in CPU memory.
    cpu,
    /// Frames the source has in GPU memory come as Frame::dmabuf without
    /// CPU pixels (map_frame() reads one when needed); others still come
    /// with pixels.
    dmabuf,
};

class FrameSource {
public:
    FrameSource() = default;
    FrameSource(const FrameSource&) = delete;
    FrameSource& operator=(const FrameSource&) = delete;
    FrameSource(FrameSource&&) = delete;
    FrameSource& operator=(FrameSource&&) = delete;
    virtual ~FrameSource() = default;

    /// Readable while a frame is pending; -1 if the source never blocks.
    [[nodiscard]] virtual int wake_fd() const noexcept = 0;
    /// The newest frame, or nullopt if nothing arrived since the last call.
    /// Frames that were never taken are skipped, and their damage is merged
    /// into the next one.
    [[nodiscard]] virtual std::optional<Frame> take_frame() = 0;
    /// The desktop size in pixels; 0 x 0 before the first frame.
    [[nodiscard]] virtual std::pair<std::uint32_t, std::uint32_t> size() const = 0;

    /// Says what the consumer needs, from the next frame the source makes
    /// on. A frame made before may still come as a dmabuf after switching to
    /// `cpu`. Sources without dmabufs ignore it.
    virtual void set_access(FrameAccess access) { static_cast<void>(access); }
    /// The pixels of the frame taken last: its image, or its dmabuf read and
    /// converted now. nullopt if there is none, it cannot be read, or it was
    /// released. Valid as long as the frame. Sources whose frames always
    /// have an image need not implement it.
    [[nodiscard]] virtual std::optional<codec::ImageView> map_frame() { return std::nullopt; }
    /// Gives the frame taken last back to the source before the next
    /// take_frame(); its image and dmabuf must not be used afterwards.
    virtual void release_frame() {}
};

/// A cursor shape: straight-alpha BGRA, top-down, stride width * 4.
struct CursorImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t hotspot_x = 0;
    std::int32_t hotspot_y = 0;
    std::vector<std::byte> pixels;

    friend bool operator==(const CursorImage&, const CursorImage&) = default;
};

struct CursorUpdate {
    /// A new shape; nullopt when it did not change.
    std::optional<CursorImage> shape;
    /// The hotspot's position in desktop pixels; nullopt when it did not change.
    std::optional<std::pair<std::int32_t, std::int32_t>> position;
    /// False while the cursor is hidden.
    bool visible = true;
};

class CursorSource {
public:
    CursorSource() = default;
    CursorSource(const CursorSource&) = delete;
    CursorSource& operator=(const CursorSource&) = delete;
    CursorSource(CursorSource&&) = delete;
    CursorSource& operator=(CursorSource&&) = delete;
    virtual ~CursorSource() = default;

    [[nodiscard]] virtual int wake_fd() const noexcept = 0;
    /// Everything that changed since the last call, merged; nullopt if nothing did.
    [[nodiscard]] virtual std::optional<CursorUpdate> take_cursor() = 0;
};

/// Where client input goes. Keys and buttons are Linux evdev codes
/// (KEY_* and BTN_* of linux/input-event-codes.h), so the compositor applies
/// the keyboard layout (docs/PLAN.md §3.3).
class InputSink {
public:
    InputSink() = default;
    InputSink(const InputSink&) = delete;
    InputSink& operator=(const InputSink&) = delete;
    InputSink(InputSink&&) = delete;
    InputSink& operator=(InputSink&&) = delete;
    virtual ~InputSink() = default;

    virtual void key(std::uint32_t evdev_code, bool pressed) = 0;
    /// Absolute position in desktop pixels.
    virtual void pointer_motion_absolute(double x, double y) = 0;
    virtual void pointer_motion_relative(double dx, double dy) = 0;
    virtual void button(std::uint32_t evdev_button, bool pressed) = 0;
    /// Scroll in v120 units (120 per wheel notch); positive y scrolls down,
    /// positive x scrolls right.
    virtual void scroll_discrete(std::int32_t x_v120, std::int32_t y_v120) = 0;
    /// A character the client typed as Unicode (best effort, docs/PLAN.md §7);
    /// backends that cannot type text ignore it.
    virtual void text(char32_t codepoint) = 0;
    /// Ends one RDP input PDU, for backends that group events (libei frames).
    virtual void flush() = 0;
};

}  // namespace farland::platform
