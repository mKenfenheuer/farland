// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/kwin/output_management.hpp>

#include <algorithm>
#include <kde-output-device-v2-client-protocol.h>
#include <kde-output-management-v2-client-protocol.h>
#include <wayland-client.h>

namespace farland::platform::kwin {

namespace {

constexpr std::string_view log_component = "platform.kwin";
constexpr std::uint32_t management_version = 18;
/// Events up to this version have handlers.
constexpr std::uint32_t device_version = 20;
/// 60 Hz, in mHz.
constexpr std::uint32_t refresh_rate = 60'000;
constexpr auto step_timeout = std::chrono::seconds(10);
/// How long to wait before asking again after KWin refused, and how often.
constexpr auto retry_delay = std::chrono::milliseconds(500);
constexpr int max_refusals = 3;
using Clock = std::chrono::steady_clock;

}  // namespace

struct OutputManagement::Mode {
    kde_output_device_mode_v2* proxy = nullptr;
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool removed = false;
};

struct OutputManagement::Device {
    OutputManagement* owner = nullptr;
    std::uint32_t global = 0;
    kde_output_device_v2* proxy = nullptr;
    std::string name;
    std::uint32_t capabilities = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    bool enabled = false;
    std::uint32_t priority = 0;
    std::vector<std::unique_ptr<Mode>> modes;
    Mode* current = nullptr;

    enum class Step : std::uint8_t {
        idle,
        custom_modes,  ///< A configuration with the new custom mode is being applied.
        new_mode,      ///< Waiting for KWin to announce the new mode.
        switch_mode,   ///< A configuration switching to the mode is being applied.
    };
    Step step = Step::idle;
    Clock::time_point step_started;
    std::optional<std::pair<std::int32_t, std::int32_t>> wanted;
    /// The last size KWin refused, not asked for again right away.
    std::optional<std::pair<std::int32_t, std::int32_t>> refused;
    /// Refusals of the size wanted now. KWin refuses a configuration while
    /// the outputs it covers are changing, which is exactly what happens when
    /// a client adds or removes a monitor, so a refusal is tried again.
    int refusals = 0;
    /// When to try again; unset while nothing is waiting to be retried.
    std::optional<Clock::time_point> retry_at;
    kde_output_configuration_v2* configuration = nullptr;
    kde_mode_list_v2* mode_list = nullptr;

    [[nodiscard]] Mode* mode_of_size(std::int32_t width, std::int32_t height) const
    {
        const auto it = std::ranges::find_if(
            modes, [&](const auto& mode) { return !mode->removed && mode->width == width && mode->height == height; });
        return it == modes.end() ? nullptr : it->get();
    }
    void drop_configuration()
    {
        if (configuration != nullptr) {
            kde_output_configuration_v2_destroy(configuration);
            configuration = nullptr;
        }
        if (mode_list != nullptr) {
            kde_mode_list_v2_destroy(mode_list);
            mode_list = nullptr;
        }
    }
};

/// What request_layout() asks for, and what was there before it.
struct OutputManagement::Layout {
    using Entry = OutputManagement::LayoutEntry;

    /// The outputs the session is to be laid out around, in order.
    std::vector<std::string> wanted;
    /// Every output as it was before the first request_layout().
    std::vector<Entry> before;
    /// Putting `before` back.
    bool restoring = false;
    kde_output_configuration_v2* configuration = nullptr;
    Clock::time_point started;
    int refusals = 0;
    std::optional<Clock::time_point> retry_at;

    void drop_configuration()
    {
        if (configuration != nullptr) {
            kde_output_configuration_v2_destroy(configuration);
            configuration = nullptr;
        }
    }
};

struct OutputManagement::Listeners {
    static Device& device(void* data) noexcept { return *static_cast<Device*>(data); }
    static Mode& mode(void* data) noexcept { return *static_cast<Mode*>(data); }

    static constexpr kde_output_device_mode_v2_listener mode_listener{
        .size =
            [](void* data, kde_output_device_mode_v2* /*mode*/, std::int32_t width, std::int32_t height) {
                mode(data).width = width;
                mode(data).height = height;
            },
        .refresh = ignore_event,
        .preferred = ignore_event,
        .removed = [](void* data, kde_output_device_mode_v2* /*mode*/) { mode(data).removed = true; },
        .flags = ignore_event,
    };

    static void mode_added(void* data, kde_output_device_v2* /*device*/, kde_output_device_mode_v2* proxy)
    {
        auto added = std::make_unique<Mode>();
        added->proxy = proxy;
        kde_output_device_mode_v2_add_listener(proxy, &mode_listener, added.get());
        device(data).modes.push_back(std::move(added));
    }
    static void current_mode(void* data, kde_output_device_v2* /*device*/, kde_output_device_mode_v2* proxy)
    {
        auto& d = device(data);
        const auto it = std::ranges::find(d.modes, proxy, &Mode::proxy);
        d.current = it == d.modes.end() ? nullptr : it->get();
    }
    static void done(void* data, kde_output_device_v2* /*device*/)
    {
        auto& d = device(data);
        std::erase_if(d.modes, [&d](const auto& m) {
            if (!m->removed || m.get() == d.current) {
                return false;
            }
            kde_output_device_mode_v2_destroy(m->proxy);
            return true;
        });
        if (d.step == Device::Step::new_mode || d.step == Device::Step::idle) {
            d.owner->advance(d);
        }
        // The outputs changed: KWin may have laid the session out by itself
        // (an output came or went), so the layout is asked for again.
        d.owner->apply_layout();
    }

    static constexpr kde_output_device_v2_listener device_listener{
        .geometry =
            [](void* data, kde_output_device_v2* /*device*/, std::int32_t x, std::int32_t y, std::int32_t /*width*/,
               std::int32_t /*height*/, std::int32_t /*subpixel*/, const char* /*make*/, const char* /*model*/,
               std::int32_t /*transform*/) {
                device(data).x = x;
                device(data).y = y;
            },
        .current_mode = &current_mode,
        .mode = &mode_added,
        .done = &done,
        .scale = ignore_event,
        .edid = ignore_event,
        .enabled = [](void* data, kde_output_device_v2* /*device*/,
                      std::int32_t enabled) { device(data).enabled = enabled != 0; },
        .uuid = ignore_event,
        .serial_number = ignore_event,
        .eisa_id = ignore_event,
        .capabilities = [](void* data, kde_output_device_v2* /*device*/,
                           std::uint32_t flags) { device(data).capabilities = flags; },
        .overscan = ignore_event,
        .vrr_policy = ignore_event,
        .rgb_range = ignore_event,
        .name = [](void* data, kde_output_device_v2* /*device*/,
                   const char* name) { device(data).name = name != nullptr ? name : ""; },
        .high_dynamic_range = ignore_event,
        .sdr_brightness = ignore_event,
        .wide_color_gamut = ignore_event,
        .auto_rotate_policy = ignore_event,
        .icc_profile_path = ignore_event,
        .brightness_metadata = ignore_event,
        .brightness_overrides = ignore_event,
        .sdr_gamut_wideness = ignore_event,
        .color_profile_source = ignore_event,
        .brightness = ignore_event,
        .color_power_tradeoff = ignore_event,
        .dimming = ignore_event,
        .replication_source = ignore_event,
        .ddc_ci_allowed = ignore_event,
        .max_bits_per_color = ignore_event,
        .max_bits_per_color_range = ignore_event,
        .automatic_max_bits_per_color_limit = ignore_event,
        .edr_policy = ignore_event,
        .sharpness = ignore_event,
        .priority = [](void* data, kde_output_device_v2* /*device*/,
                       std::uint32_t priority) { device(data).priority = priority; },
        .auto_brightness = ignore_event,
    };

    static OutputManagement& management(void* data) noexcept { return *static_cast<OutputManagement*>(data); }

    static constexpr kde_output_configuration_v2_listener layout_listener{
        .applied =
            [](void* data, kde_output_configuration_v2* /*configuration*/) { management(data).layout_done(true); },
        .failed = [](void* data, kde_output_configuration_v2* /*configuration*/) { management(data).layout_done(false); },
        .failure_reason =
            [](void* /*data*/, kde_output_configuration_v2* /*configuration*/, const char* reason) {
                log::debug(log_component, "KWin cannot lay the session's outputs out: {}",
                           reason != nullptr ? reason : "");
            },
    };

    static constexpr kde_output_configuration_v2_listener configuration_listener{
        .applied =
            [](void* data, kde_output_configuration_v2* /*configuration*/) {
                device(data).owner->configuration_done(device(data), true);
            },
        .failed =
            [](void* data, kde_output_configuration_v2* /*configuration*/) {
                device(data).owner->configuration_done(device(data), false);
            },
        .failure_reason =
            [](void* data, kde_output_configuration_v2* /*configuration*/, const char* reason) {
                log::warn(log_component, "KWin cannot configure output {}: {}", device(data).name,
                          reason != nullptr ? reason : "");
            },
    };
};

Result<std::unique_ptr<OutputManagement>> OutputManagement::create(WaylandConnection& connection)
{
    const auto* global = connection.find_global(kde_output_management_v2_interface.name);
    if (global == nullptr || global->version < custom_modes_version) {
        return fail(Errc::unsupported, "KWin offers no kde_output_management_v2 with custom modes");
    }
    auto* manager = static_cast<kde_output_management_v2*>(
        connection.bind(*global, &kde_output_management_v2_interface, management_version));
    std::unique_ptr<OutputManagement> management(new OutputManagement(connection, manager));
    for (const auto& g : connection.globals()) {
        management->add_device(g);
    }
    auto* raw = management.get();
    connection.set_global_listeners([raw](const WaylandGlobal& g) { raw->add_device(g); },
                                    [raw](const WaylandGlobal& g) { raw->remove_device(g); });
    if (!connection.roundtrip(std::chrono::seconds(5))) {
        return fail(Errc::io, "KWin did not describe its output devices");
    }
    return management;
}

OutputManagement::OutputManagement(WaylandConnection& connection, kde_output_management_v2* manager) noexcept
    : connection_(connection), manager_(manager)
{
}

OutputManagement::~OutputManagement()
{
    // A desktop that laid the session out and went without putting it back
    // would leave the session's screens in the order it chose for a client
    // that is no longer there.
    if (layout_) {
        restore_layout(std::chrono::seconds(1));
    }
    connection_.set_global_listeners({}, {});
    for (auto& device : devices_) {
        device->drop_configuration();
        for (auto& mode : device->modes) {
            kde_output_device_mode_v2_destroy(mode->proxy);
        }
        kde_output_device_v2_destroy(device->proxy);
    }
    kde_output_management_v2_destroy(manager_);
}

void OutputManagement::add_device(const WaylandGlobal& global)
{
    if (global.interface != kde_output_device_v2_interface.name) {
        return;
    }
    auto device = std::make_unique<Device>();
    device->owner = this;
    device->global = global.name;
    device->proxy =
        static_cast<kde_output_device_v2*>(connection_.bind(global, &kde_output_device_v2_interface, device_version));
    kde_output_device_v2_add_listener(device->proxy, &Listeners::device_listener, device.get());
    devices_.push_back(std::move(device));
}

void OutputManagement::remove_device(const WaylandGlobal& global)
{
    std::erase_if(devices_, [&global](const auto& device) {
        if (device->global != global.name) {
            return false;
        }
        device->drop_configuration();
        for (auto& mode : device->modes) {
            kde_output_device_mode_v2_destroy(mode->proxy);
        }
        kde_output_device_v2_destroy(device->proxy);
        return true;
    });
}

OutputManagement::Device* OutputManagement::find(std::string_view name) const
{
    const auto it = std::ranges::find_if(devices_, [name](const auto& d) { return d->name == name; });
    return it == devices_.end() ? nullptr : it->get();
}

bool OutputManagement::resizable(std::string_view name) const
{
    const auto* device = find(name);
    return device != nullptr && (device->capabilities & KDE_OUTPUT_DEVICE_V2_CAPABILITY_CUSTOM_MODES) != 0;
}

std::optional<std::pair<std::int32_t, std::int32_t>> OutputManagement::size(std::string_view name) const
{
    const auto* device = find(name);
    if (device == nullptr || device->current == nullptr) {
        return std::nullopt;
    }
    return std::pair{device->current->width, device->current->height};
}

void OutputManagement::request_size(std::string_view name, std::int32_t width, std::int32_t height)
{
    auto* device = find(name);
    if (device == nullptr || !resizable(name) || width <= 0 || height <= 0) {
        return;
    }
    const auto wanted = std::pair{width, height};
    if (device->wanted != wanted) {
        device->refusals = 0;
        device->retry_at.reset();
    }
    device->wanted = wanted;
    if (device->refused == device->wanted) {
        return;
    }
    device->refused.reset();
    if (device->step == Device::Step::idle) {
        advance(*device);
    }
}

bool OutputManagement::enabled(std::string_view name) const
{
    const auto* device = find(name);
    return device != nullptr && device->enabled;
}

void OutputManagement::request_layout(std::span<const std::string> outputs)
{
    if (outputs.empty()) {
        return;
    }
    if (!layout_) {
        layout_ = std::make_unique<Layout>();
        // What the session showed before we laid it out, to put back when the
        // desktop goes: whoever is at the machine gets their screens as they
        // were.
        for (const auto& device : devices_) {
            layout_->before.push_back(
                LayoutEntry{device->name, device->enabled, device->x, device->y, device->priority});
        }
    }
    layout_->wanted.assign(outputs.begin(), outputs.end());
    layout_->restoring = false;
    layout_->refusals = 0;
    layout_->retry_at.reset();
    apply_layout();
}

void OutputManagement::restore_layout(std::chrono::milliseconds timeout)
{
    if (!layout_ || layout_->before.empty()) {
        layout_.reset();
        return;
    }
    layout_->wanted.clear();
    layout_->restoring = true;
    layout_->refusals = 0;
    layout_->retry_at.reset();
    apply_layout();
    const auto deadline = Clock::now() + timeout;
    while (layout_ && (layout_->configuration != nullptr || layout_->retry_at) && Clock::now() < deadline) {
        static_cast<void>(connection_.roundtrip(std::chrono::milliseconds(100)));
        check_timeouts();
    }
    layout_.reset();
}

std::vector<OutputManagement::LayoutEntry> OutputManagement::layout_target() const
{
    std::vector<LayoutEntry> target;
    if (!layout_) {
        return target;
    }
    if (layout_->restoring) {
        return layout_->before;
    }
    // Beside the screens that stay as they are, so that nothing overlaps.
    std::int32_t x = 0;
    for (const auto& device : devices_) {
        if (!device->enabled || std::ranges::find(layout_->wanted, device->name) != layout_->wanted.end()) {
            continue;
        }
        const auto width = device->current != nullptr ? device->current->width : 0;
        x = std::max(x, device->x + width);
    }
    std::uint32_t priority = 1;
    for (const auto& name : layout_->wanted) {
        const auto* device = find(name);
        if (device == nullptr) {
            return {};  // KWin has not announced it yet: ask again later
        }
        const auto width = device->current != nullptr ? device->current->width : 0;
        target.push_back(LayoutEntry{name, true, x, 0, priority});
        x += width;
        ++priority;
    }
    // The other screens keep where they are, but come after ours in the
    // output order: a configuration that gives only some outputs a priority
    // leaves KWin's own order in place, and the panel stays where it was.
    for (const auto& device : devices_) {
        if (!device->enabled || std::ranges::find(layout_->wanted, device->name) != layout_->wanted.end()) {
            continue;
        }
        target.push_back(LayoutEntry{device->name, true, device->x, device->y, priority});
        ++priority;
    }
    return target;
}

bool OutputManagement::layout_reached() const
{
    const auto target = layout_target();
    if (target.empty()) {
        return false;
    }
    // The order of the priorities is what matters, not their numbers: KWin
    // numbers them its own way, and comparing the numbers would ask for the
    // same configuration for ever. The first of them is the primary screen,
    // which is where the panel and new windows go.
    bool first = true;
    std::uint32_t previous = 0;
    for (const auto& entry : target) {
        const auto* device = find(entry.name);
        if (device == nullptr) {
            return false;
        }
        if (device->enabled != entry.enabled || (entry.enabled && (device->x != entry.x || device->y != entry.y))) {
            return false;
        }
        if (entry.priority == 0) {
            continue;
        }
        if (!first && device->priority <= previous) {
            return false;
        }
        previous = device->priority;
        first = false;
    }
    return true;
}

void OutputManagement::apply_layout()
{
    if (!layout_ || layout_->configuration != nullptr || layout_->retry_at) {
        return;  // one at a time
    }
    const auto target = layout_target();
    if (target.empty() || layout_reached()) {
        return;
    }
    layout_->configuration = kde_output_management_v2_create_configuration(manager_);
    kde_output_configuration_v2_add_listener(layout_->configuration, &Listeners::layout_listener, this);
    for (const auto& entry : target) {
        auto* device = find(entry.name);
        if (device == nullptr) {
            continue;
        }
        kde_output_configuration_v2_enable(layout_->configuration, device->proxy, entry.enabled ? 1 : 0);
        if (!entry.enabled) {
            continue;
        }
        kde_output_configuration_v2_position(layout_->configuration, device->proxy, entry.x, entry.y);
        if (entry.priority != 0) {
            kde_output_configuration_v2_set_priority(layout_->configuration, device->proxy, entry.priority);
        }
    }
    layout_->started = Clock::now();
    kde_output_configuration_v2_apply(layout_->configuration);
    connection_.flush();
}

void OutputManagement::layout_done(bool applied)
{
    if (!layout_) {
        return;
    }
    layout_->drop_configuration();
    if (!applied) {
        if (++layout_->refusals < max_refusals) {
            // Another output was coming or going; asking again once that
            // settled works.
            layout_->retry_at = Clock::now() + retry_delay;
            return;
        }
        log::warn(log_component,
                  "KWin will not lay the session's outputs out (it refuses every configuration while the session is "
                  "off its seat); the screens keep the order they have");
        layout_->retry_at.reset();
        return;
    }
    layout_->refusals = 0;
    log::info(log_component, "the session is laid out around {}",
              layout_->restoring ? "the screens it had before" : "the screens of this connection");
}

bool OutputManagement::busy() const
{
    return std::ranges::any_of(devices_,
                               [](const auto& d) { return d->step != Device::Step::idle || d->retry_at.has_value(); }) ||
           (layout_ && (layout_->configuration != nullptr || layout_->retry_at.has_value()));
}

void OutputManagement::check_timeouts()
{
    for (auto& device : devices_) {
        if (device->step != Device::Step::idle && Clock::now() - device->step_started > step_timeout) {
            log::warn(log_component, "KWin did not resize output {} in time", device->name);
            device->drop_configuration();
            device->step = Device::Step::idle;
            device->retry_at.reset();
            device->refused = device->wanted;
            continue;
        }
        if (device->step == Device::Step::idle && device->retry_at && Clock::now() >= *device->retry_at) {
            device->retry_at.reset();
            advance(*device);
        }
    }
    if (!layout_) {
        return;
    }
    if (layout_->configuration != nullptr && Clock::now() - layout_->started > step_timeout) {
        log::warn(log_component, "KWin did not lay the session's outputs out in time");
        layout_->drop_configuration();
        layout_->retry_at.reset();
        return;
    }
    if (layout_->retry_at && Clock::now() >= *layout_->retry_at) {
        layout_->retry_at.reset();
        apply_layout();
    }
}

void OutputManagement::advance(Device& device)
{
    if (!device.wanted || device.refused == device.wanted) {
        device.step = Device::Step::idle;
        return;
    }
    const auto [width, height] = *device.wanted;
    if (device.current != nullptr && device.current->width == width && device.current->height == height) {
        device.step = Device::Step::idle;
        device.wanted.reset();
        return;
    }
    auto* mode = device.mode_of_size(width, height);
    if (mode == nullptr && device.step == Device::Step::new_mode) {
        return;  // not announced yet
    }
    device.step_started = Clock::now();
    device.configuration = kde_output_management_v2_create_configuration(manager_);
    kde_output_configuration_v2_add_listener(device.configuration, &Listeners::configuration_listener, &device);
    if (mode != nullptr) {
        log::debug(log_component, "switching output {} to {}x{}", device.name, width, height);
        kde_output_configuration_v2_mode(device.configuration, device.proxy, mode->proxy);
        device.step = Device::Step::switch_mode;
    } else {
        // The custom mode list replaces the previous one: only the new size.
        log::debug(log_component, "adding a {}x{} mode to output {}", width, height, device.name);
        device.mode_list = kde_output_management_v2_create_mode_list(manager_);
        kde_mode_list_v2_set_resolution(device.mode_list, static_cast<std::uint32_t>(width),
                                        static_cast<std::uint32_t>(height));
        kde_mode_list_v2_set_refresh_rate(device.mode_list, refresh_rate);
        kde_mode_list_v2_set_reduced_blanking(device.mode_list, 1);
        kde_mode_list_v2_add_mode(device.mode_list);
        kde_output_configuration_v2_set_custom_modes(device.configuration, device.proxy, device.mode_list);
        device.step = Device::Step::custom_modes;
    }
    kde_output_configuration_v2_apply(device.configuration);
    connection_.flush();
}

void OutputManagement::configuration_done(Device& device, bool applied)
{
    const auto step = device.step;
    device.drop_configuration();
    if (!applied) {
        device.step = Device::Step::idle;
        if (++device.refusals < max_refusals) {
            // Usually another output was coming or going; asking again once
            // that settled works.
            log::debug(log_component, "KWin refused to resize output {}; asking again", device.name);
            device.retry_at = Clock::now() + retry_delay;
            return;
        }
        log::warn(log_component, "KWin refused to resize output {} to {}x{}", device.name,
                  device.wanted ? device.wanted->first : 0, device.wanted ? device.wanted->second : 0);
        device.refused = device.wanted;
        device.retry_at.reset();
        return;
    }
    device.refusals = 0;
    device.step = step == Device::Step::custom_modes ? Device::Step::new_mode : Device::Step::idle;
    advance(device);
}

}  // namespace farland::platform::kwin
