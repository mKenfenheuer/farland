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
    }

    static constexpr kde_output_device_v2_listener device_listener{
        .geometry = ignore_event,
        .current_mode = &current_mode,
        .mode = &mode_added,
        .done = &done,
        .scale = ignore_event,
        .edid = ignore_event,
        .enabled = ignore_event,
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
        .priority = ignore_event,
        .auto_brightness = ignore_event,
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
    device->wanted = std::pair{width, height};
    if (device->refused == device->wanted) {
        return;
    }
    device->refused.reset();
    if (device->step == Device::Step::idle) {
        advance(*device);
    }
}

bool OutputManagement::busy() const
{
    return std::ranges::any_of(devices_, [](const auto& d) { return d->step != Device::Step::idle; });
}

void OutputManagement::check_timeouts()
{
    for (auto& device : devices_) {
        if (device->step != Device::Step::idle && Clock::now() - device->step_started > step_timeout) {
            log::warn(log_component, "KWin did not resize output {} in time", device->name);
            device->drop_configuration();
            device->step = Device::Step::idle;
            device->refused = device->wanted;
        }
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
        log::warn(log_component, "KWin refused to resize output {} to {}x{}", device.name,
                  device.wanted ? device.wanted->first : 0, device.wanted ? device.wanted->second : 0);
        device.refused = device.wanted;
        device.step = Device::Step::idle;
        return;
    }
    device.step = step == Device::Step::custom_modes ? Device::Step::new_mode : Device::Step::idle;
    advance(device);
}

}  // namespace farland::platform::kwin
