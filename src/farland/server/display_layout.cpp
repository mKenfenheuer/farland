// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/base/log.hpp>
#include <farland/server/display_layout.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace farland::server {

namespace {

namespace disp = channels::disp;
namespace gfx = channels::rdpgfx;
constexpr std::string_view log_component = "server.display";

// The optional attributes of [MS-RDPEDISP] 2.2.2.2.1, which [MS-RDPBCGR]
// 2.2.1.3.9.1 gives the same ranges: a value outside its range is ignored.
void set_attributes(DisplayMonitor& m, std::uint32_t physical_width, std::uint32_t physical_height,
                    std::uint32_t orientation, std::uint32_t desktop_scale, std::uint32_t device_scale)
{
    const auto physical_ok = [](std::uint32_t mm) { return mm >= 10 && mm <= 10000; };
    if (physical_ok(physical_width) && physical_ok(physical_height)) {
        m.physical_width = physical_width;
        m.physical_height = physical_height;
    }
    if (orientation == disp::orientation::landscape || orientation == disp::orientation::portrait ||
        orientation == disp::orientation::landscape_flipped || orientation == disp::orientation::portrait_flipped) {
        m.orientation = orientation;
    }
    if (desktop_scale >= 100 && desktop_scale <= 500) {
        m.desktop_scale_factor = desktop_scale;
    }
    if (device_scale == 100 || device_scale == 140 || device_scale == 180) {
        m.device_scale_factor = device_scale;
    }
}

bool overlap(const PixelRect& a, const PixelRect& b) noexcept
{
    return std::uint64_t{a.x} < std::uint64_t{b.x} + b.width && std::uint64_t{b.x} < std::uint64_t{a.x} + a.width &&
           std::uint64_t{a.y} < std::uint64_t{b.y} + b.height && std::uint64_t{b.y} < std::uint64_t{a.y} + a.height;
}

bool size_ok(std::int64_t size, const DisplayLimits& limits) noexcept
{
    return std::cmp_greater_equal(size, limits.min_size) && std::cmp_less_equal(size, limits.max_size);
}

}  // namespace

Letterbox letterbox(const PixelRect& monitor, std::uint32_t width, std::uint32_t height)
{
    FARLAND_ASSERT(width > 0 && height > 0);
    std::uint32_t w = width;
    std::uint32_t h = height;
    if (w > monitor.width || h > monitor.height) {
        // Scale down to fit, rounding the other side to the nearest pixel.
        if (std::uint64_t{monitor.width} * height <= std::uint64_t{monitor.height} * width) {
            w = monitor.width;
            h = static_cast<std::uint32_t>(((std::uint64_t{height} * monitor.width) + (width / 2)) / width);
        } else {
            h = monitor.height;
            w = static_cast<std::uint32_t>(((std::uint64_t{width} * monitor.height) + (height / 2)) / height);
        }
        w = std::clamp<std::uint32_t>(w, 1, monitor.width);
        h = std::clamp<std::uint32_t>(h, 1, monitor.height);
    }
    Letterbox box;
    box.picture = {monitor.x + ((monitor.width - w) / 2), monitor.y + ((monitor.height - h) / 2), w, h};
    const auto& p = box.picture;
    const std::uint32_t right = monitor.x + monitor.width;
    const std::uint32_t bottom = monitor.y + monitor.height;
    if (p.y > monitor.y) {
        box.bars.push_back({monitor.x, monitor.y, monitor.width, p.y - monitor.y});
    }
    if (p.y + p.height < bottom) {
        box.bars.push_back({monitor.x, p.y + p.height, monitor.width, bottom - (p.y + p.height)});
    }
    if (p.x > monitor.x) {
        box.bars.push_back({monitor.x, p.y, p.x - monitor.x, p.height});
    }
    if (p.x + p.width < right) {
        box.bars.push_back({p.x + p.width, p.y, right - (p.x + p.width), p.height});
    }
    return box;
}

DisplayLayout DisplayLayout::single(std::uint32_t width, std::uint32_t height)
{
    FARLAND_ASSERT(width > 0 && height > 0);
    DisplayLayout layout;
    layout.monitors_.push_back(DisplayMonitor{.rect = {0, 0, width, height}, .primary = true});
    layout.width_ = width;
    layout.height_ = height;
    return layout;
}

DisplayLayout DisplayLayout::row(std::span<const std::pair<std::uint32_t, std::uint32_t>> sizes,
                                 const DisplayLimits& limits)
{
    DisplayLayout layout;
    for (const auto& [w, h] : sizes) {
        const std::uint32_t width = std::clamp<std::uint32_t>(w, 1, limits.max_extent);
        const std::uint32_t height = std::clamp<std::uint32_t>(h, 1, limits.max_extent);
        if (!layout.monitors_.empty() &&
            (layout.monitors_.size() >= limits.max_monitors || layout.width_ + width > limits.max_extent)) {
            break;
        }
        layout.monitors_.push_back(
            DisplayMonitor{.rect = {layout.width_, 0, width, height}, .primary = layout.monitors_.empty()});
        layout.width_ += width;
        layout.height_ = std::max(layout.height_, height);
    }
    if (layout.monitors_.empty()) {
        return single(1, 1);
    }
    return layout;
}

Result<DisplayLayout> DisplayLayout::from_disp(const disp::MonitorLayoutPdu& pdu, const DisplayLimits& limits)
{
    std::vector<Candidate> candidates;
    candidates.reserve(pdu.monitors.size());
    for (const auto& m : pdu.monitors) {
        // [MS-RDPEDISP] 2.2.2.2.1: Width and Height 200 to 8192, Width even.
        if (!size_ok(m.width, limits) || !size_ok(m.height, limits)) {
            return fail(Errc::invalid_value, "a monitor size is out of range");
        }
        if (m.width % 2 != 0) {
            return fail(Errc::invalid_value, "a monitor width is odd");
        }
        Candidate c{m.left, m.top, DisplayMonitor{.rect = {0, 0, m.width, m.height}, .primary = m.primary()}};
        set_attributes(c.monitor, m.physical_width, m.physical_height, m.orientation, m.desktop_scale_factor,
                       m.device_scale_factor);
        candidates.push_back(c);
    }
    return build(std::move(candidates), limits);
}

DisplayLayout DisplayLayout::from_client_data(const proto::gcc::ClientData& data, std::uint32_t width,
                                              std::uint32_t height, const DisplayLimits& limits)
{
    if (data.monitor && !data.monitor->monitors.empty()) {
        const auto& defs = data.monitor->monitors;
        const std::vector<proto::gcc::MonitorAttributes>* attributes = nullptr;
        if (data.monitor_ex && data.monitor_ex->monitors.size() == defs.size()) {
            attributes = &data.monitor_ex->monitors;
        }
        std::vector<Candidate> candidates;
        bool sizes_ok = true;
        for (std::size_t i = 0; i < defs.size(); ++i) {
            // [MS-RDPBCGR] 2.2.1.3.6.1: the coordinates are inclusive.
            const auto& def = defs[i];
            const std::int64_t w = std::int64_t{def.right} - def.left + 1;
            const std::int64_t h = std::int64_t{def.bottom} - def.top + 1;
            if (!size_ok(w, limits) || !size_ok(h, limits)) {
                sizes_ok = false;
                break;
            }
            Candidate c{def.left, def.top,
                        DisplayMonitor{.rect = {0, 0, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)},
                                       .primary = (def.flags & gfx::monitor_primary) != 0}};
            if (attributes != nullptr) {
                const auto& a = (*attributes)[i];
                set_attributes(c.monitor, a.physical_width, a.physical_height, a.orientation, a.desktop_scale_factor,
                               a.device_scale_factor);
            }
            candidates.push_back(c);
        }
        if (!sizes_ok) {
            log::warn(log_component, "ignoring the client's monitors: a monitor size is out of range");
        } else {
            auto layout = build(std::move(candidates), limits);
            if (layout) {
                return *std::move(layout);
            }
            log::warn(log_component, "ignoring the client's monitors: {}", layout.error().what);
        }
    }
    auto layout = single(std::clamp<std::uint32_t>(width, 1, limits.max_size),
                         std::clamp<std::uint32_t>(height, 1, limits.max_size));
    const auto& core = data.core;
    set_attributes(layout.monitors_.front(), core.desktop_physical_width.value_or(0),
                   core.desktop_physical_height.value_or(0), core.desktop_orientation.value_or(0),
                   core.desktop_scale_factor.value_or(0), core.device_scale_factor.value_or(0));
    return layout;
}

Result<DisplayLayout> DisplayLayout::build(std::vector<Candidate> candidates, const DisplayLimits& limits)
{
    if (candidates.empty()) {
        return fail(Errc::invalid_value, "a layout without monitors");
    }
    if (candidates.size() > std::min(limits.max_monitors, disp::max_monitors)) {
        return fail(Errc::limit_exceeded, "more monitors than the server takes");
    }
    const auto primaries = std::ranges::count_if(candidates, [](const Candidate& c) { return c.monitor.primary; });
    if (primaries > 1) {
        return fail(Errc::invalid_value, "more than one primary monitor");
    }
    if (primaries == 0) {
        // The primary monitor is the one at the origin ([MS-RDPEDISP] 2.2.2.2.1); else the first.
        const auto origin =
            std::ranges::find_if(candidates, [](const Candidate& c) { return c.left == 0 && c.top == 0; });
        (origin != candidates.end() ? *origin : candidates.front()).monitor.primary = true;
    }
    std::int64_t min_x = std::numeric_limits<std::int64_t>::max();
    std::int64_t min_y = std::numeric_limits<std::int64_t>::max();
    std::int64_t max_x = std::numeric_limits<std::int64_t>::min();
    std::int64_t max_y = std::numeric_limits<std::int64_t>::min();
    std::uint64_t area = 0;
    for (const auto& c : candidates) {
        min_x = std::min(min_x, c.left);
        min_y = std::min(min_y, c.top);
        max_x = std::max(max_x, c.left + c.monitor.rect.width);
        max_y = std::max(max_y, c.top + c.monitor.rect.height);
        area += std::uint64_t{c.monitor.rect.width} * c.monitor.rect.height;
    }
    if (std::cmp_greater(max_x - min_x, limits.max_extent) || std::cmp_greater(max_y - min_y, limits.max_extent)) {
        return fail(Errc::limit_exceeded, "the monitors span too large a desktop");
    }
    if (area > limits.max_area()) {
        return fail(Errc::limit_exceeded, "the monitors' total area is too large");
    }
    DisplayLayout layout;
    layout.width_ = static_cast<std::uint32_t>(max_x - min_x);
    layout.height_ = static_cast<std::uint32_t>(max_y - min_y);
    for (auto& c : candidates) {
        c.monitor.rect.x = static_cast<std::uint32_t>(c.left - min_x);
        c.monitor.rect.y = static_cast<std::uint32_t>(c.top - min_y);
        for (const auto& other : layout.monitors_) {
            if (overlap(other.rect, c.monitor.rect)) {
                return fail(Errc::invalid_value, "monitors overlap");
            }
        }
        layout.monitors_.push_back(c.monitor);
    }
    return layout;
}

std::uint64_t DisplayLayout::area() const noexcept
{
    std::uint64_t sum = 0;
    for (const auto& m : monitors_) {
        sum += std::uint64_t{m.rect.width} * m.rect.height;
    }
    return sum;
}

std::vector<gfx::MonitorDef> DisplayLayout::gfx_monitors() const
{
    std::vector<gfx::MonitorDef> defs;
    defs.reserve(monitors_.size());
    for (const auto& m : monitors_) {
        defs.push_back(gfx::MonitorDef{.left = static_cast<std::int32_t>(m.rect.x),
                                       .top = static_cast<std::int32_t>(m.rect.y),
                                       .right = static_cast<std::int32_t>(m.rect.x + m.rect.width - 1),
                                       .bottom = static_cast<std::int32_t>(m.rect.y + m.rect.height - 1),
                                       .flags = m.primary ? gfx::monitor_primary : 0});
    }
    return defs;
}

std::vector<std::size_t> DisplayLayout::screen_monitors(std::size_t screens) const
{
    if (screens == 0) {
        return {};
    }
    if (screens == 1) {
        const auto primary = std::ranges::find_if(monitors_, &DisplayMonitor::primary);
        return {primary != monitors_.end() ? static_cast<std::size_t>(primary - monitors_.begin()) : 0};
    }
    std::vector<std::size_t> order(monitors_.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::ranges::stable_sort(order, [this](std::size_t a, std::size_t b) {
        const auto& ra = monitors_[a].rect;
        const auto& rb = monitors_[b].rect;
        return std::pair(ra.x, ra.y) < std::pair(rb.x, rb.y);
    });
    order.resize(std::min(order.size(), screens));
    return order;
}

OutputLayout DisplayLayout::place(std::span<const std::pair<std::uint32_t, std::uint32_t>> screen_sizes) const
{
    OutputLayout out{.width = width_, .height = height_, .monitors = gfx_monitors(), .screens = {}, .fills = {}};
    const auto order = screen_monitors(screen_sizes.size());
    std::vector<bool> covered(monitors_.size());
    for (std::size_t screen = 0; screen < order.size(); ++screen) {
        const auto [w, h] = screen_sizes[screen];
        if (w == 0 || h == 0) {
            continue;
        }
        const std::size_t index = order[screen];
        covered[index] = true;
        const auto box = letterbox(monitors_[index].rect, w, h);
        out.screens.push_back(ScreenPlacement{.screen = screen, .width = w, .height = h, .target = box.picture});
        out.fills.insert(out.fills.end(), box.bars.begin(), box.bars.end());
    }
    for (std::size_t i = 0; i < monitors_.size(); ++i) {
        if (!covered[i]) {
            out.fills.push_back(monitors_[i].rect);
        }
    }
    return out;
}

}  // namespace farland::server
