// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/channels/disp.hpp>
#include <farland/channels/rdpgfx.hpp>
#include <farland/proto/gcc.hpp>
#include <farland/server/scroll_detector.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

/// The client's monitors and where the server's screens show on them
/// (docs/PLAN.md §3.3): the layout comes from CS_MONITOR at connect time or
/// from a Display Control layout PDU later, and each captured screen (a
/// monitor of the shared desktop, a virtual monitor, a test pattern) is put
/// on one client monitor, scaled down and centred where the sizes differ.
namespace farland::server {

/// What farland accepts as a layout. The values go to the client in the
/// DISPLAYCONTROL_CAPS_PDU ([MS-RDPEDISP] 2.2.2.1).
struct DisplayLimits {
    /// Every monitor gets its own surface and encoder, so this stays small.
    std::uint32_t max_monitors = 8;
    /// Each monitor's width and height ([MS-RDPEDISP] 2.2.2.2.1).
    std::uint32_t min_size = 200;
    std::uint32_t max_size = 8192;
    /// The total area is at most max_monitors * area_factor_a * area_factor_b
    /// pixels: eight 4K monitors.
    std::uint32_t area_factor_a = 3840;
    std::uint32_t area_factor_b = 2160;
    /// The bounding box of all monitors, in either direction.
    std::uint32_t max_extent = 16384;

    [[nodiscard]] std::uint64_t max_area() const noexcept
    {
        return std::uint64_t{max_monitors} * area_factor_a * area_factor_b;
    }
    [[nodiscard]] channels::disp::CapsPdu caps() const noexcept { return {max_monitors, area_factor_a, area_factor_b}; }
};

/// One client monitor. The optional attributes are 0 when the client did not
/// send them or sent a value outside the specification's range.
struct DisplayMonitor {
    /// In desktop pixels; the monitors' bounding box starts at 0,0.
    PixelRect rect;
    bool primary = false;
    std::uint32_t physical_width = 0;        ///< millimetres
    std::uint32_t physical_height = 0;       ///< millimetres
    std::uint32_t orientation = 0;           ///< degrees
    std::uint32_t desktop_scale_factor = 0;  ///< percent
    std::uint32_t device_scale_factor = 0;   ///< percent

    friend bool operator==(const DisplayMonitor&, const DisplayMonitor&) = default;
};

/// Where one screen's picture shows in the desktop.
struct ScreenPlacement {
    std::size_t screen = 0;  ///< index of the screen
    /// The picture's size.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// Where it shows: at its own size, or scaled down to fit its monitor.
    PixelRect target;

    [[nodiscard]] bool scaled() const noexcept { return target.width != width || target.height != height; }
    friend bool operator==(const ScreenPlacement&, const ScreenPlacement&) = default;
};

/// Everything an output (the GFX surfaces, or one desktop picture for bitmap
/// updates) needs to show the screens on the client's monitors.
struct OutputLayout {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// For ResetGraphics ([MS-RDPEGFX] 2.2.2.14), in desktop coordinates.
    std::vector<channels::rdpgfx::MonitorDef> monitors;
    std::vector<ScreenPlacement> screens;
    /// Black: the borders around scaled or smaller pictures, and monitors
    /// without a picture.
    std::vector<PixelRect> fills;

    friend bool operator==(const OutputLayout&, const OutputLayout&) = default;
};

/// A picture of `width` x `height` on `monitor`: at its own size when it
/// fits, else scaled down to fit with its aspect ratio kept, centred either
/// way. `bars` are the parts of the monitor it leaves uncovered.
struct Letterbox {
    PixelRect picture;
    std::vector<PixelRect> bars;
};
[[nodiscard]] Letterbox letterbox(const PixelRect& monitor, std::uint32_t width, std::uint32_t height);

class DisplayLayout {
public:
    /// One primary monitor; `width` and `height` at least 1.
    [[nodiscard]] static DisplayLayout single(std::uint32_t width, std::uint32_t height);

    /// A Display Control layout. Errors when it breaks [MS-RDPEDISP]
    /// 2.2.2.2.1 (sizes 200 to 8192, widths even) or `limits`, or when
    /// monitors overlap or more than one is primary. Optional attributes out
    /// of their range count as absent, as the specification says.
    [[nodiscard]] static Result<DisplayLayout> from_disp(const channels::disp::MonitorLayoutPdu& pdu,
                                                         const DisplayLimits& limits);

    /// The client's monitors from CS_MONITOR and CS_MONITOR_EX ([MS-RDPBCGR]
    /// 2.2.1.3.6, 2.2.1.3.9) under the same rules, except that widths may be
    /// odd. Without the block, or when it breaks the rules, one monitor of
    /// `width` x `height` (the client's desktop size, capped at
    /// limits.max_size) with CS_CORE's attributes.
    [[nodiscard]] static DisplayLayout from_client_data(const proto::gcc::ClientData& data, std::uint32_t width,
                                                        std::uint32_t height, const DisplayLimits& limits);

    /// Monitors of the given sizes side by side, left to right and top
    /// aligned, the first primary: a shared desktop's own screens, as one
    /// client window shows them. Stops before a monitor that would break
    /// `limits`; a size of 0 counts as 1.
    [[nodiscard]] static DisplayLayout row(std::span<const std::pair<std::uint32_t, std::uint32_t>> sizes,
                                           const DisplayLimits& limits);

    [[nodiscard]] const std::vector<DisplayMonitor>& monitors() const noexcept { return monitors_; }
    /// The bounding box of the monitors: the desktop size.
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    /// The monitors' total area in pixels.
    [[nodiscard]] std::uint64_t area() const noexcept;

    [[nodiscard]] std::vector<channels::rdpgfx::MonitorDef> gfx_monitors() const;

    /// The monitor each of `screens` screens shows on, in order: a single
    /// screen on the primary monitor, several from left to right (then top to
    /// bottom). Screens beyond the monitors show nowhere.
    [[nodiscard]] std::vector<std::size_t> screen_monitors(std::size_t screens) const;

    /// Places screens with the given picture sizes (0 x 0: no picture yet)
    /// on the monitors of screen_monitors(). A monitor without a picture is
    /// filled black.
    [[nodiscard]] OutputLayout place(std::span<const std::pair<std::uint32_t, std::uint32_t>> screen_sizes) const;

    friend bool operator==(const DisplayLayout&, const DisplayLayout&) = default;

private:
    struct Candidate {
        std::int64_t left = 0;
        std::int64_t top = 0;
        DisplayMonitor monitor;  ///< rect.x and rect.y are filled in by build()
    };
    [[nodiscard]] static Result<DisplayLayout> build(std::vector<Candidate> candidates, const DisplayLimits& limits);

    std::vector<DisplayMonitor> monitors_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

}  // namespace farland::server
