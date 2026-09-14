// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/platform/backend.hpp>
#include <farland/platform/portal/portal_session.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace farland::platform::portal {

/// Where a portal stream appears on the RDP desktop.
struct StreamRegion {
    std::uint32_t node_id = 0;
    /// In desktop pixels. Width or height 0: unknown size.
    Rect desktop;
    /// The stream's logical size, the coordinate space of
    /// NotifyPointerMotionAbsolute; 0 means the same as `desktop`.
    std::int32_t logical_width = 0;
    std::int32_t logical_height = 0;
};

/// A position in one stream's logical coordinates.
struct StreamPoint {
    std::uint32_t node_id = 0;
    double x = 0;
    double y = 0;
};

/// The desktop layout implied by the portal: streams keep their logical
/// position, shifted so the top-left-most one is at 0,0; streams without a
/// position (virtual monitors, windows) go to the right of the others, top
/// aligned. Desktop pixels equal logical pixels.
[[nodiscard]] std::vector<StreamRegion> default_layout(std::span<const PortalStream> streams);

/// The stream under a desktop position, in its logical coordinates. Points
/// outside every stream go to the nearest one, clamped to its edge. When no
/// region has a size, the first region takes the position unchanged
/// (relative to its origin). nullopt without regions.
[[nodiscard]] std::optional<StreamPoint> map_to_stream(std::span<const StreamRegion> regions, double x, double y);

/// Input through the portal's Notify* methods, for portals without
/// ConnectToEIS (RemoteDesktop version 1). Events go out asynchronously; the
/// portal's answers are dispatched by PortalSession::process() and flush(),
/// and failures are logged.
///
/// Call it from the thread that owns `session`, which must be started and
/// outlive this object. Never use it after connect_to_eis(): the portal then
/// rejects Notify* calls.
class PortalNotifyInput final : public InputSink {
public:
    /// Uses default_layout(session.streams()).
    explicit PortalNotifyInput(PortalSession& session);
    PortalNotifyInput(PortalSession& session, std::vector<StreamRegion> layout);
    PortalNotifyInput(const PortalNotifyInput&) = delete;
    PortalNotifyInput& operator=(const PortalNotifyInput&) = delete;
    PortalNotifyInput(PortalNotifyInput&&) = delete;
    PortalNotifyInput& operator=(PortalNotifyInput&&) = delete;
    /// Releases keys and buttons that are still down.
    ~PortalNotifyInput() override;

    /// Replaces the layout, e.g. when the capture side arranges streams itself.
    void set_layout(std::vector<StreamRegion> layout) { layout_ = std::move(layout); }

    void key(std::uint32_t evdev_code, bool pressed) override;
    void pointer_motion_absolute(double x, double y) override;
    void pointer_motion_relative(double dx, double dy) override;
    void button(std::uint32_t evdev_button, bool pressed) override;
    /// NotifyPointerAxisDiscrete steps; partial notches accumulate until they
    /// make a whole one, and are dropped when the direction changes.
    void scroll_discrete(std::int32_t x_v120, std::int32_t y_v120) override;
    /// Not possible through Notify*: ignored.
    void text(char32_t codepoint) override;
    /// Pushes out queued calls and dispatches answers (PortalSession::process()).
    void flush() override;

private:
    template <class Args>
    void notify(const char* method, const Args& args);
    [[nodiscard]] bool allowed(std::uint32_t device);
    void scroll_axis(std::int32_t v120, std::int32_t& remainder, std::uint32_t axis);

    PortalSession& session_;
    std::vector<StreamRegion> layout_;
    std::vector<std::uint32_t> pressed_keys_;
    std::vector<std::uint32_t> pressed_buttons_;
    std::int32_t scroll_x_ = 0;
    std::int32_t scroll_y_ = 0;
    std::uint32_t warned_devices_ = 0;
};

}  // namespace farland::platform::portal
