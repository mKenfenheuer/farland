// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/kwin/screencast.hpp>

#include <wayland-client.h>
#include <zkde-screencast-unstable-v1-client-protocol.h>

namespace farland::platform::kwin {

namespace {

constexpr std::string_view log_component = "platform.kwin";
/// stream_output is in version 1; nothing later is used.
constexpr std::uint32_t screencast_version = 1;

}  // namespace

struct ScreencastStream::Listener {
    static ScreencastStream& self(void* data) noexcept { return *static_cast<ScreencastStream*>(data); }

    static constexpr zkde_screencast_stream_unstable_v1_listener listener{
        .closed = [](void* data, zkde_screencast_stream_unstable_v1* /*stream*/) { self(data).state_ = State::closed; },
        .created =
            [](void* data, zkde_screencast_stream_unstable_v1* /*stream*/, std::uint32_t node) {
                self(data).node_ = node;
                self(data).state_ = State::created;
            },
        .failed =
            [](void* data, zkde_screencast_stream_unstable_v1* /*stream*/, const char* error) {
                self(data).error_ = error != nullptr ? error : "";
                self(data).state_ = State::failed;
            },
    };
};

ScreencastStream::ScreencastStream(zkde_screencast_stream_unstable_v1* proxy) : proxy_(proxy)
{
    zkde_screencast_stream_unstable_v1_add_listener(proxy_, &Listener::listener, this);
}

ScreencastStream::~ScreencastStream()
{
    zkde_screencast_stream_unstable_v1_close(proxy_);
}

Result<std::unique_ptr<Screencast>> Screencast::create(WaylandConnection& connection)
{
    const auto* global = connection.find_global(zkde_screencast_unstable_v1_interface.name);
    if (global == nullptr) {
        return fail(Errc::unsupported, "KWin does not offer zkde_screencast_unstable_v1 to this program");
    }
    auto* manager = static_cast<zkde_screencast_unstable_v1*>(
        connection.bind(*global, &zkde_screencast_unstable_v1_interface, screencast_version));
    log::debug(log_component, "zkde_screencast_unstable_v1 version {}", global->version);
    return std::unique_ptr<Screencast>(new Screencast(connection, manager));
}

Screencast::~Screencast()
{
    zkde_screencast_unstable_v1_destroy(manager_);
}

std::unique_ptr<ScreencastStream> Screencast::stream_output(wl_output* output, Cursor cursor)
{
    auto* proxy = zkde_screencast_unstable_v1_stream_output(manager_, output, static_cast<std::uint32_t>(cursor));
    std::unique_ptr<ScreencastStream> stream(new ScreencastStream(proxy));
    connection_.flush();
    return stream;
}

}  // namespace farland::platform::kwin
