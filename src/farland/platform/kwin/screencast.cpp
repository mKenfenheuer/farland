// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/platform/kwin/screencast.hpp>

#include <wayland-client.h>
#include <zkde-screencast-unstable-v1-client-protocol.h>

#include <algorithm>

namespace farland::platform::kwin {

namespace {

constexpr std::string_view log_component = "platform.kwin";
/// The highest version the vendored protocol describes: stream_output is in
/// version 1, stream_virtual_output in 2, and the one that also takes a
/// description in 4. KWin 6 offers 6; binding the version we know keeps the
/// requests we send within what the generated code describes.
constexpr std::uint32_t screencast_version = 5;

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
    const std::uint32_t version = std::min(global->version, screencast_version);
    log::debug(log_component, "zkde_screencast_unstable_v1 version {} (KWin offers {}){}", version, global->version,
               version >= virtual_output_version ? ", with virtual outputs" : "");
    return std::unique_ptr<Screencast>(new Screencast(connection, manager, version));
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

std::unique_ptr<ScreencastStream> Screencast::stream_virtual_output(const std::string& name,
                                                                    const std::string& description, std::int32_t width,
                                                                    std::int32_t height, double scale, Cursor cursor)
{
    if (!has_virtual_outputs()) {
        return nullptr;
    }
    const wl_fixed_t fixed_scale = wl_fixed_from_double(scale);
    auto* proxy = version_ >= description_version
                      ? zkde_screencast_unstable_v1_stream_virtual_output_with_description(
                            manager_, name.c_str(), description.c_str(), width, height, fixed_scale,
                            static_cast<std::uint32_t>(cursor))
                      : zkde_screencast_unstable_v1_stream_virtual_output(manager_, name.c_str(), width, height,
                                                                          fixed_scale,
                                                                          static_cast<std::uint32_t>(cursor));
    std::unique_ptr<ScreencastStream> stream(new ScreencastStream(proxy));
    connection_.flush();
    return stream;
}

}  // namespace farland::platform::kwin
