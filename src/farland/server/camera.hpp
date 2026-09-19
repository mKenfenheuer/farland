// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/channels/rdpecam.hpp>
#include <farland/server/dynamic_channels.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

/// The client's camera ([MS-RDPECAM]) over the device enumeration channel and
/// one channel per camera: opens the enumerator, agrees a version, takes the
/// first camera the client offers, picks a media type it can use, starts the
/// stream and turns the client's samples into frames for a local camera.
///
/// One camera at a time. A desktop shows one webcam, and every extra camera
/// would need its own channel, its own media type and its own PipeWire node
/// for no gain that anyone asked for; the others stay listed and unused, and
/// one is taken up if the one in use goes away.
///
/// farland has no H.264 or Motion JPEG decoder, so only the uncompressed
/// media types are usable (YUY2, NV12, I420, RGB24, RGB32). That is no real
/// restriction: a client converts for the server, so FreeRDP offers those
/// formats for a camera whose hardware produces MJPG. A client that offers
/// nothing else closes the camera with a clear message rather than sending
/// samples nothing can read.
///
/// Sample flow is one at a time ([MS-RDPECAM] 3.1.5.6): the server asks for a
/// sample, the client answers with one, the server asks again. A camera
/// consumer that falls behind therefore slows the client down instead of
/// building a queue, which is what a live camera wants.
namespace farland::server {

namespace camera_event {
/// The camera is streaming; Frames in `media_type` follow.
struct Opened {
    std::string device_name;
    channels::rdpecam::MediaType media_type;
};
/// One frame, in the Opened media type. It owns its bytes: the client's
/// message is gone by the time the session polls the event.
struct Frame {
    std::vector<std::byte> data;
};
/// The camera is gone: none offered, none usable, or the client closed it.
struct Closed {
    std::string reason;
};
}  // namespace camera_event

using CameraEvent = std::variant<camera_event::Opened, camera_event::Frame, camera_event::Closed>;

struct CameraOptions {
    /// Largest picture taken from the client. A camera that only offers more
    /// than this is used at its smallest size anyway; the cap is there so
    /// that a client cannot make the server carry 4K frames per camera when
    /// a smaller one would do.
    std::uint32_t max_width = 1920;
    std::uint32_t max_height = 1080;
    /// Frame rate aimed for when the camera offers a choice.
    std::uint32_t preferred_fps = 30;
};

class CameraServer {
public:
    /// Opens the enumeration channel on `channels` (whose capabilities must
    /// be ready); `channels` must outlive this object.
    explicit CameraServer(DynamicChannels& channels, CameraOptions options = {});

    /// Handles `event` if it concerns one of the camera channels. False otherwise.
    bool handle(const channels::DvcEvent& event);
    [[nodiscard]] std::optional<CameraEvent> poll_event();

    /// Asks the client for the next frame. Call once the frame just polled
    /// has been consumed; nothing happens unless a camera is streaming.
    void request_frame();

    /// Stops the stream and deactivates the camera, without closing the
    /// enumeration channel: another camera can still be offered.
    void stop();

    [[nodiscard]] bool streaming() const noexcept { return state_ == State::streaming; }
    [[nodiscard]] bool closed() const noexcept { return closed_; }
    /// The media type in use; only while streaming().
    [[nodiscard]] const channels::rdpecam::MediaType& media_type() const noexcept { return media_type_; }

    /// The media type to ask for among the ones the client offered, or
    /// nullopt when none can be used: the largest uncompressed picture that
    /// fits the limits, and among equals the one closest to the wanted frame
    /// rate. Exposed for the tests.
    [[nodiscard]] static std::optional<channels::rdpecam::MediaType>
    choose_media_type(std::span<const channels::rdpecam::MediaType> offered, const CameraOptions& options);

private:
    enum class State : std::uint8_t {
        /// Waiting for the enumerator channel and the version handshake.
        connecting,
        /// The version is agreed; waiting for a camera to be offered.
        listening,
        /// A device channel is opening.
        opening,
        /// Activated; waiting for the stream list.
        streams,
        /// Waiting for the media type list.
        media_types,
        /// Start Streams sent; waiting for the Success Response.
        starting,
        streaming,
        /// The camera failed or went away; the enumerator stays open.
        idle,
    };

    void send_enumerator(const channels::rdpecam::ServerPdu& pdu);
    void send_device(const channels::rdpecam::ServerPdu& pdu);
    [[nodiscard]] Result<void> on_enumerator(std::span<const std::byte> message);
    [[nodiscard]] Result<void> on_device(std::span<const std::byte> message);
    void open_device(const std::string& device_name, const std::string& channel_name);
    /// Gives the camera up and tells the session why; the enumerator stays
    /// open, so a camera offered later is taken.
    void drop_device(std::string reason);
    void close(std::string reason);

    DynamicChannels* channels_;  ///< never null
    CameraOptions options_;
    std::uint32_t enumerator_id_ = 0;
    std::optional<std::uint32_t> device_id_;
    std::uint8_t version_ = channels::rdpecam::version2;
    State state_ = State::connecting;
    bool closed_ = false;
    std::string device_name_;
    std::string device_channel_;
    /// Cameras offered while one was in use, in the order they arrived.
    std::vector<std::pair<std::string, std::string>> spare_;
    std::uint8_t stream_index_ = 0;
    channels::rdpecam::MediaType media_type_;
    /// A sample was asked for and has not arrived; keeps the one-at-a-time
    /// flow from running two requests at once.
    bool sample_pending_ = false;
    std::deque<CameraEvent> events_;
};

}  // namespace farland::server
