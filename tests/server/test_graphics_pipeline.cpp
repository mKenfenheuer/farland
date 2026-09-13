// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The Graphics Pipeline against a scripted client that does what FreeRDP's
// client does: drdynvc, the RDPGFX capability exchange, ZGFX decompression,
// and planar decoding onto its surface.

#include <farland/channels/drdynvc.hpp>
#include <farland/channels/rdpgfx.hpp>
#include <farland/channels/svc.hpp>
#include <farland/codec/avc420.hpp>
#include <farland/codec/planar.hpp>
#include <farland/codec/progressive.hpp>
#include <farland/codec/zgfx.hpp>
#include <farland/server/graphics_pipeline.hpp>
#include <farland/server/test_pattern.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <deque>
#include <utility>
#include <vector>

namespace dyn = farland::channels::drdynvc;
namespace gfx = farland::channels::rdpgfx;
namespace svc = farland::channels::svc;
namespace pe = farland::server::pipeline_event;
using farland::server::DynamicChannels;
using farland::server::GraphicsPipeline;
using farland::server::TileCodec;
using Bytes = std::vector<std::byte>;

namespace {

constexpr std::uint16_t width = 150;  // not a multiple of the tile size
constexpr std::uint16_t height = 100;

/// The client end: drdynvc, one GFX channel, a surface.
class Client {
public:
    DynamicChannels::SendChunk sink()
    {
        return [this](std::span<const std::byte> chunk) { chunks_.emplace_back(chunk.begin(), chunk.end()); };
    }

    /// drdynvc PDUs the server sent since the last call.
    std::vector<dyn::ServerPdu> take()
    {
        std::vector<dyn::ServerPdu> out;
        for (const auto& chunk : std::exchange(chunks_, {})) {
            if (auto message = reassembler_.add(chunk).value()) {
                storage_.push_back(std::move(*message));
                out.push_back(dyn::decode_server_pdu(storage_.back()).value());
            }
        }
        return out;
    }

    /// RDPGFX PDUs the server sent on `channel` since the last call:
    /// reassembles DVC messages, decompresses them and splits the PDUs.
    std::vector<gfx::Pdu> take_gfx(std::uint32_t channel)
    {
        std::vector<gfx::Pdu> out;
        for (const auto& pdu : take()) {
            if (const auto* first = std::get_if<dyn::DataFirst>(&pdu)) {
                REQUIRE(first->channel_id == channel);
                expected_ = first->length;
                message_.assign(first->data.begin(), first->data.end());
            } else if (const auto* data = std::get_if<dyn::Data>(&pdu)) {
                REQUIRE(data->channel_id == channel);
                message_.insert(message_.end(), data->data.begin(), data->data.end());
            } else {
                continue;
            }
            if (expected_ != 0 && message_.size() < expected_) {
                continue;
            }
            storage_.push_back(zgfx_.decompress(message_).value());
            message_.clear();
            expected_ = 0;
            std::span<const std::byte> rest(storage_.back());
            while (!rest.empty()) {
                const auto size = gfx::frame_pdu(rest, std::size_t{1} << 20U).value();
                REQUIRE(size.has_value());
                out.push_back(gfx::decode_pdu(rest.first(*size)).value());
                rest = rest.subspan(*size);
            }
        }
        return out;
    }

    Bytes surface = Bytes(std::size_t{width} * height * 4);

    /// Paints a planar WireToSurface1 onto the surface.
    void paint(const gfx::WireToSurface1& w)
    {
        REQUIRE(w.codec_id == gfx::codec::planar);
        const auto rw = w.dest_rect.width();
        const auto rh = w.dest_rect.height();
        Bytes decoded(std::size_t{rw} * rh * 4);
        REQUIRE(farland::codec::planar::decode(w.bitmap_data, rw, rh, farland::codec::planar::Orientation::top_down,
                                               decoded)
                    .has_value());
        for (std::uint32_t y = 0; y < rh; ++y) {
            std::copy_n(decoded.begin() + static_cast<std::ptrdiff_t>(std::size_t{y} * rw * 4), rw * 4,
                        surface.begin() + static_cast<std::ptrdiff_t>(
                                              ((std::size_t{w.dest_rect.top + y} * width) + w.dest_rect.left) * 4));
        }
    }

private:
    std::vector<Bytes> chunks_;
    svc::Reassembler reassembler_{std::size_t{1} << 20U};
    std::deque<Bytes> storage_;
    Bytes message_;
    std::size_t expected_ = 0;
    farland::codec::ZgfxDecompressor zgfx_;
};

void send(DynamicChannels& channels, const dyn::ClientPdu& pdu)
{
    for (const auto& chunk : svc::encode_chunks(dyn::encode_client_pdu(pdu))) {
        REQUIRE(channels.receive(chunk).has_value());
    }
}

/// Delivers DVC events to the pipeline, as the session does.
void dispatch(DynamicChannels& channels, GraphicsPipeline& pipeline)
{
    while (auto event = channels.poll_event()) {
        CHECK(pipeline.handle(*event));
    }
}

/// Pixels equal, ignoring the X byte.
bool same_picture(const Bytes& surface, const farland::codec::ImageView& frame)
{
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            for (std::size_t c = 0; c < 3; ++c) {
                if (surface.at(((std::size_t{y} * width + x) * 4) + c) !=
                    frame.data[(y * frame.stride) + (x * 4) + c]) {
                    return false;
                }
            }
        }
    }
    return true;
}

/// drdynvc up, capabilities done.
void start_channels(Client& client, DynamicChannels& channels)
{
    channels.start();
    static_cast<void>(client.take());
    send(channels, dyn::CapsResponse{dyn::version3});
    REQUIRE(std::holds_alternative<farland::channels::dvc_event::CapabilitiesReady>(channels.poll_event().value()));
}

/// Accepts the GFX channel, advertises 10.7 and consumes the surface setup.
/// Returns the channel ID.
std::uint32_t establish(Client& client, DynamicChannels& channels, GraphicsPipeline& pipeline)
{
    const auto id = std::get<dyn::CreateRequest>(client.take().at(0)).channel_id;
    send(channels, dyn::CreateResponse{id, 0});
    dispatch(channels, pipeline);
    const gfx::CapsAdvertise advertise{{gfx::make_capability_set(gfx::cap_version::v10_7, 0)}};
    send(channels, dyn::Data{id, gfx::encode(gfx::Pdu{advertise})});
    dispatch(channels, pipeline);
    REQUIRE(pipeline.ready());
    REQUIRE(std::holds_alternative<pe::Ready>(pipeline.poll_event().value()));
    REQUIRE(client.take_gfx(id).size() == 4);
    return id;
}

/// Peak signal-to-noise ratio over B, G and R, in dB.
double psnr(const farland::codec::ImageView& a, const farland::codec::ImageView& b)
{
    double sum = 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            for (std::size_t c = 0; c < 3; ++c) {
                const double d = std::to_integer<int>(a.data[(y * a.stride) + (x * 4) + c]) -
                                 std::to_integer<int>(b.data[(y * b.stride) + (x * 4) + c]);
                sum += d * d;
            }
        }
    }
    const double mse = sum / (double{width} * height * 3);
    return mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

}  // namespace

TEST_CASE("GFX: capability exchange, surface layout, and frames of changed planar tiles")
{
    Client client;
    DynamicChannels channels(client.sink());
    channels.start();
    static_cast<void>(client.take());
    send(channels, dyn::CapsResponse{dyn::version3});
    REQUIRE(std::holds_alternative<farland::channels::dvc_event::CapabilitiesReady>(channels.poll_event().value()));

    gfx::GfxServerConfig config;
    config.avc420 = config.avc444 = config.avc444v2 = false;
    GraphicsPipeline pipeline(channels, width, height, config, TileCodec::planar);
    const auto requests = client.take();
    REQUIRE(requests.size() == 1);
    const auto& create = std::get<dyn::CreateRequest>(requests[0]);
    CHECK(create.name == GraphicsPipeline::channel_name);
    const auto id = create.channel_id;
    send(channels, dyn::CreateResponse{id, 0});
    dispatch(channels, pipeline);
    CHECK_FALSE(pipeline.ready());

    // The client advertises 8.1 and 10.7; the server confirms 10.7 and lays out the surface.
    const gfx::CapsAdvertise advertise{
        {gfx::make_capability_set(gfx::cap_version::v8_1, 0), gfx::make_capability_set(gfx::cap_version::v10_7, 0)}};
    send(channels, dyn::Data{id, gfx::encode(gfx::Pdu{advertise})});
    dispatch(channels, pipeline);
    REQUIRE(pipeline.ready());
    CHECK(std::get<pe::Ready>(pipeline.poll_event().value()).negotiated.version == gfx::cap_version::v10_7);
    auto pdus = client.take_gfx(id);
    REQUIRE(pdus.size() == 4);
    CHECK(std::get<gfx::CapsConfirm>(pdus[0]).caps_set.version == gfx::cap_version::v10_7);
    const auto& reset = std::get<gfx::ResetGraphics>(pdus[1]);
    CHECK((reset.width == width && reset.height == height));
    const auto& surface = std::get<gfx::CreateSurface>(pdus[2]);
    CHECK((surface.width == width && surface.height == height));
    CHECK(std::get<gfx::MapSurfaceToOutput>(pdus[3]).surface_id == surface.surface_id);

    // The first frame covers all 3 x 2 tiles.
    farland::server::TestPattern pattern(width, height);
    const auto frame = pattern.render(0);
    const auto frame_id = pipeline.send_frame(frame);
    REQUIRE(frame_id.has_value());
    pdus = client.take_gfx(id);
    REQUIRE(pdus.size() == 2 + 6);
    CHECK(std::get<gfx::StartFrame>(pdus.front()).frame_id == *frame_id);
    CHECK(std::get<gfx::EndFrame>(pdus.back()).frame_id == *frame_id);
    for (std::size_t i = 1; i + 1 < pdus.size(); ++i) {
        client.paint(std::get<gfx::WireToSurface1>(pdus[i]));
    }
    CHECK(same_picture(client.surface, frame));

    // Nothing changed: no frame.
    CHECK_FALSE(pipeline.send_frame(frame).has_value());

    // The client acknowledges the frame.
    send(channels, dyn::Data{id, gfx::encode(gfx::Pdu{gfx::FrameAcknowledge{0, *frame_id, 1}})});
    dispatch(channels, pipeline);
    const auto ack = std::get<pe::FrameAcked>(pipeline.poll_event().value());
    CHECK((ack.frame_id == *frame_id && ack.known));

    // Later frames carry only what moved, and the surface stays right.
    const auto later = pattern.render(10);
    const auto second = pipeline.send_frame(later);
    REQUIRE(second.has_value());
    pdus = client.take_gfx(id);
    CHECK(pdus.size() >= 3);
    for (std::size_t i = 1; i + 1 < pdus.size(); ++i) {
        client.paint(std::get<gfx::WireToSurface1>(pdus[i]));
    }
    CHECK(same_picture(client.surface, later));
}

TEST_CASE("GFX: a refused channel closes the pipeline")
{
    Client client;
    DynamicChannels channels(client.sink());
    channels.start();
    static_cast<void>(client.take());
    send(channels, dyn::CapsResponse{dyn::version3});
    static_cast<void>(channels.poll_event());

    GraphicsPipeline pipeline(channels, width, height);
    const auto id = std::get<dyn::CreateRequest>(client.take().at(0)).channel_id;
    send(channels, dyn::CreateResponse{id, static_cast<std::int32_t>(0x80004005)});  // E_FAIL
    dispatch(channels, pipeline);
    CHECK(pipeline.closed());
    CHECK(std::holds_alternative<pe::Closed>(pipeline.poll_event().value()));
}

TEST_CASE("GFX: Progressive frames decode to the picture")
{
    Client client;
    DynamicChannels channels(client.sink());
    start_channels(client, channels);
    gfx::GfxServerConfig config;
    config.avc420 = config.avc444 = config.avc444v2 = false;
    GraphicsPipeline pipeline(channels, width, height, config);  // Progressive by default
    const auto id = establish(client, channels, pipeline);
    CHECK(pipeline.codec() == TileCodec::progressive);

    auto decoder = farland::codec::progressive::Decoder::create(width, height).value();
    farland::server::TestPattern pattern(width, height);
    for (const std::uint64_t n : {0U, 7U, 20U}) {
        const auto frame = pattern.render(n);
        const auto frame_id = pipeline.send_frame(frame);
        REQUIRE(frame_id.has_value());
        const auto pdus = client.take_gfx(id);
        REQUIRE(pdus.size() >= 3);
        for (std::size_t i = 1; i + 1 < pdus.size(); ++i) {
            const auto& w = std::get<gfx::WireToSurface2>(pdus[i]);
            CHECK(w.codec_id == gfx::codec::progressive);
            CHECK(w.bitmap_data.size() <= std::size_t{16} * 1024);
            REQUIRE(decoder.decode(w.bitmap_data, *frame_id).has_value());
        }
        CHECK(psnr(decoder.image(), frame) >= 30.0);
    }
}

TEST_CASE("GFX: thin clients get planar instead of Progressive")
{
    Client client;
    DynamicChannels channels(client.sink());
    start_channels(client, channels);
    GraphicsPipeline pipeline(channels, width, height);
    const auto id = std::get<dyn::CreateRequest>(client.take().at(0)).channel_id;
    send(channels, dyn::CreateResponse{id, 0});
    dispatch(channels, pipeline);
    const gfx::CapsAdvertise advertise{{gfx::make_capability_set(gfx::cap_version::v8, gfx::caps_flag::thin_client)}};
    send(channels, dyn::Data{id, gfx::encode(gfx::Pdu{advertise})});
    dispatch(channels, pipeline);
    REQUIRE(pipeline.ready());
    CHECK(pipeline.codec() == TileCodec::planar);
}

TEST_CASE("GFX: AVC420 frames carry an H.264 access unit and region rectangles")
{
    // OpenH264 is loaded at runtime (or x264 compiled in); without either, skip.
    const farland::server::H264Factory factory = [](const farland::video::EncoderConfig& config) {
        farland::video::BackendOptions options;
        if (const char* library = std::getenv("FARLAND_OPENH264_LIBRARY")) {
            options.openh264_library = library;
        }
        return farland::video::create_encoder(farland::video::compiled_backends(), config, options);
    };
    {
        farland::video::EncoderConfig probe;
        probe.width = farland::codec::avc::coded_size(width);
        probe.height = farland::codec::avc::coded_size(height);
        if (!factory(probe)) {
            SKIP("no H.264 encoder available (set FARLAND_OPENH264_LIBRARY)");
        }
    }
    Client client;
    DynamicChannels channels(client.sink());
    start_channels(client, channels);
    gfx::GfxServerConfig config;
    config.avc444 = config.avc444v2 = false;
    GraphicsPipeline pipeline(channels, width, height, config, TileCodec::avc420, factory);
    const auto id = establish(client, channels, pipeline);
    CHECK(pipeline.codec() == TileCodec::avc420);

    farland::server::TestPattern pattern(width, height);
    for (const std::uint64_t n : {0U, 5U}) {
        REQUIRE(pipeline.send_frame(pattern.render(n)).has_value());
        const auto pdus = client.take_gfx(id);
        REQUIRE(pdus.size() == 3);
        const auto& w = std::get<gfx::WireToSurface1>(pdus[1]);
        CHECK(w.codec_id == gfx::codec::avc420);
        const auto stream = farland::codec::avc::decode_avc420(w.bitmap_data).value();
        CHECK_FALSE(stream.bitstream.empty());
        REQUIRE_FALSE(stream.regions.empty());
        if (n == 0) {
            // The first frame is an IDR and lists the whole surface.
            REQUIRE(stream.regions.size() == 1);
            const auto& r = stream.regions[0].rect;
            CHECK((r.left == 0 && r.top == 0 && r.right == width && r.bottom == height));
        }
        for (const auto& region : stream.regions) {
            CHECK((region.rect.right <= width && region.rect.bottom <= height));
            CHECK((region.rect.left >= w.dest_rect.left && region.rect.right <= w.dest_rect.right));
        }
    }
}
