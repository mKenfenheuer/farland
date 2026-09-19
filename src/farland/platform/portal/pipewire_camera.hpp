// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/platform/video.hpp>

#include <memory>
#include <string>

/// The client's camera as a camera of the session's own, through the user's
/// PipeWire daemon. Applications that use PipeWire directly (Firefox,
/// Chromium, GNOME's Camera, anything on the portal's Camera interface) list
/// it like any other webcam; those that insist on a /dev/video node do not
/// see it, which needs v4l2loopback and is out of farland's reach.
namespace farland::platform::portal {

struct VirtualCameraOptions {
    /// node.name and node.description, as applications list the camera.
    std::string node_name = "farland-camera";
    std::string description = "farland camera";
    /// The frames the node keeps; one in flight and one being filled.
    std::uint32_t buffers = 2;
};

/// A virtual camera (a stream node with media.class Video/Source) that local
/// applications can capture from; it goes away with the object.
[[nodiscard]] Result<std::unique_ptr<VideoSink>> create_virtual_camera(const VideoMode& mode,
                                                                       const VirtualCameraOptions& options = {});

}  // namespace farland::platform::portal
