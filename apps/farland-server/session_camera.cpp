// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "session_camera.hpp"

#include <farland/base/log.hpp>

#ifdef FARLAND_HAVE_PIPEWIRE_AUDIO
#include <farland/platform/portal/pipewire_camera.hpp>
#endif

#include <format>
#include <utility>

namespace farland::app {

namespace {

constexpr std::string_view log_component = "app.camera";
constexpr auto statistics_interval = std::chrono::seconds(10);

namespace cam = channels::rdpecam;

/// The local layout for a media type the client agreed to. Only the
/// uncompressed formats reach here: CameraServer asks for nothing else.
[[nodiscard]] std::optional<platform::VideoMode> local_mode(const cam::MediaType& type)
{
    platform::VideoFormat format{};
    switch (type.format) {
    case cam::MediaFormat::yuy2:
        format = platform::VideoFormat::yuy2;
        break;
    case cam::MediaFormat::nv12:
        format = platform::VideoFormat::nv12;
        break;
    case cam::MediaFormat::i420:
        format = platform::VideoFormat::i420;
        break;
    case cam::MediaFormat::rgb24:
        format = platform::VideoFormat::rgb24;
        break;
    case cam::MediaFormat::rgb32:
        format = platform::VideoFormat::rgb32;
        break;
    case cam::MediaFormat::h264:
    case cam::MediaFormat::mjpg:
    case cam::MediaFormat::invalid:
        return std::nullopt;
    }
    return platform::VideoMode{
        .format = format,
        .width = type.width,
        .height = type.height,
        .fps = std::max(type.fps(), 1U),
        .bottom_up = (type.flags & cam::media_flag::bottom_up_image) != 0,
    };
}

}  // namespace

SessionCamera::SessionCamera(std::string peer, server::CameraOptions options)
    : peer_(std::move(peer)), options_(options), statistics_since_(std::chrono::steady_clock::now())
{
}

SessionCamera::~SessionCamera() = default;

void SessionCamera::dynamic_channels_ready(server::DynamicChannels& channels)
{
    if (!camera_) {
        camera_.emplace(channels, options_);
    }
}

bool SessionCamera::handle(const channels::DvcEvent& event)
{
    return camera_ && camera_->handle(event);
}

void SessionCamera::service()
{
    if (!camera_) {
        return;
    }
    while (auto event = camera_->poll_event()) {
        std::visit(
            [this](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, server::camera_event::Opened>) {
                    open_local_camera(e);
                } else if constexpr (std::is_same_v<T, server::camera_event::Frame>) {
                    if (local_ != nullptr) {
                        local_->write(e.data);
                        ++frames_;
                    } else {
                        ++dropped_;
                    }
                } else if constexpr (std::is_same_v<T, server::camera_event::Closed>) {
                    close_local_camera(e.reason);
                }
            },
            *event);
        // The frame just handled is the one the request was for; asking again
        // here is what keeps the stream going, at the pace we can take it.
        camera_->request_frame();
    }
    if (local_ != nullptr && local_->closed()) {
        const std::string why = local_->error();
        close_local_camera(why.empty() ? "the local camera stopped" : why);
        camera_->stop();
    }
    log_statistics(std::chrono::steady_clock::now());
}

void SessionCamera::open_local_camera(const server::camera_event::Opened& opened)
{
    close_local_camera("a new camera");
    const auto mode = local_mode(opened.media_type);
    if (!mode) {
        log::warn(log_component, "{}: the camera's format cannot be published locally", peer_);
        return;
    }
#ifdef FARLAND_HAVE_PIPEWIRE_AUDIO
    auto camera = platform::portal::create_virtual_camera(
        *mode, {.node_name = "farland-camera", .description = std::format("{} (farland)", opened.device_name)});
    if (!camera) {
        log::warn(log_component, "{}: no local camera: {}", peer_, camera.error().message());
        return;
    }
    local_ = std::move(*camera);
    mode_ = *mode;
    device_name_ = opened.device_name;
    frames_ = 0;
    dropped_ = 0;
    statistics_since_ = std::chrono::steady_clock::now();
    log::info(log_component, "{}: camera '{}' is local as farland-camera ({} {}x{} at {} fps)", peer_,
              opened.device_name, cam::format_name(opened.media_type.format), mode_.width, mode_.height, mode_.fps);
#else
    log::warn(log_component, "{}: this build has no PipeWire, so the client's camera stays unused", peer_);
#endif
}

void SessionCamera::close_local_camera(const std::string& reason)
{
    if (local_ == nullptr) {
        return;
    }
    log::info(log_component, "{}: the local camera is gone ({}), {} frames carried", peer_, reason, frames_);
    local_.reset();
    device_name_.clear();
}

void SessionCamera::log_statistics(std::chrono::steady_clock::time_point now)
{
    if (local_ == nullptr || now - statistics_since_ < statistics_interval) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - statistics_since_).count();
    log::info(log_component, "{}: camera {:.1f} fps ({} frames in {:.0f} s{})", peer_,
              static_cast<double>(frames_) / seconds, frames_, seconds,
              dropped_ > 0 ? std::format(", {} dropped", dropped_) : std::string{});
    frames_ = 0;
    dropped_ = 0;
    statistics_since_ = now;
}

}  // namespace farland::app
