// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The camera server against a scripted client that does what FreeRDP's
// rdpecam client does: drdynvc, the version handshake on the enumeration
// channel, a device channel per camera, and one sample per request.

#include <farland/channels/drdynvc.hpp>
#include <farland/channels/svc.hpp>
#include <farland/server/camera.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace dyn = farland::channels::drdynvc;
namespace svc = farland::channels::svc;
namespace cam = farland::channels::rdpecam;
namespace ev = farland::server::camera_event;
using farland::server::CameraOptions;
using farland::server::CameraServer;
using farland::server::DynamicChannels;
using Bytes = std::vector<std::byte>;

namespace {

cam::MediaType media_type(cam::MediaFormat format, std::uint32_t width, std::uint32_t height, std::uint32_t fps,
                          std::uint8_t flags = 0)
{
    return {.format = format,
            .width = width,
            .height = height,
            .frame_rate_numerator = fps,
            .frame_rate_denominator = 1,
            .pixel_aspect_numerator = 1,
            .pixel_aspect_denominator = 1,
            .flags = flags};
}

/// The client end: drdynvc, the camera channels, and what the server sent on
/// each of them.
class Client {
public:
    DynamicChannels::SendChunk sink()
    {
        return [this](std::span<const std::byte> chunk) { chunks_.emplace_back(chunk.begin(), chunk.end()); };
    }

    /// drdynvc PDUs since the last call, with created channels remembered.
    std::vector<dyn::ServerPdu> take()
    {
        poll();
        return std::exchange(pending_, {});
    }

    /// The camera PDUs the server sent on `channel` since the last call.
    /// PDUs for other channels stay for a later call.
    std::vector<cam::ServerPdu> take_camera(std::uint32_t channel)
    {
        poll();
        std::vector<cam::ServerPdu> out;
        std::vector<dyn::ServerPdu> kept;
        for (auto& pdu : pending_) {
            const auto* data = std::get_if<dyn::Data>(&pdu);
            if (data == nullptr || data->channel_id != channel) {
                kept.push_back(std::move(pdu));
                continue;
            }
            auto decoded = cam::decode_server_pdu(data->data);
            INFO((decoded ? std::string("ok") : decoded.error().message()));
            REQUIRE(decoded.has_value());
            out.push_back(std::move(*decoded));
        }
        pending_ = std::move(kept);
        return out;
    }

    /// The id the server opened for `name`, if it did.
    [[nodiscard]] std::optional<std::uint32_t> channel_for(std::string_view name)
    {
        poll();
        for (const auto& [id, channel] : channels_) {
            if (channel == name) {
                return id;
            }
        }
        return std::nullopt;
    }

private:
    /// Decodes everything the server has written so far; the channels it
    /// opened are remembered, and the PDUs wait for take().
    void poll()
    {
        for (const auto& chunk : std::exchange(chunks_, {})) {
            if (auto message = reassembler_.add(chunk).value()) {
                storage_.push_back(std::move(*message));
                auto pdu = dyn::decode_server_pdu(storage_.back()).value();
                if (const auto* create = std::get_if<dyn::CreateRequest>(&pdu)) {
                    channels_[create->channel_id] = create->name;
                }
                pending_.push_back(std::move(pdu));
            }
        }
    }

    std::vector<Bytes> chunks_;
    std::vector<dyn::ServerPdu> pending_;
    svc::Reassembler reassembler_{std::size_t{1} << 20U};
    std::deque<Bytes> storage_;
    std::map<std::uint32_t, std::string> channels_;
};

void send(DynamicChannels& channels, const dyn::ClientPdu& pdu)
{
    for (const auto& chunk : svc::encode_chunks(dyn::encode_client_pdu(pdu))) {
        const auto received = channels.receive(chunk);
        INFO((received ? std::string("ok") : received.error().message()));
        REQUIRE(received.has_value());
    }
}

/// One camera PDU from the client, split into DYNVC_DATA_FIRST and
/// DYNVC_DATA the way a real client must: a drdynvc PDU is small, and a
/// camera frame is not (a 640x480 NV12 picture is 450 KB).
void send_dvc(DynamicChannels& channels, std::uint32_t channel, std::span<const std::byte> message)
{
    // What fits one PDU, leaving room for its header.
    constexpr std::size_t piece = 1000;
    if (message.size() <= piece) {
        send(channels, dyn::Data{channel, message});
        return;
    }
    send(channels, dyn::DataFirst{channel, static_cast<std::uint32_t>(message.size()), message.first(piece)});
    for (std::size_t at = piece; at < message.size(); at += piece) {
        send(channels, dyn::Data{channel, message.subspan(at, std::min(piece, message.size() - at))});
    }
}

void dispatch(DynamicChannels& channels, CameraServer& camera)
{
    while (auto event = channels.poll_event()) {
        static_cast<void>(camera.handle(*event));
    }
}

/// A camera session driven to the point where it streams.
struct Fixture {
    Client client;
    DynamicChannels channels{client.sink()};
    std::optional<CameraServer> camera;
    std::uint32_t enumerator = 0;
    std::uint32_t device = 0;
    std::vector<ev::Opened> opened;
    std::vector<Bytes> frames;
    std::vector<std::string> closed;

    explicit Fixture(CameraOptions options = {})
    {
        channels.start();
        static_cast<void>(client.take());
        send(channels, dyn::CapsResponse{dyn::version3});
        REQUIRE(std::holds_alternative<farland::channels::dvc_event::CapabilitiesReady>(channels.poll_event().value()));
        camera.emplace(channels, options);
        // The server asks for the enumeration channel.
        const auto request = std::get<dyn::CreateRequest>(client.take().at(0));
        CHECK(request.name == cam::enumerator_channel_name);
        enumerator = request.channel_id;
        send(channels, dyn::CreateResponse{enumerator, 0});
        dispatch(channels, *camera);
    }

    void client_says(std::uint32_t channel, const cam::ClientPdu& pdu)
    {
        const auto message = cam::encode_client_pdu(pdu);
        send_dvc(channels, channel, message);
        dispatch(channels, *camera);
        drain();
    }

    void drain()
    {
        while (auto event = camera->poll_event()) {
            std::visit(
                [this](const auto& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, ev::Opened>) {
                        opened.push_back(e);
                    } else if constexpr (std::is_same_v<T, ev::Frame>) {
                        frames.push_back(e.data);
                    } else if constexpr (std::is_same_v<T, ev::Closed>) {
                        closed.push_back(e.reason);
                    }
                },
                *event);
            camera->request_frame();
        }
    }

    /// Version handshake, one camera, one stream, the media types given, and
    /// the Success Response that starts it.
    void bring_up(const std::vector<cam::MediaType>& types)
    {
        client_says(enumerator, cam::SelectVersionRequest{cam::version2});
        client_says(enumerator, cam::DeviceAdded{"Integrated Camera", "rdpecam0"});
        const auto id = client.channel_for("rdpecam0");
        REQUIRE(id.has_value());
        device = *id;
        send(channels, dyn::CreateResponse{device, 0});
        dispatch(channels, *camera);
        // Activate and ask what the camera has.
        const auto opening = client.take_camera(device);
        REQUIRE(opening.size() == 2);
        CHECK(std::holds_alternative<cam::ActivateDeviceRequest>(opening[0]));
        CHECK(std::holds_alternative<cam::StreamListRequest>(opening[1]));
        cam::StreamListResponse streams;
        streams.streams.push_back({cam::frame_source::color, cam::stream_category_capture, true, false});
        client_says(device, streams);
        // The stream list brings the request for that stream's media types.
        const auto asked = client.take_camera(device);
        REQUIRE(asked.size() == 1);
        CHECK(std::get<cam::MediaTypeListRequest>(asked[0]).stream_index == 0);
        cam::MediaTypeListResponse list;
        list.media_types = types;
        client_says(device, list);
    }
};

}  // namespace

TEST_CASE("Camera: the server answers the version the client asks for", "[server][camera]")
{
    Fixture f;
    // Nothing goes out until the client speaks ([MS-RDPECAM] 3.1.5.1).
    CHECK(f.client.take_camera(f.enumerator).empty());

    f.client_says(f.enumerator, cam::SelectVersionRequest{cam::version2});
    auto sent = f.client.take_camera(f.enumerator);
    REQUIRE(sent.size() == 1);
    CHECK(std::get<cam::SelectVersionResponse>(sent[0]).version == cam::version2);

    // A client that speaks only version 1 is answered with version 1.
    Fixture older;
    older.client_says(older.enumerator, cam::SelectVersionRequest{cam::version1});
    sent = older.client.take_camera(older.enumerator);
    REQUIRE(sent.size() == 1);
    CHECK(std::get<cam::SelectVersionResponse>(sent[0]).version == cam::version1);
}

TEST_CASE("Camera: a camera is activated, negotiated and streamed", "[server][camera]")
{
    Fixture f;
    f.bring_up({media_type(cam::MediaFormat::nv12, 640, 480, 30)});

    // The media type list brings a Start Streams Request for the chosen type.
    const auto started = f.client.take_camera(f.device);
    REQUIRE(started.size() == 1);
    const auto& start = std::get<cam::StartStreamsRequest>(started[0]);
    CHECK(start.stream_index == 0);
    CHECK(start.media_type == media_type(cam::MediaFormat::nv12, 640, 480, 30));

    // The client accepts; the camera opens and the first sample is asked for.
    f.client_says(f.device, cam::SuccessResponse{});
    REQUIRE(f.opened.size() == 1);
    CHECK(f.opened[0].device_name == "Integrated Camera");
    CHECK(f.opened[0].media_type.width == 640);
    CHECK(f.camera->streaming());
    auto asked = f.client.take_camera(f.device);
    REQUIRE(asked.size() == 1);
    CHECK(std::get<cam::SampleRequest>(asked[0]).stream_index == 0);

    // One sample per request, and each answer brings the next request.
    const Bytes frame(cam::frame_size(cam::MediaFormat::nv12, 640, 480), std::byte{0x7F});
    for (int i = 0; i < 3; ++i) {
        f.client_says(f.device, cam::SampleResponse{0, frame});
        asked = f.client.take_camera(f.device);
        REQUIRE(asked.size() == 1);
        CHECK(std::holds_alternative<cam::SampleRequest>(asked[0]));
    }
    REQUIRE(f.frames.size() == 3);
    CHECK(f.frames[0].size() == frame.size());
    CHECK(f.closed.empty());
}

TEST_CASE("Camera: a frame of the wrong size gives the camera up", "[server][camera]")
{
    Fixture f;
    f.bring_up({media_type(cam::MediaFormat::nv12, 640, 480, 30)});
    static_cast<void>(f.client.take_camera(f.device));
    f.client_says(f.device, cam::SuccessResponse{});
    static_cast<void>(f.client.take_camera(f.device));

    const Bytes short_frame(100, std::byte{0});
    f.client_says(f.device, cam::SampleResponse{0, short_frame});
    CHECK(f.frames.empty());
    REQUIRE(f.closed.size() == 1);
    CHECK(f.closed[0].contains("100-byte frame"));
    CHECK_FALSE(f.camera->streaming());
    CHECK_FALSE(f.camera->closed());  // the enumerator stays, for the next camera
}

TEST_CASE("Camera: a camera with nothing but compressed formats is not used", "[server][camera]")
{
    Fixture f;
    f.bring_up({media_type(cam::MediaFormat::h264, 1280, 720, 30), media_type(cam::MediaFormat::mjpg, 640, 480, 30),
                // Uncompressed, but the client says it has to be decoded.
                media_type(cam::MediaFormat::nv12, 640, 480, 30, cam::media_flag::decoding_required)});
    CHECK(f.opened.empty());
    REQUIRE(f.closed.size() == 1);
    CHECK(f.closed[0].contains("no format farland can read"));
    // Nothing was started.
    CHECK(f.client.take_camera(f.device).empty());
}

TEST_CASE("Camera: the media type chosen is the largest usable one", "[server][camera]")
{
    const CameraOptions options{.max_width = 1280, .max_height = 720, .preferred_fps = 30};
    const std::vector<cam::MediaType> offered{
        media_type(cam::MediaFormat::h264, 1920, 1080, 30),  // needs a decoder
        media_type(cam::MediaFormat::nv12, 1920, 1080, 30),  // too large
        media_type(cam::MediaFormat::yuy2, 640, 480, 30),    // usable, smaller
        media_type(cam::MediaFormat::nv12, 1280, 720, 15),   // usable, the wrong rate
        media_type(cam::MediaFormat::nv12, 1280, 720, 30),   // the one
        media_type(cam::MediaFormat::rgb32, 1280, 720, 30),  // same size, costlier format
    };
    const auto chosen = CameraServer::choose_media_type(offered, options);
    REQUIRE(chosen.has_value());
    CHECK(chosen->format == cam::MediaFormat::nv12);
    CHECK(chosen->width == 1280);
    CHECK(chosen->height == 720);
    CHECK(chosen->fps() == 30);

    // With no uncompressed type inside the limits there is no choice at all.
    const std::vector<cam::MediaType> impossible{media_type(cam::MediaFormat::mjpg, 640, 480, 30),
                                                 media_type(cam::MediaFormat::nv12, 3840, 2160, 30)};
    CHECK_FALSE(CameraServer::choose_media_type(impossible, options).has_value());
}

TEST_CASE("Camera: a camera offered while one runs is taken when that one goes", "[server][camera]")
{
    Fixture f;
    f.bring_up({media_type(cam::MediaFormat::nv12, 640, 480, 30)});
    static_cast<void>(f.client.take_camera(f.device));
    f.client_says(f.device, cam::SuccessResponse{});
    static_cast<void>(f.client.take_camera(f.device));
    REQUIRE(f.camera->streaming());

    // A second camera arrives; it waits.
    f.client_says(f.enumerator, cam::DeviceAdded{"USB Camera", "rdpecam1"});
    CHECK_FALSE(f.client.channel_for("rdpecam1").has_value());

    // The first is unplugged: the second is opened in its place.
    f.client_says(f.enumerator, cam::DeviceRemoved{"rdpecam0"});
    REQUIRE(f.closed.size() == 1);
    const auto second = f.client.channel_for("rdpecam1");
    REQUIRE(second.has_value());
    send(f.channels, dyn::CreateResponse{*second, 0});
    dispatch(f.channels, *f.camera);
    const auto opening = f.client.take_camera(*second);
    REQUIRE(opening.size() == 2);
    CHECK(std::holds_alternative<cam::ActivateDeviceRequest>(opening[0]));
}

TEST_CASE("Camera: a client without the channel closes the camera cleanly", "[server][camera]")
{
    Client client;
    DynamicChannels channels(client.sink());
    channels.start();
    static_cast<void>(client.take());
    send(channels, dyn::CapsResponse{dyn::version3});
    REQUIRE(std::holds_alternative<farland::channels::dvc_event::CapabilitiesReady>(channels.poll_event().value()));
    CameraServer camera(channels);
    const auto request = std::get<dyn::CreateRequest>(client.take().at(0));
    send(channels, dyn::CreateResponse{request.channel_id, -1});
    dispatch(channels, camera);

    const auto event = camera.poll_event();
    REQUIRE(event.has_value());
    CHECK(std::get<ev::Closed>(*event).reason.contains("no camera channel"));
    CHECK(camera.closed());
}
