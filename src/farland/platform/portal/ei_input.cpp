// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/portal/ei_input.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <fcntl.h>
#include <libei.h>
#include <limits>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unistd.h>
#include <utility>

namespace farland::platform::portal {

namespace {

constexpr std::string_view component = "platform.ei";

constexpr std::array all_capabilities{
    EiInput::Capability::keyboard, EiInput::Capability::pointer, EiInput::Capability::pointer_absolute,
    EiInput::Capability::button,   EiInput::Capability::scroll,  EiInput::Capability::touch,
};

constexpr std::uint32_t bit(EiInput::Capability capability) noexcept
{
    return std::uint32_t{1} << std::to_underlying(capability);
}

constexpr enum ei_device_capability to_ei(EiInput::Capability capability) noexcept
{
    switch (capability) {
    case EiInput::Capability::keyboard:
        return EI_DEVICE_CAP_KEYBOARD;
    case EiInput::Capability::pointer:
        return EI_DEVICE_CAP_POINTER;
    case EiInput::Capability::pointer_absolute:
        return EI_DEVICE_CAP_POINTER_ABSOLUTE;
    case EiInput::Capability::button:
        return EI_DEVICE_CAP_BUTTON;
    case EiInput::Capability::scroll:
        return EI_DEVICE_CAP_SCROLL;
    case EiInput::Capability::touch:
        return EI_DEVICE_CAP_TOUCH;
    }
    return EI_DEVICE_CAP_KEYBOARD;
}

std::string capability_names(std::uint32_t caps)
{
    constexpr std::array<std::string_view, all_capabilities.size()> names{
        "keyboard", "pointer", "pointer-absolute", "button", "scroll", "touch",
    };
    std::string text;
    for (const auto capability : all_capabilities) {
        if ((caps & bit(capability)) != 0) {
            if (!text.empty()) {
                text += ',';
            }
            text += names.at(std::to_underlying(capability));
        }
    }
    return text.empty() ? std::string("none") : text;
}

std::string_view safe(const char* text) noexcept
{
    return text != nullptr ? std::string_view(text) : std::string_view();
}

void log_handler(struct ei* /*context*/, enum ei_log_priority priority, const char* message,
                 struct ei_log_context* /*log_context*/)
{
    // libei's info messages are connection chatter; they go to debug.
    log::Level level = log::Level::debug;
    if (priority >= EI_LOG_PRIORITY_ERROR) {
        level = log::Level::error;
    } else if (priority >= EI_LOG_PRIORITY_WARNING) {
        level = log::Level::warn;
    }
    if (!log::enabled(level)) {
        return;
    }
    std::string_view text = safe(message);
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
        text.remove_suffix(1);
    }
    log::write(level, component, text);
}

struct ei* new_context() noexcept
{
    struct ei* context = ei_new_sender(nullptr);
    if (context == nullptr) {
        return nullptr;
    }
    ei_configure_name(context, "farland");
    ei_log_set_handler(context, &log_handler);
    ei_log_set_priority(context, log::enabled(log::Level::debug) ? EI_LOG_PRIORITY_DEBUG : EI_LOG_PRIORITY_WARNING);
    return context;
}

struct EventDeleter {
    void operator()(struct ei_event* event) const noexcept { ei_event_unref(event); }
};
using EventPtr = std::unique_ptr<struct ei_event, EventDeleter>;

}  // namespace

/// A region of an absolute device, copied when the device is added (regions
/// are constant for a device's lifetime).
struct Region {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    double scale = 1;
    std::string mapping_id;
};

/// A key or button whose press went to the compositor and whose release has not.
struct Held {
    bool key = false;
    std::uint32_t code = 0;
    /// The client released it while the device was paused; the release is
    /// sent when the device resumes.
    bool released = false;
};

struct EiInput::Device {
    struct ei_device* handle = nullptr;
    std::string name;
    std::uint32_t caps = 0;
    /// Resumed by the server, and emulating: events go through.
    bool resumed = false;
    /// Got events since the last frame.
    bool dirty = false;
    std::vector<Region> regions;
    std::vector<Held> held;

    [[nodiscard]] bool has(Capability capability) const noexcept { return (caps & bit(capability)) != 0; }
};

/// Where a region of an absolute device lies in the desktop.
struct EiInput::Target {
    Device* device = nullptr;
    const Region* region = nullptr;
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;

    [[nodiscard]] bool contains(double px, double py) const noexcept
    {
        return px >= x && px < x + width && py >= y && py < y + height;
    }

    [[nodiscard]] double distance_squared(double px, double py) const noexcept
    {
        const double dx = std::max({x - px, 0.0, px - (x + width)});
        const double dy = std::max({y - py, 0.0, py - (y + height)});
        return (dx * dx) + (dy * dy);
    }

    /// The target containing the point, or else the nearest one; null when
    /// there are none.
    [[nodiscard]] static const Target* nearest(const std::vector<Target>& targets, double px, double py) noexcept
    {
        const Target* best = nullptr;
        double best_distance = std::numeric_limits<double>::infinity();
        for (const auto& target : targets) {
            if (target.contains(px, py)) {
                return &target;
            }
            if (const double d = target.distance_squared(px, py); best == nullptr || d < best_distance) {
                best = &target;
                best_distance = d;
            }
        }
        return best;
    }

    /// Desktop pixels to the region's logical coordinates, clamped into it.
    [[nodiscard]] std::pair<double, double> map(double px, double py) const noexcept
    {
        const auto& r = *region;
        const double lx = r.x + ((px - x) * r.width / width);
        const double ly = r.y + ((py - y) * r.height / height);
        // Regions are half-open: x + width itself lies outside. The protocol
        // carries floats, so the last point inside must be one as a float.
        const auto last = [](double start, double size) {
            return static_cast<double>(std::nextafter(static_cast<float>(start + size), static_cast<float>(start)));
        };
        return {std::clamp(lx, r.x, last(r.x, r.width)), std::clamp(ly, r.y, last(r.y, r.height))};
    }
};

EiInput::EiInput(struct ei* context) noexcept : ei_(context) {}

Result<std::unique_ptr<EiInput>> EiInput::connect_fd(int fd)
{
    if (fd < 0) {
        return fail(Errc::io, "invalid EIS socket");
    }
    // libei's handshake blocks on a blocking socket.
    const int flags = ::fcntl(fd, F_GETFL);                           // NOLINT(cppcoreguidelines-pro-type-vararg)
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {  // NOLINT(cppcoreguidelines-pro-type-vararg)
        log::warn(component, "cannot make the EIS socket non-blocking: {}", std::generic_category().message(errno));
        ::close(fd);
        return fail(Errc::io, "cannot make the EIS socket non-blocking");
    }
    struct ei* context = new_context();
    if (context == nullptr) {
        ::close(fd);
        return fail(Errc::io, "cannot create a libei context");
    }
    // libei owns the fd from here on, and closes it on failure too.
    return finish_setup(context, ei_setup_backend_fd(context, fd));
}

Result<std::unique_ptr<EiInput>> EiInput::connect_socket(const std::string& path)
{
    struct ei* context = new_context();
    if (context == nullptr) {
        return fail(Errc::io, "cannot create a libei context");
    }
    return finish_setup(context, ei_setup_backend_socket(context, path.c_str()));
}

Result<std::unique_ptr<EiInput>> EiInput::finish_setup(struct ei* context, int rc)
{
    if (rc < 0) {
        log::warn(component, "cannot connect to the EIS server: {}", std::generic_category().message(-rc));
        ei_unref(context);
        return fail(Errc::io, "cannot connect to the EIS server");
    }
    // The constructor is private, so std::make_unique cannot reach it.
    std::unique_ptr<EiInput> input(new EiInput(context));
    // The setup already read the server's first messages; handle what it queued.
    input->drain_events();
    return input;
}

EiInput::~EiInput()
{
    if (state_ != State::disconnected) {
        // Never leave a key stuck in the compositor. Releases for paused
        // devices cannot be sent; the compositor gets them when it removes the
        // devices of the closed connection.
        const std::uint64_t now = ei_now(ei_);
        for (const auto& device : devices_) {
            if (!device->resumed) {
                continue;
            }
            for (const auto& held : device->held) {
                send_held(*device, held.key, held.code, false);
                device->dirty = true;
            }
            for (const auto& touch : touches_) {
                if (touch.device == device.get()) {
                    ei_touch_up(touch.handle);
                    device->dirty = true;
                }
            }
            if (device->dirty) {
                ei_device_frame(device->handle, now);
            }
            ei_device_stop_emulating(device->handle);
        }
    }
    for (const auto& touch : touches_) {
        ei_touch_unref(touch.handle);
    }
    for (const auto& device : devices_) {
        ei_device_unref(device->handle);
    }
    if (seat_ != nullptr) {
        ei_seat_unref(seat_);
    }
    ei_unref(ei_);
}

int EiInput::fd() const noexcept
{
    return ei_get_fd(ei_);
}

void EiInput::dispatch()
{
    ei_dispatch(ei_);
    drain_events();
}

bool EiInput::can_send(Capability capability) const noexcept
{
    return std::ranges::any_of(devices_, [&](const auto& d) { return d->resumed && d->has(capability); });
}

void EiInput::set_outputs(std::vector<Output> outputs)
{
    outputs_ = std::move(outputs);
    for (const auto& output : outputs_) {
        log::debug(component, "output '{}' at {}x{}+{}+{}", output.mapping_id, output.desktop.width,
                   output.desktop.height, output.desktop.x, output.desktop.y);
    }
}

void EiInput::drain_events()
{
    while (true) {
        const EventPtr event(ei_get_event(ei_));
        if (!event) {
            return;
        }
        handle_event(event.get());
    }
}

void EiInput::handle_event(struct ei_event* event)
{
    switch (ei_event_get_type(event)) {
    case EI_EVENT_CONNECT:
        state_ = State::connected;
        log::debug(component, "connected to the EIS server");
        break;
    case EI_EVENT_DISCONNECT:
        // libei removes every device and seat before this event.
        state_ = State::disconnected;
        for (const auto& touch : touches_) {
            ei_touch_unref(touch.handle);
        }
        touches_.clear();
        for (const auto& device : devices_) {
            ei_device_unref(device->handle);
        }
        devices_.clear();
        last_pointer_ = nullptr;
        if (seat_ != nullptr) {
            ei_seat_unref(seat_);
            seat_ = nullptr;
        }
        log::info(component, "the EIS server disconnected");
        break;
    case EI_EVENT_SEAT_ADDED: {
        struct ei_seat* seat = ei_event_get_seat(event);
        if (seat_ != nullptr) {
            log::debug(component, "ignoring additional seat '{}'", safe(ei_seat_get_name(seat)));
            break;
        }
        seat_ = ei_seat_ref(seat);
        std::uint32_t offered = 0;
        for (const auto capability : all_capabilities) {
            if (ei_seat_has_capability(seat, to_ei(capability))) {
                offered |= bit(capability);
            }
        }
        log::debug(component, "binding seat '{}' (offers {})", safe(ei_seat_get_name(seat)), capability_names(offered));
        // A C variadic list terminated by NULL is libei's API.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        ei_seat_bind_capabilities(seat, EI_DEVICE_CAP_KEYBOARD, EI_DEVICE_CAP_POINTER, EI_DEVICE_CAP_POINTER_ABSOLUTE,
                                  EI_DEVICE_CAP_BUTTON, EI_DEVICE_CAP_SCROLL, EI_DEVICE_CAP_TOUCH, nullptr);
        break;
    }
    case EI_EVENT_SEAT_REMOVED:
        if (ei_event_get_seat(event) == seat_) {
            log::debug(component, "seat '{}' removed", safe(ei_seat_get_name(seat_)));
            ei_seat_unref(seat_);
            seat_ = nullptr;
        }
        break;
    case EI_EVENT_DEVICE_ADDED:
        device_added(event);
        break;
    case EI_EVENT_DEVICE_RESUMED:
        if (auto* device = find(event)) {
            device_resumed(*device);
        }
        break;
    case EI_EVENT_DEVICE_PAUSED:
        if (auto* device = find(event)) {
            drop_touches(*device);
            device_paused(*device);
        }
        break;
    case EI_EVENT_DEVICE_REMOVED:
        device_removed(event);
        break;
    case EI_EVENT_KEYBOARD_MODIFIERS:
        log::trace(component, "modifiers: depressed {:#x} latched {:#x} locked {:#x} group {}",
                   ei_event_keyboard_get_xkb_mods_depressed(event), ei_event_keyboard_get_xkb_mods_latched(event),
                   ei_event_keyboard_get_xkb_mods_locked(event), ei_event_keyboard_get_xkb_group(event));
        break;
    default:
        // Everything else only reaches receiver contexts.
        break;
    }
}

void EiInput::device_added(struct ei_event* event)
{
    struct ei_device* handle = ei_event_get_device(event);
    std::uint32_t caps = 0;
    for (const auto capability : all_capabilities) {
        if (ei_device_has_capability(handle, to_ei(capability))) {
            caps |= bit(capability);
        }
    }
    if (caps == 0) {
        log::debug(component, "closing device '{}' without usable capabilities", safe(ei_device_get_name(handle)));
        ei_device_close(handle);
        return;
    }

    auto device = std::make_unique<Device>();
    device->name = std::string(safe(ei_device_get_name(handle)));
    device->caps = caps;
    for (std::size_t i = 0;; ++i) {
        struct ei_region* region = ei_device_get_region(handle, i);
        if (region == nullptr) {
            break;
        }
        const double scale = ei_region_get_physical_scale(region);
        device->regions.push_back(Region{
            .x = static_cast<double>(ei_region_get_x(region)),
            .y = static_cast<double>(ei_region_get_y(region)),
            .width = static_cast<double>(ei_region_get_width(region)),
            .height = static_cast<double>(ei_region_get_height(region)),
            .scale = scale > 0 && std::isfinite(scale) ? scale : 1.0,
            .mapping_id = std::string(safe(ei_region_get_mapping_id(region))),
        });
    }
    log::debug(component, "device '{}' added: {}, {}", device->name, capability_names(caps),
               ei_device_get_type(handle) == EI_DEVICE_TYPE_PHYSICAL ? "physical" : "virtual");
    for (const auto& r : device->regions) {
        log::debug(component, "  region {}x{}+{}+{} scale {} mapping '{}'", r.width, r.height, r.x, r.y, r.scale,
                   r.mapping_id);
    }
    if (device->has(Capability::pointer_absolute) && device->regions.empty()) {
        log::debug(component, "device '{}' has no regions: no absolute motion through it", device->name);
    }
    device->handle = ei_device_ref(handle);
    devices_.push_back(std::move(device));
}

void EiInput::device_resumed(Device& device)
{
    device.resumed = true;
    ei_device_start_emulating(device.handle, ++sequence_);
    // Releases the client sent while the device was paused.
    const auto released = std::ranges::stable_partition(device.held, [](const Held& h) { return !h.released; });
    for (const auto& held : released) {
        send_held(device, held.key, held.code, false);
    }
    const bool sent = !released.empty();
    device.held.erase(released.begin(), released.end());
    if (sent) {
        ei_device_frame(device.handle, ei_now(ei_));
    }
    log::debug(component, "device '{}' resumed (sequence {}){}", device.name, sequence_,
               sent ? ", sent releases held back while paused" : "");
}

void EiInput::device_paused(Device& device)
{
    device.resumed = false;
    device.dirty = false;
    // libei already filters events while paused; this keeps the sequence tidy.
    ei_device_stop_emulating(device.handle);
    log::debug(component, "device '{}' paused", device.name);
}

void EiInput::device_removed(struct ei_event* event)
{
    struct ei_device* handle = ei_event_get_device(event);
    const auto it = std::ranges::find_if(devices_, [&](const auto& d) { return d->handle == handle; });
    if (it == devices_.end()) {
        return;
    }
    Device& device = **it;
    log::debug(component, "device '{}' removed{}", device.name,
               device.held.empty() ? "" : " with keys or buttons held (the compositor releases them)");
    if (last_pointer_ == &device) {
        last_pointer_ = nullptr;
    }
    drop_touches(device);
    ei_device_unref(device.handle);
    devices_.erase(it);
}

EiInput::Device* EiInput::find(struct ei_event* event) const noexcept
{
    auto* const handle = ei_event_get_device(event);
    for (const auto& device : devices_) {
        if (device->handle == handle) {
            return device.get();
        }
    }
    return nullptr;
}

EiInput::Device* EiInput::pick(Capability capability) const noexcept
{
    if ((capability == Capability::button || capability == Capability::scroll) && last_pointer_ != nullptr &&
        last_pointer_->resumed && last_pointer_->has(capability)) {
        return last_pointer_;
    }
    for (const auto& device : devices_) {
        if (device->resumed && device->has(capability)) {
            return device.get();
        }
    }
    return nullptr;
}

std::vector<EiInput::Target> EiInput::absolute_targets(Capability capability, const Device* only) const
{
    std::vector<Target> matched;
    std::vector<Target> fallback;
    for (const auto& device : devices_) {
        if (!device->resumed || !device->has(capability) || (only != nullptr && device.get() != only)) {
            continue;
        }
        for (const auto& region : device->regions) {
            if (region.width <= 0 || region.height <= 0) {
                continue;
            }
            const auto output = std::ranges::find_if(outputs_, [&](const Output& o) {
                return !region.mapping_id.empty() && o.mapping_id == region.mapping_id && o.desktop.width > 0 &&
                       o.desktop.height > 0;
            });
            if (output != outputs_.end()) {
                const Rect& desktop = output->desktop;
                matched.push_back(Target{device.get(), &region, static_cast<double>(desktop.x),
                                         static_cast<double>(desktop.y), static_cast<double>(desktop.width),
                                         static_cast<double>(desktop.height)});
            }
            fallback.push_back(Target{device.get(), &region, region.x, region.y, region.width * region.scale,
                                      region.height * region.scale});
        }
    }
    if (matched.empty() && fallback.size() == 1 && outputs_.size() == 1 && outputs_.front().desktop.width > 0 &&
        outputs_.front().desktop.height > 0) {
        // One output and one region belong together, with or without
        // mapping ids (portals before ScreenCast version 5 have none).
        const Rect& desktop = outputs_.front().desktop;
        Target single = fallback.front();
        single.x = static_cast<double>(desktop.x);
        single.y = static_cast<double>(desktop.y);
        single.width = static_cast<double>(desktop.width);
        single.height = static_cast<double>(desktop.height);
        return {single};
    }
    return matched.empty() ? fallback : matched;
}

void EiInput::send_held(Device& device, bool key, std::uint32_t code, bool pressed)
{
    if (key) {
        ei_device_keyboard_key(device.handle, code, pressed);
    } else {
        ei_device_button_button(device.handle, code, pressed);
    }
}

void EiInput::press_or_release(bool key, std::uint32_t code, bool pressed)
{
    if (closed()) {
        return;
    }
    for (const auto& device : devices_) {
        const auto it =
            std::ranges::find_if(device->held, [&](const Held& h) { return h.key == key && h.code == code; });
        if (it == device->held.end()) {
            continue;
        }
        if (pressed) {
            // Held already: a client's key repeat (the compositor repeats on
            // its own), or pressed again before a release held back by a pause
            // went out.
            it->released = false;
        } else if (device->resumed) {
            send_held(*device, key, code, false);
            device->held.erase(it);
            device->dirty = true;
        } else {
            it->released = true;
        }
        return;
    }
    if (!pressed) {
        // Its press was never sent (no device, or the device was paused).
        return;
    }
    Device* device = pick(key ? Capability::keyboard : Capability::button);
    if (device == nullptr) {
        log::trace(component, "no device for {} {}: dropped", key ? "key" : "button", code);
        return;
    }
    send_held(*device, key, code, true);
    device->held.push_back(Held{.key = key, .code = code, .released = false});
    device->dirty = true;
}

void EiInput::key(std::uint32_t evdev_code, bool pressed)
{
    press_or_release(true, evdev_code, pressed);
}

void EiInput::button(std::uint32_t evdev_button, bool pressed)
{
    press_or_release(false, evdev_button, pressed);
}

void EiInput::pointer_motion_absolute(double x, double y)
{
    if (closed() || !std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    const auto targets = absolute_targets();
    const Target* best = Target::nearest(targets, x, y);
    if (best == nullptr) {
        return;
    }
    const auto [lx, ly] = best->map(x, y);
    ei_device_pointer_motion_absolute(best->device->handle, lx, ly);
    best->device->dirty = true;
    last_pointer_ = best->device;
}

void EiInput::pointer_motion_relative(double dx, double dy)
{
    if (closed() || !std::isfinite(dx) || !std::isfinite(dy)) {
        return;
    }
    Device* device = pick(Capability::pointer);
    if (device == nullptr) {
        return;
    }
    ei_device_pointer_motion(device->handle, dx, dy);
    device->dirty = true;
    last_pointer_ = device;
}

void EiInput::scroll_discrete(std::int32_t x_v120, std::int32_t y_v120)
{
    if (closed() || (x_v120 == 0 && y_v120 == 0)) {
        return;
    }
    // Wheel steps are not a scroll sequence, so no scroll_stop follows them
    // (libinput's wheel source has none either).
    Device* device = pick(Capability::scroll);
    if (device == nullptr) {
        return;
    }
    ei_device_scroll_delta(
        device->handle,
        static_cast<double>(x) / 4.0,
        static_cast<double>(y / 4.0)
    );

    device->dirty = true;
}

void EiInput::touch_down(std::uint32_t slot, double x, double y)
{
    if (closed() || !std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    if (std::ranges::any_of(touches_, [&](const Touch& t) { return t.slot == slot; })) {
        touch_motion(slot, x, y);
        return;
    }
    Device* device = nullptr;
    double lx = x;
    double ly = y;
    const auto targets = absolute_targets(Capability::touch);
    if (const Target* best = Target::nearest(targets, x, y)) {
        device = best->device;
        std::tie(lx, ly) = best->map(x, y);
    } else {
        // A touch device without regions takes desktop pixels as they are.
        device = pick(Capability::touch);
    }
    if (device == nullptr) {
        log::trace(component, "no touch device for slot {}: dropped", slot);
        return;
    }
    struct ei_touch* handle = ei_device_touch_new(device->handle);
    if (handle == nullptr) {
        return;
    }
    ei_touch_down(handle, lx, ly);
    touches_.push_back(Touch{.slot = slot, .device = device, .handle = handle});
    device->dirty = true;
}

void EiInput::touch_motion(std::uint32_t slot, double x, double y)
{
    if (closed() || !std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    const auto it = std::ranges::find(touches_, slot, &Touch::slot);
    if (it == touches_.end() || !it->device->resumed) {
        return;
    }
    double lx = x;
    double ly = y;
    const auto targets = absolute_targets(Capability::touch, it->device);
    if (const Target* best = Target::nearest(targets, x, y)) {
        std::tie(lx, ly) = best->map(x, y);
    }
    ei_touch_motion(it->handle, lx, ly);
    it->device->dirty = true;
}

void EiInput::touch_up(std::uint32_t slot)
{
    end_touch(slot, false);
}

void EiInput::touch_cancel(std::uint32_t slot)
{
    end_touch(slot, true);
}

void EiInput::end_touch(std::uint32_t slot, bool cancel)
{
    const auto it = std::ranges::find(touches_, slot, &Touch::slot);
    if (it == touches_.end()) {
        return;
    }
    if (!closed() && it->device->resumed) {
#ifdef FARLAND_HAVE_EI_TOUCH_CANCEL
        if (cancel) {
            ei_touch_cancel(it->handle);
        } else {
            ei_touch_up(it->handle);
        }
#else
        static_cast<void>(cancel);
        ei_touch_up(it->handle);
#endif
        it->device->dirty = true;
    }
    ei_touch_unref(it->handle);
    touches_.erase(it);
}

void EiInput::drop_touches(const Device& device)
{
    const auto dropped = std::ranges::remove_if(touches_, [&](const Touch& t) {
        if (t.device != &device) {
            return false;
        }
        ei_touch_unref(t.handle);
        return true;
    });
    if (!dropped.empty()) {
        log::debug(component, "device '{}' went away with {} touches down (the compositor ends them)", device.name,
                   dropped.size());
    }
    touches_.erase(dropped.begin(), dropped.end());
}

void EiInput::text(char32_t codepoint)
{
    log::trace(component, "text input U+{:04X} ignored: EIS only sends keycodes",
               static_cast<std::uint32_t>(codepoint));
}

void EiInput::flush()
{
    if (closed()) {
        return;
    }
    const std::uint64_t now = ei_now(ei_);
    for (const auto& device : devices_) {
        if (device->dirty && device->resumed) {
            ei_device_frame(device->handle, now);
        }
        device->dirty = false;
    }
    // A failed send queues the disconnection without waking fd().
    drain_events();
}

}  // namespace farland::platform::portal
