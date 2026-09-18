// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/wlroots/wayland/connection.hpp>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <format>
#include <iterator>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>

namespace farland::platform::wayland {

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::string_view log_component = "platform.wayland";

/// A connected socket to the compositor at `path`; -1 on failure.
int connect_socket(const std::string& path)
{
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    std::ranges::copy(path, std::begin(address.sun_path));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API takes sockaddr_un as sockaddr.
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        const int saved = errno;
        ::close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

int remaining_ms(Clock::time_point deadline)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return static_cast<int>(std::clamp<std::int64_t>(left, 0, 1000));
}

}  // namespace

Result<std::unique_ptr<Connection>> Connection::connect(const std::string& path, std::chrono::milliseconds timeout)
{
    wl_display* display = nullptr;
    if (path.empty()) {
        display = wl_display_connect(nullptr);
    } else if (const int fd = connect_socket(path); fd >= 0) {
        display = wl_display_connect_to_fd(fd);
        if (display == nullptr) {
            ::close(fd);
        }
    }
    if (display == nullptr) {
        log::error(log_component, "cannot connect to the Wayland compositor at {}: {}",
                   path.empty() ? std::string("$WAYLAND_DISPLAY") : path, std::strerror(errno));
        return fail(Errc::io, "cannot connect to the Wayland compositor");
    }
    std::unique_ptr<Connection> connection(new Connection(display));
    static constexpr wl_registry_listener listener{
        .global = &Connection::handle_global,
        .global_remove = &Connection::handle_global_remove,
    };
    connection->registry_ = wl_display_get_registry(display);
    wl_registry_add_listener(connection->registry_, &listener, connection.get());
    if (!connection->roundtrip(timeout)) {
        log::error(log_component, "no Wayland globals: {}",
                   connection->error_.empty() ? std::string("timed out") : connection->error_);
        return fail(Errc::io, "the Wayland compositor did not answer");
    }
    return connection;
}

Connection::Connection(wl_display* display) : display_(display) {}

Connection::~Connection()
{
    if (registry_ != nullptr) {
        wl_registry_destroy(registry_);
    }
    wl_display_flush(display_);
    wl_display_disconnect(display_);
}

int Connection::fd() const noexcept
{
    return wl_display_get_fd(display_);
}

const Global* Connection::find(std::string_view interface) const noexcept
{
    const auto it = std::ranges::find(globals_, interface, &Global::interface);
    return it == globals_.end() ? nullptr : &*it;
}

void* Connection::bind(const Global& global, const wl_interface* interface, std::uint32_t version) const
{
    return wl_registry_bind(registry_, global.name, interface, std::min(version, global.version));
}

void Connection::handle_global(void* data, wl_registry* /*registry*/, std::uint32_t name, const char* interface,
                               std::uint32_t version)
{
    static_cast<Connection*>(data)->globals_.push_back(Global{name, interface, version});
}

void Connection::handle_global_remove(void* data, wl_registry* /*registry*/, std::uint32_t name)
{
    auto* self = static_cast<Connection*>(data);
    std::erase_if(self->globals_, [name](const Global& g) { return g.name == name; });
    for (const auto& callback : self->removed_callbacks_) {
        callback(name);
    }
}

void Connection::on_global_removed(std::function<void(std::uint32_t name)> callback)
{
    removed_callbacks_.push_back(std::move(callback));
}

void Connection::close_with(std::string_view what)
{
    if (closed_) {
        return;
    }
    closed_ = true;
    const int code = wl_display_get_error(display_);
    if (code == EPROTO) {
        const wl_interface* interface = nullptr;
        std::uint32_t id = 0;
        const std::uint32_t protocol_code = wl_display_get_protocol_error(display_, &interface, &id);
        error_ = std::format("protocol error {} on {}@{}", protocol_code,
                             interface != nullptr ? interface->name : "unknown", id);
    } else {
        error_ = std::format("{}: {}", what, std::strerror(code != 0 ? code : errno));
    }
    log::warn(log_component, "the Wayland connection closed: {}", error_);
}

bool Connection::dispatch_pending()
{
    if (!closed_ && wl_display_dispatch_pending(display_) < 0) {
        close_with("dispatch");
    }
    return !closed_;
}

void Connection::flush()
{
    if (!closed_ && wl_display_flush(display_) < 0 && errno != EAGAIN) {
        close_with("flush");
    }
}

void Connection::dispatch()
{
    if (closed_) {
        return;
    }
    while (wl_display_prepare_read(display_) != 0) {
        if (!dispatch_pending()) {
            return;
        }
    }
    flush();
    pollfd pfd{fd(), POLLIN, 0};
    if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        if (wl_display_read_events(display_) < 0) {
            close_with("read");
            return;
        }
    } else {
        wl_display_cancel_read(display_);
    }
    if (dispatch_pending()) {
        flush();
    }
}

bool Connection::wait_until(const std::function<bool()>& done, std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;
    while (!closed_) {
        while (wl_display_prepare_read(display_) != 0) {
            if (!dispatch_pending()) {
                return false;
            }
        }
        if (done()) {
            wl_display_cancel_read(display_);
            return true;
        }
        if (Clock::now() >= deadline) {
            wl_display_cancel_read(display_);
            return false;
        }
        flush();
        pollfd pfd{fd(), POLLIN, 0};
        if (::poll(&pfd, 1, remaining_ms(deadline)) > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            if (wl_display_read_events(display_) < 0) {
                close_with("read");
                return false;
            }
        } else {
            wl_display_cancel_read(display_);
        }
        if (!dispatch_pending()) {
            return false;
        }
    }
    return false;
}

bool Connection::roundtrip(std::chrono::milliseconds timeout)
{
    bool done = false;
    static constexpr wl_callback_listener listener{
        .done = [](void* data, wl_callback* /*callback*/,
                   std::uint32_t /*serial*/) { *static_cast<bool*>(data) = true; },
    };
    wl_callback* callback = wl_display_sync(display_);
    wl_callback_add_listener(callback, &listener, &done);
    const bool ok = wait_until([&done] { return done; }, timeout);
    wl_callback_destroy(callback);
    return ok;
}

}  // namespace farland::platform::wayland
