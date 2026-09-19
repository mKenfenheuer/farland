// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/channels/dvc_server.hpp>
#include <farland/platform/video.hpp>
#include <farland/server/camera.hpp>
#include <farland/server/dynamic_channels.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace farland::app {

/// The client's camera in one session ([MS-RDPECAM]), on the session thread.
///
/// The camera channels open once the dynamic channels are ready. When the
/// client offers a camera and a media type farland can carry, a Video/Source
/// node appears in the user's PipeWire for as long as the camera streams, and
/// every frame the client sends is copied into it. Asking for the next frame
/// is what paces the stream, so nothing queues up.
class SessionCamera {
public:
    explicit SessionCamera(std::string peer, server::CameraOptions options = {});
    SessionCamera(const SessionCamera&) = delete;
    SessionCamera& operator=(const SessionCamera&) = delete;
    SessionCamera(SessionCamera&&) = delete;
    SessionCamera& operator=(SessionCamera&&) = delete;
    ~SessionCamera();

    /// The dynamic channels are ready: opens the enumeration channel.
    /// `channels` must outlive this object.
    void dynamic_channels_ready(server::DynamicChannels& channels);
    /// Handles a dynamic channel event for a camera channel. False otherwise.
    bool handle(const channels::DvcEvent& event);
    /// Moves what the client sent into the local camera; called each turn of
    /// the session loop.
    void service();

private:
    void open_local_camera(const server::camera_event::Opened& opened);
    void close_local_camera(const std::string& reason);
    void log_statistics(std::chrono::steady_clock::time_point now);

    std::string peer_;
    server::CameraOptions options_;
    std::optional<server::CameraServer> camera_;
    std::unique_ptr<platform::VideoSink> local_;
    platform::VideoMode mode_;
    std::string device_name_;
    std::uint64_t frames_ = 0;
    std::uint64_t dropped_ = 0;
    std::chrono::steady_clock::time_point statistics_since_;
};

}  // namespace farland::app
