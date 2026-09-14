// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/codec/image.hpp>

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

/// One captured frame in CPU memory, BGRX as codec::ImageView describes.
/// `image` stays valid until the next take_frame() on the same source.
struct Frame {
    codec::ImageView image;
    /// What changed since the previous frame taken; empty means everything.
    std::vector<Rect> damage;
    std::uint64_t sequence = 0;
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
