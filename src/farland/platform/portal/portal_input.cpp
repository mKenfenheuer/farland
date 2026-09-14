// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/portal/portal_bus.hpp>
#include <farland/platform/portal/portal_input.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace farland::platform::portal {

namespace {

constexpr std::string_view log_component = "platform.portal";
constexpr std::int32_t v120_per_step = 120;
/// org.freedesktop.portal.RemoteDesktop.NotifyPointerAxisDiscrete axes.
constexpr std::uint32_t axis_vertical = 0;
constexpr std::uint32_t axis_horizontal = 1;

bool has_size(const StreamRegion& region) noexcept
{
    return region.desktop.width > 0 && region.desktop.height > 0;
}

/// Distance from a point to a region (0 inside).
double distance(const StreamRegion& region, double x, double y) noexcept
{
    const auto& r = region.desktop;
    const double dx = std::max({static_cast<double>(r.x) - x, 0.0, x - (static_cast<double>(r.x) + r.width - 1)});
    const double dy = std::max({static_cast<double>(r.y) - y, 0.0, y - (static_cast<double>(r.y) + r.height - 1)});
    return std::hypot(dx, dy);
}

double scale(double offset, std::int32_t desktop_size, std::int32_t logical_size) noexcept
{
    return logical_size > 0 && desktop_size > 0 ? offset * logical_size / desktop_size : offset;
}

void remove_value(std::vector<std::uint32_t>& values, std::uint32_t value)
{
    std::erase(values, value);
}

}  // namespace

std::vector<StreamRegion> default_layout(std::span<const PortalStream> streams)
{
    std::int32_t min_x = std::numeric_limits<std::int32_t>::max();
    std::int32_t min_y = std::numeric_limits<std::int32_t>::max();
    for (const auto& stream : streams) {
        if (stream.position) {
            min_x = std::min(min_x, stream.position->first);
            min_y = std::min(min_y, stream.position->second);
        }
    }
    std::vector<StreamRegion> regions;
    std::int64_t right = 0;
    for (const auto& stream : streams) {
        StreamRegion region;
        region.node_id = stream.node_id;
        if (stream.size) {
            region.desktop.width = std::max(stream.size->first, 0);
            region.desktop.height = std::max(stream.size->second, 0);
        }
        if (stream.position) {
            region.desktop.x = static_cast<std::int32_t>(std::int64_t{stream.position->first} - min_x);
            region.desktop.y = static_cast<std::int32_t>(std::int64_t{stream.position->second} - min_y);
            right = std::max(right, std::int64_t{region.desktop.x} + region.desktop.width);
        }
        regions.push_back(region);
    }
    for (std::size_t i = 0; i < regions.size(); ++i) {
        if (!streams[i].position) {
            regions[i].desktop.x = static_cast<std::int32_t>(std::min<std::int64_t>(right, INT32_MAX));
            right += regions[i].desktop.width;
        }
    }
    return regions;
}

std::optional<StreamPoint> map_to_stream(std::span<const StreamRegion> regions, double x, double y)
{
    if (regions.empty()) {
        return std::nullopt;
    }
    const StreamRegion* best = nullptr;
    double best_distance = std::numeric_limits<double>::infinity();
    for (const auto& region : regions) {
        if (!has_size(region)) {
            continue;
        }
        const double d = distance(region, x, y);
        if (d < best_distance) {
            best = &region;
            best_distance = d;
        }
    }
    if (best == nullptr) {
        const auto& first = regions.front();
        return StreamPoint{first.node_id, x - first.desktop.x, y - first.desktop.y};
    }
    const auto& r = best->desktop;
    const double cx = std::clamp(x, static_cast<double>(r.x), static_cast<double>(r.x) + r.width - 1);
    const double cy = std::clamp(y, static_cast<double>(r.y), static_cast<double>(r.y) + r.height - 1);
    return StreamPoint{best->node_id, scale(cx - r.x, r.width, best->logical_width),
                       scale(cy - r.y, r.height, best->logical_height)};
}

PortalNotifyInput::PortalNotifyInput(PortalSession& session)
    : PortalNotifyInput(session, default_layout(session.streams()))
{
}

PortalNotifyInput::PortalNotifyInput(PortalSession& session, std::vector<StreamRegion> layout)
    : session_(session), layout_(std::move(layout))
{
}

PortalNotifyInput::~PortalNotifyInput()
{
    for (const auto code : std::vector(pressed_keys_)) {
        key(code, false);
    }
    for (const auto code : std::vector(pressed_buttons_)) {
        button(code, false);
    }
    flush();
}

template <class Args>
void PortalNotifyInput::notify(const char* method, const Args& args)
{
    auto* bus = session_.bus();
    if (bus == nullptr || session_.closed()) {
        return;
    }
    auto call = bus->new_call(detail::remote_desktop_interface, method);
    if (!call) {
        log::warn(log_component, "{}", call.error().message);
        return;
    }
    detail::MessageWriter writer(call->get());
    writer.object_path(session_.session_handle()).options({});
    args(writer);
    if (writer.status() < 0) {
        log::warn(log_component, "cannot build a {} call", method);
        return;
    }
    bus->send(call->get());
}

bool PortalNotifyInput::allowed(std::uint32_t device)
{
    if ((session_.devices() & device) != 0) {
        return true;
    }
    if ((warned_devices_ & device) == 0) {
        warned_devices_ |= device;
        log::warn(log_component, "the portal session has no {} access; dropping that input",
                  device == device_keyboard ? "keyboard" : "pointer");
    }
    return false;
}

void PortalNotifyInput::key(std::uint32_t evdev_code, bool pressed)
{
    if (!allowed(device_keyboard) || evdev_code > INT32_MAX) {
        return;
    }
    remove_value(pressed_keys_, evdev_code);
    if (pressed) {
        pressed_keys_.push_back(evdev_code);
    }
    notify("NotifyKeyboardKeycode",
           [&](detail::MessageWriter& w) { w.i32(static_cast<std::int32_t>(evdev_code)).u32(pressed ? 1 : 0); });
}

void PortalNotifyInput::pointer_motion_absolute(double x, double y)
{
    if (!allowed(device_pointer)) {
        return;
    }
    const auto point = map_to_stream(layout_, x, y);
    if (!point) {
        return;
    }
    notify("NotifyPointerMotionAbsolute",
           [&](detail::MessageWriter& w) { w.u32(point->node_id).f64(point->x).f64(point->y); });
}

void PortalNotifyInput::pointer_motion_relative(double dx, double dy)
{
    if (!allowed(device_pointer)) {
        return;
    }
    notify("NotifyPointerMotion", [&](detail::MessageWriter& w) { w.f64(dx).f64(dy); });
}

void PortalNotifyInput::button(std::uint32_t evdev_button, bool pressed)
{
    if (!allowed(device_pointer) || evdev_button > INT32_MAX) {
        return;
    }
    remove_value(pressed_buttons_, evdev_button);
    if (pressed) {
        pressed_buttons_.push_back(evdev_button);
    }
    notify("NotifyPointerButton",
           [&](detail::MessageWriter& w) { w.i32(static_cast<std::int32_t>(evdev_button)).u32(pressed ? 1 : 0); });
}

void PortalNotifyInput::scroll_axis(std::int32_t v120, std::int32_t& remainder, std::uint32_t axis)
{
    if (v120 == 0) {
        return;
    }
    if ((remainder > 0 && v120 < 0) || (remainder < 0 && v120 > 0)) {
        remainder = 0;
    }
    const std::int64_t total = std::int64_t{remainder} + v120;
    const std::int64_t steps = total / v120_per_step;  // rounds toward zero, the remainder keeps the sign
    remainder = static_cast<std::int32_t>(total - (steps * v120_per_step));
    if (steps != 0) {
        notify("NotifyPointerAxisDiscrete",
               [&](detail::MessageWriter& w) { w.u32(axis).i32(static_cast<std::int32_t>(steps)); });
    }
}

void PortalNotifyInput::scroll_discrete(std::int32_t x_v120, std::int32_t y_v120)
{
    if (!allowed(device_pointer)) {
        return;
    }
    scroll_axis(y_v120, scroll_y_, axis_vertical);
    scroll_axis(x_v120, scroll_x_, axis_horizontal);
}

void PortalNotifyInput::text(char32_t /*codepoint*/) {}

void PortalNotifyInput::flush()
{
    session_.process();
}

}  // namespace farland::platform::portal
