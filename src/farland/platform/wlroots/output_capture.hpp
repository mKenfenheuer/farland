// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/platform/backend.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct ext_image_copy_capture_manager_v1;
struct ext_output_image_capture_source_manager_v1;
struct wl_output;
struct wl_pointer;
struct wl_shm;
struct zwlr_screencopy_manager_v1;

namespace farland::platform::wayland {
class Connection;
}

namespace farland::platform::wlroots {

/// Damage gathered over one or more captures: rectangles, or everything.
/// Beyond `max_rects` rectangles it keeps their bounding box instead.
class Damage {
public:
    static constexpr std::size_t max_rects = 32;

    void add(const Rect& rect);
    void add_all() noexcept { all_ = true; }
    void merge(const Damage& other);
    void clear() noexcept
    {
        rects_.clear();
        all_ = false;
    }
    [[nodiscard]] bool all() const noexcept { return all_; }
    [[nodiscard]] bool empty() const noexcept { return !all_ && rects_.empty(); }
    [[nodiscard]] const std::vector<Rect>& rects() const noexcept { return rects_; }
    /// As Frame::damage has it: empty for everything.
    [[nodiscard]] std::vector<Rect> for_frame() const { return all_ ? std::vector<Rect>{} : rects_; }

private:
    std::vector<Rect> rects_;
    bool all_ = false;
};

/// Straight-alpha BGRA from premultiplied pixels (in place), as CursorImage
/// wants them.
void unpremultiply(std::span<std::byte> bgra) noexcept;

/// Captures one output (docs/PLAN.md §3.3) into shared memory, damage-driven:
/// a capture is always outstanding, and the compositor completes it when the
/// output changed.
///
/// - ext-image-copy-capture-v1 on an ext_image_capture_source_v1 of the
///   output where the compositor has both (sway and labwc). With a wl_pointer
///   the cursor comes from a pointer cursor session (its position, hotspot
///   and image, cropped to its visible pixels) and stays out of the frames;
///   wlroots' headless outputs take such "hardware" cursors.
/// - zwlr_screencopy_manager_v1 otherwise (cage 0.2): copy_with_damage, with
///   the cursor painted into the frames, so cursor() is null.
///
/// Frames live in up to three buffers (the one the consumer holds, a newer
/// one waiting, and the one being captured into); a waiting frame that is
/// replaced merges its damage into the newer one. Frames are XRGB8888 or
/// ARGB8888, which are BGRX in memory.
///
/// Threading: the session thread; events arrive through the connection's
/// dispatch.
class OutputCapture {
public:
    struct Protocols {
        /// Both or neither; preferred over screencopy.
        ext_image_copy_capture_manager_v1* image_copy = nullptr;
        ext_output_image_capture_source_manager_v1* output_sources = nullptr;
        zwlr_screencopy_manager_v1* screencopy = nullptr;
        wl_shm* shm = nullptr;
    };

    /// Starts capturing `output`. `pointer` (may be null) is the seat's
    /// pointer for the cursor session.
    [[nodiscard]] static Result<std::unique_ptr<OutputCapture>> create(wayland::Connection& connection,
                                                                       wl_output* output, wl_pointer* pointer,
                                                                       const Protocols& protocols, std::string name);
    OutputCapture(const OutputCapture&) = delete;
    OutputCapture& operator=(const OutputCapture&) = delete;
    OutputCapture(OutputCapture&&) = delete;
    OutputCapture& operator=(OutputCapture&&) = delete;
    ~OutputCapture();

    [[nodiscard]] FrameSource& frames() noexcept;
    /// Null when the cursor is part of the frames.
    [[nodiscard]] CursorSource* cursor() noexcept;
    /// The capture stopped for good (the output went away, or captures
    /// keep failing).
    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] const std::string& error() const noexcept;
    /// "ext-image-copy-capture-v1" or "wlr-screencopy-unstable-v1".
    [[nodiscard]] std::string_view protocol() const noexcept;

    struct Impl;

private:
    explicit OutputCapture(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace farland::platform::wlroots
