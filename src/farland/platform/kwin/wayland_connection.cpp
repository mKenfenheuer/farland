// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/kwin/wayland_connection.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <wayland-client.h>

namespace farland::platform::kwin {

namespace {

constexpr std::string_view log_component = "platform.wayland";
constexpr std::uint32_t output_version = 4;
constexpr std::uint32_t seat_version = 2;
using Clock = std::chrono::steady_clock;

WaylandConnection& self(void* data) noexcept
{
    return *static_cast<WaylandConnection*>(data);
}

WaylandOutput& output_of(void* data) noexcept
{
    return *static_cast<WaylandOutput*>(data);
}

void release(wl_output* output) noexcept
{
    if (wl_output_get_version(output) >= WL_OUTPUT_RELEASE_SINCE_VERSION) {
        wl_output_release(output);
    } else {
        wl_output_destroy(output);
    }
}

void release(wl_seat* seat) noexcept
{
    if (wl_seat_get_version(seat) >= WL_SEAT_RELEASE_SINCE_VERSION) {
        wl_seat_release(seat);
    } else {
        wl_seat_destroy(seat);
    }
}

}  // namespace

struct WaylandConnection::Listeners {
    static constexpr wl_registry_listener registry{
        .global = [](void* data, wl_registry* registry, std::uint32_t name, const char* interface,
                     std::uint32_t version) { self(data).global_added(registry, name, interface, version); },
        .global_remove = [](void* data, wl_registry* /*registry*/,
                            std::uint32_t name) { self(data).global_removed(name); },
    };

    static constexpr wl_output_listener output{
        .geometry =
            [](void* data, wl_output* /*output*/, std::int32_t x, std::int32_t y, std::int32_t /*physical_width*/,
               std::int32_t /*physical_height*/, std::int32_t /*subpixel*/, const char* /*make*/, const char* /*model*/,
               std::int32_t /*transform*/) {
                output_of(data).x = x;
                output_of(data).y = y;
            },
        .mode =
            [](void* data, wl_output* /*output*/, std::uint32_t flags, std::int32_t width, std::int32_t height,
               std::int32_t /*refresh*/) {
                if ((flags & WL_OUTPUT_MODE_CURRENT) != 0) {
                    output_of(data).width = width;
                    output_of(data).height = height;
                }
            },
        .done =
            [](void* data, wl_output* /*output*/) {
                output_of(data).ready = true;
                ++output_of(data).descriptions;
            },
        .scale = [](void* data, wl_output* /*output*/, std::int32_t factor) { output_of(data).scale = factor; },
        .name = [](void* data, wl_output* /*output*/, const char* name) { output_of(data).name = name; },
        .description = [](void* data, wl_output* /*output*/,
                          const char* description) { output_of(data).description = description; },
    };

    static constexpr wl_seat_listener seat{
        .capabilities = ignore_event,
        .name = ignore_event,
    };

    static void sync_done(void* data, wl_callback* callback, std::uint32_t /*serial*/)
    {
        *static_cast<bool*>(data) = true;
        wl_callback_destroy(callback);
    }
    static constexpr wl_callback_listener sync{.done = &sync_done};
};

Result<std::unique_ptr<WaylandConnection>> WaylandConnection::connect(const std::string& display,
                                                                      std::chrono::milliseconds timeout)
{
    wl_display* raw = wl_display_connect(display.empty() ? nullptr : display.c_str());
    if (raw == nullptr) {
        log::debug(log_component, "cannot connect to Wayland display '{}': {}", display, std::strerror(errno));
        return fail(Errc::io, "cannot connect to the Wayland compositor");
    }
    return finish(raw, timeout);
}

Result<std::unique_ptr<WaylandConnection>> WaylandConnection::connect_fd(int fd, std::chrono::milliseconds timeout)
{
    wl_display* raw = wl_display_connect_to_fd(fd);
    if (raw == nullptr) {
        return fail(Errc::io, "cannot connect to the Wayland compositor");
    }
    return finish(raw, timeout);
}

Result<std::unique_ptr<WaylandConnection>> WaylandConnection::finish(wl_display* display,
                                                                     std::chrono::milliseconds timeout)
{
    std::unique_ptr<WaylandConnection> connection(new WaylandConnection(display));
    connection->registry_ = wl_display_get_registry(display);
    wl_registry_add_listener(connection->registry_, &Listeners::registry, connection.get());
    // The globals, then the outputs' and the seat's first events.
    if (!connection->roundtrip(timeout) || !connection->roundtrip(timeout)) {
        return fail(Errc::io, "the Wayland compositor does not answer");
    }
    return connection;
}

WaylandConnection::~WaylandConnection()
{
    for (auto& output : outputs_) {
        release(output->proxy);
    }
    if (seat_ != nullptr) {
        release(seat_);
    }
    if (registry_ != nullptr) {
        wl_registry_destroy(registry_);
    }
    wl_display_flush(display_);
    wl_display_disconnect(display_);
}

int WaylandConnection::fd() const noexcept
{
    return wl_display_get_fd(display_);
}

const WaylandGlobal* WaylandConnection::find_global(std::string_view interface) const noexcept
{
    const auto it = std::ranges::find(globals_, interface, &WaylandGlobal::interface);
    return it == globals_.end() ? nullptr : &*it;
}

void* WaylandConnection::bind(const WaylandGlobal& global, const wl_interface* interface,
                              std::uint32_t max_version) const
{
    return wl_registry_bind(registry_, global.name, interface, std::min(global.version, max_version));
}

void WaylandConnection::set_global_listeners(std::function<void(const WaylandGlobal&)> added,
                                             std::function<void(const WaylandGlobal&)> removed)
{
    on_added_ = std::move(added);
    on_removed_ = std::move(removed);
}

const WaylandOutput* WaylandConnection::find_output(std::string_view name) const noexcept
{
    const auto it = std::ranges::find_if(outputs_, [name](const auto& output) { return output->name == name; });
    return it == outputs_.end() ? nullptr : it->get();
}

void WaylandConnection::global_added(wl_registry* registry, std::uint32_t name, const char* interface,
                                     std::uint32_t version)
{
    globals_.push_back(WaylandGlobal{name, interface, version});
    const std::string_view kind(interface);
    if (kind == wl_output_interface.name) {
        auto output = std::make_unique<WaylandOutput>();
        output->global = name;
        output->proxy = static_cast<wl_output*>(
            wl_registry_bind(registry, name, &wl_output_interface, std::min(version, output_version)));
        wl_output_add_listener(output->proxy, &Listeners::output, output.get());
        outputs_.push_back(std::move(output));
        ++outputs_generation_;
    } else if (kind == wl_seat_interface.name && seat_ == nullptr) {
        seat_ = static_cast<wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, seat_version)));
        wl_seat_add_listener(seat_, &Listeners::seat, this);
        seat_global_ = name;
    }
    if (on_added_) {
        on_added_(globals_.back());
    }
}

void WaylandConnection::global_removed(std::uint32_t name)
{
    const auto it = std::ranges::find(globals_, name, &WaylandGlobal::name);
    if (it == globals_.end()) {
        return;
    }
    if (on_removed_) {
        on_removed_(*it);
    }
    globals_.erase(it);
    const auto output = std::ranges::find(outputs_, name, [](const auto& o) { return o->global; });
    if (output != outputs_.end()) {
        release((*output)->proxy);
        outputs_.erase(output);
        ++outputs_generation_;
    }
    if (name == seat_global_ && seat_ != nullptr) {
        release(seat_);
        seat_ = nullptr;
    }
}

void WaylandConnection::check_error()
{
    if (!broken_ && wl_display_get_error(display_) != 0) {
        broken_ = true;
        log::warn(log_component, "the Wayland connection broke: {}", std::strerror(wl_display_get_error(display_)));
    }
}

void WaylandConnection::flush()
{
    if (!broken_ && wl_display_flush(display_) < 0 && errno != EAGAIN) {
        check_error();
    }
}

void WaylandConnection::dispatch()
{
    if (broken_) {
        return;
    }
    const auto descriptions = [this] {
        std::uint64_t sum = 0;
        for (const auto& output : outputs_) {
            sum += output->descriptions;
        }
        return sum;
    };
    const auto described_before = descriptions();
    while (wl_display_prepare_read(display_) != 0) {
        if (wl_display_dispatch_pending(display_) < 0) {
            check_error();
            return;
        }
    }
    flush();
    pollfd pfd{fd(), POLLIN, 0};
    if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        if (wl_display_read_events(display_) < 0) {
            check_error();
            return;
        }
    } else {
        wl_display_cancel_read(display_);
    }
    if (wl_display_dispatch_pending(display_) < 0) {
        check_error();
        return;
    }
    flush();
    if (descriptions() != described_before) {
        ++outputs_generation_;
    }
}

bool WaylandConnection::roundtrip(std::chrono::milliseconds timeout)
{
    if (broken_) {
        return false;
    }
    bool done = false;
    wl_callback* callback = wl_display_sync(display_);
    wl_callback_add_listener(callback, &Listeners::sync, &done);
    const auto deadline = Clock::now() + timeout;
    while (!done) {
        dispatch();
        if (broken_) {
            return false;
        }
        if (done) {
            break;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) {
            wl_callback_destroy(callback);
            return false;
        }
        pollfd pfd{fd(), POLLIN, 0};
        ::poll(&pfd, 1, static_cast<int>(std::min<long long>(left, 100)));
    }
    return true;
}

}  // namespace farland::platform::kwin
