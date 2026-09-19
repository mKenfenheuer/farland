// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The Graphics Pipeline against a scripted client that does what FreeRDP's
// client does: drdynvc, the RDPGFX capability exchange, ZGFX decompression,
// and planar decoding onto its surface.

#include <farland/channels/drdynvc.hpp>
#include <farland/channels/rdpgfx.hpp>
#include <farland/channels/svc.hpp>
#include <farland/codec/avc420.hpp>
#include <farland/codec/clear.hpp>
#include <farland/codec/planar.hpp>
#include <farland/codec/progressive.hpp>
#include <farland/codec/zgfx.hpp>
#include <farland/server/display_layout.hpp>
#include <farland/server/graphics_pipeline.hpp>
#include <farland/server/quality_controller.hpp>
#include <farland/server/test_pattern.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <optional>
#include <tuple>
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

    /// Applies a SurfaceToSurface within the one surface, as FreeRDP does
    /// (the source is read completely before the destination is written).
    void copy(const gfx::SurfaceToSurface& s)
    {
        REQUIRE(s.surface_id_src == s.surface_id_dest);
        const auto rw = s.rect_src.width();
        const auto rh = s.rect_src.height();
        Bytes block(std::size_t{rw} * rh * 4);
        for (std::uint32_t y = 0; y < rh; ++y) {
            std::copy_n(surface.begin() + static_cast<std::ptrdiff_t>(
                                              ((std::size_t{s.rect_src.top + y} * width) + s.rect_src.left) * 4),
                        rw * 4, block.begin() + static_cast<std::ptrdiff_t>(std::size_t{y} * rw * 4));
        }
        for (const auto& point : s.dest_pts) {
            REQUIRE((point.x >= 0 && point.y >= 0 && point.x + rw <= width && point.y + rh <= height));
            for (std::uint32_t y = 0; y < rh; ++y) {
                std::copy_n(block.begin() + static_cast<std::ptrdiff_t>(std::size_t{y} * rw * 4), rw * 4,
                            surface.begin() + static_cast<std::ptrdiff_t>(
                                                  ((std::size_t(point.y) + y) * width + std::size_t(point.x)) * 4));
            }
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
    // Single-pass Progressive only; ClearCodec and refinement have a test of their own.
    GraphicsPipeline pipeline(channels, width, height, config, TileCodec::progressive, {},
                              farland::server::PipelineOptions{.clearcodec = false, .refine = false});
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

namespace {

/// What a StubEncoder saw of dmabufs, and how it treats them.
struct DmabufStub {
    bool accepts = true;
    bool refuse = false;  ///< encode_dmabuf fails with Errc::unsupported
    int dmabuf_pictures = 0;
    int cpu_pictures = 0;
    int forgotten = 0;
};

/// An H.264 encoder that encodes nothing: every picture becomes an access
/// unit delimiter, the first an "IDR". Enough for the pipeline's decisions.
/// With a DmabufStub it takes dmabufs too.
class StubEncoder final : public farland::video::H264Encoder {
public:
    explicit StubEncoder(const farland::video::EncoderConfig& config, DmabufStub* dmabuf = nullptr)
        : config_(config), dmabuf_(dmabuf)
    {
    }

    [[nodiscard]] farland::video::Backend backend() const noexcept override
    {
        return farland::video::Backend::openh264;
    }
    [[nodiscard]] const farland::video::EncoderConfig& config() const noexcept override { return config_; }
    [[nodiscard]] farland::Result<void> configure(const farland::video::EncoderConfig& config) override
    {
        config_ = config;
        return {};
    }
    [[nodiscard]] farland::Result<void> set_rate_control(const farland::video::RateControl& rate) override
    {
        config_.rate = rate;
        return {};
    }
    void request_idr() noexcept override { idr_ = true; }
    [[nodiscard]] farland::Result<farland::video::EncodedFrame>
    encode(const farland::codec::Yuv420View& picture, const farland::video::FrameOptions& options) override
    {
        REQUIRE((picture.width == config_.width && picture.height == config_.height));
        if (dmabuf_ != nullptr) {
            ++dmabuf_->cpu_pictures;
        }
        return picture_out(options);
    }
    [[nodiscard]] bool accepts_dmabuf() const noexcept override { return dmabuf_ != nullptr && dmabuf_->accepts; }
    [[nodiscard]] farland::Result<farland::video::EncodedFrame>
    encode_dmabuf(const farland::video::DmabufFrame& frame, const farland::video::FrameOptions& options) override
    {
        REQUIRE(dmabuf_ != nullptr);
        REQUIRE((frame.width <= config_.width && frame.height <= config_.height && frame.plane_count == 1));
        if (dmabuf_->refuse) {
            return farland::fail(farland::Errc::unsupported, "refused");
        }
        ++dmabuf_->dmabuf_pictures;
        return picture_out(options);
    }
    void forget_dmabufs() noexcept override
    {
        if (dmabuf_ != nullptr) {
            ++dmabuf_->forgotten;
        }
    }

private:
    farland::video::EncodedFrame picture_out(const farland::video::FrameOptions& options)
    {
        farland::video::EncodedFrame frame;
        frame.bitstream = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0x09}, std::byte{0x10}};
        frame.idr = std::exchange(idr_, false) || options.force_idr;
        frame.qp = 24;
        return frame;
    }

    farland::video::EncoderConfig config_;
    DmabufStub* dmabuf_;
    bool idr_ = true;
};

const farland::server::H264Factory stub_factory = [](const farland::video::EncoderConfig& config) {
    return farland::Result<std::unique_ptr<farland::video::H264Encoder>>(std::make_unique<StubEncoder>(config));
};

/// Accepts the GFX channel, advertises `caps` and consumes the surface
/// setup. Returns the channel ID.
std::uint32_t establish_with(Client& client, DynamicChannels& channels, GraphicsPipeline& pipeline,
                             const gfx::CapabilitySet& caps)
{
    const auto id = std::get<dyn::CreateRequest>(client.take().at(0)).channel_id;
    send(channels, dyn::CreateResponse{id, 0});
    dispatch(channels, pipeline);
    send(channels, dyn::Data{id, gfx::encode(gfx::Pdu{gfx::CapsAdvertise{{caps}}})});
    dispatch(channels, pipeline);
    REQUIRE(pipeline.ready());
    REQUIRE(std::holds_alternative<pe::Ready>(pipeline.poll_event().value()));
    REQUIRE(client.take_gfx(id).size() == 4);
    return id;
}

/// The codec the pipeline picks for `caps` when AVC444 is requested.
TileCodec codec_for(const gfx::CapabilitySet& caps, std::uint16_t surface_width = width,
                    const farland::server::H264Factory& factory = stub_factory)
{
    Client client;
    DynamicChannels channels(client.sink());
    start_channels(client, channels);
    GraphicsPipeline pipeline(channels, surface_width, height, {}, TileCodec::avc444, factory);
    static_cast<void>(establish_with(client, channels, pipeline, caps));
    return pipeline.codec();
}

}  // namespace

TEST_CASE("GFX: AVC444 frames carry both views, v2 where the width allows it")
{
    struct Case {
        std::uint16_t width;
        std::uint16_t codec_id;
    };
    // 160 is a multiple of 32: AVC444v2. 150 is not: v1 (see codec/yuv444.hpp).
    for (const Case c : {Case{160, gfx::codec::avc444v2}, Case{150, gfx::codec::avc444}}) {
        CAPTURE(c.width);
        Client client;
        DynamicChannels channels(client.sink());
        start_channels(client, channels);
        GraphicsPipeline pipeline(channels, c.width, height, {}, TileCodec::avc444, stub_factory);
        const auto id =
            establish_with(client, channels, pipeline, gfx::make_capability_set(gfx::cap_version::v10_7, 0));
        CHECK(pipeline.codec() == TileCodec::avc444);

        farland::server::TestPattern pattern(c.width, height);
        const auto frame = pattern.render(0);
        REQUIRE(pipeline.send_frame(frame).has_value());
        auto pdus = client.take_gfx(id);
        REQUIRE(pdus.size() == 3);
        const auto& w = std::get<gfx::WireToSurface1>(pdus[1]);
        CHECK(w.codec_id == c.codec_id);
        const auto stream = farland::codec::avc::decode_avc444(w.bitmap_data).value();
        // The first frame: an IDR main view of the whole surface.
        REQUIRE(stream.first.regions.size() == 1);
        const auto& r = stream.first.regions[0].rect;
        CHECK((r.left == 0 && r.top == 0 && r.right == c.width && r.bottom == height));
        CHECK((w.dest_rect.left == 0 && w.dest_rect.right == c.width && w.dest_rect.bottom == height));
        if (stream.layout == farland::codec::avc::Avc444Layout::luma_and_chroma) {
            for (const auto& region : stream.second->regions) {
                CHECK((region.rect.right <= c.width && region.rect.bottom <= height && region.rect.top % 16 == 0));
            }
        } else {
            CHECK(stream.layout == farland::codec::avc::Avc444Layout::luma);
        }
        // Unchanged: nothing more to send.
        CHECK_FALSE(pipeline.send_frame(frame).has_value());
        CHECK_FALSE(pipeline.has_pending_refinement());
    }
}

TEST_CASE("GFX: AVC444 negotiation and fallbacks")
{
    using gfx::make_capability_set;
    namespace flag = gfx::caps_flag;
    namespace version = gfx::cap_version;
    // 10.0 and later: AVC444 unless the client sets AVC_DISABLED.
    CHECK(codec_for(make_capability_set(version::v10, 0)) == TileCodec::avc444);
    CHECK(codec_for(make_capability_set(version::v10_7, flag::avc_disabled)) == TileCodec::progressive);
    // AVC_THINCLIENT only says the client prefers AVC444.
    CHECK(codec_for(make_capability_set(version::v10_5, flag::avc_thin_client)) == TileCodec::avc444);
    // 8.1 has no AVC444: AVC420 with AVC420_ENABLED, else Progressive.
    CHECK(codec_for(make_capability_set(version::v8_1, flag::avc420_enabled)) == TileCodec::avc420);
    CHECK(codec_for(make_capability_set(version::v8_1, 0)) == TileCodec::progressive);
    // 8.0 thin clients take neither H.264 nor Progressive.
    CHECK(codec_for(make_capability_set(version::v8, flag::thin_client)) == TileCodec::planar);
    // No encoder: Progressive.
    const farland::server::H264Factory none = [](const farland::video::EncoderConfig&) {
        return farland::Result<std::unique_ptr<farland::video::H264Encoder>>(
            farland::fail(farland::Errc::unsupported, "no encoder"));
    };
    CHECK(codec_for(make_capability_set(version::v10_7, 0), width, none) == TileCodec::progressive);
}

TEST_CASE("GFX: a scrolled picture moves on the client and only the uncovered rows are encoded")
{
    Client client;
    DynamicChannels channels(client.sink());
    start_channels(client, channels);
    gfx::GfxServerConfig config;
    config.avc420 = config.avc444 = config.avc444v2 = false;
    GraphicsPipeline pipeline(channels, width, height, config, TileCodec::planar);
    const auto id = establish(client, channels, pipeline);

    // A document whose rows all differ, showing its lines from `first` on.
    const auto document = [](std::uint32_t first) {
        Bytes pixels(std::size_t{width} * height * 4);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                std::uint32_t v = ((first + y) * 0x9E3779B1U) ^ (x * 0x85EBCA6BU);
                v ^= v >> 15U;
                for (std::size_t c = 0; c < 4; ++c) {
                    pixels[((std::size_t{y} * width + x) * 4) + c] = static_cast<std::byte>(v >> (8 * c));
                }
            }
        }
        return pixels;
    };
    const auto view = [](const Bytes& pixels) {
        return farland::codec::ImageView{pixels, width, height, std::size_t{width} * 4};
    };

    const Bytes before = document(0);
    REQUIRE(pipeline.send_frame(view(before)).has_value());
    for (const auto& pdu : client.take_gfx(id)) {
        if (const auto* w = std::get_if<gfx::WireToSurface1>(&pdu)) {
            client.paint(*w);
        }
    }
    REQUIRE(same_picture(client.surface, view(before)));

    // Scrolled up by 12 rows: one move, then only the bottom row of tiles.
    const Bytes after = document(12);
    REQUIRE(pipeline.send_frame(view(after)).has_value());
    std::size_t moves = 0;
    std::size_t tiles = 0;
    for (const auto& pdu : client.take_gfx(id)) {
        if (const auto* s = std::get_if<gfx::SurfaceToSurface>(&pdu)) {
            ++moves;
            client.copy(*s);
        } else if (const auto* w = std::get_if<gfx::WireToSurface1>(&pdu)) {
            ++tiles;
            CHECK(w->dest_rect.top == 64);
            client.paint(*w);
        }
    }
    CHECK(moves == 1);
    CHECK(tiles == 3);
    CHECK(same_picture(client.surface, view(after)));
}

namespace {

using farland::server::PixelRect;

/// An AVC420 pipeline whose stub encoder takes dmabufs, established over 10.7.
struct DmabufPipeline {
    DmabufStub stub;
    Client client;
    DynamicChannels channels{client.sink()};
    std::optional<GraphicsPipeline> pipeline;
    std::uint32_t id = 0;

    DmabufPipeline()
    {
        start_channels(client, channels);
        gfx::GfxServerConfig config;
        config.avc444 = config.avc444v2 = false;
        pipeline.emplace(channels, width, height, config, TileCodec::avc420,
                         [this](const farland::video::EncoderConfig& encoder_config) {
                             return farland::Result<std::unique_ptr<farland::video::H264Encoder>>(
                                 std::make_unique<StubEncoder>(encoder_config, &stub));
                         });
        id = establish(client, channels, *pipeline);
    }

    /// The region rectangles of the one AVC420 frame sent since the last call.
    std::vector<gfx::Rect16> regions()
    {
        const auto pdus = client.take_gfx(id);
        REQUIRE(pdus.size() == 3);
        const auto& w = std::get<gfx::WireToSurface1>(pdus[1]);
        CHECK(w.codec_id == gfx::codec::avc420);
        std::vector<gfx::Rect16> rects;
        // Named: GCC before 15 lacks C++23's lifetime extension in range-for.
        const auto stream = farland::codec::avc::decode_avc420(w.bitmap_data).value();
        for (const auto& region : stream.regions) {
            rects.push_back({region.rect.left, region.rect.top, region.rect.right, region.rect.bottom});
        }
        return rects;
    }
};

farland::video::DmabufFrame dmabuf_frame()
{
    farland::video::DmabufFrame frame;
    frame.width = width;
    frame.height = height;
    frame.plane_count = 1;
    frame.planes[0] = {3, 0, width * 4U};
    return frame;
}

bool same_rects(std::vector<gfx::Rect16> a, std::vector<gfx::Rect16> b)
{
    const auto key = [](const gfx::Rect16& r) { return std::tuple(r.top, r.left, r.bottom, r.right); };
    std::ranges::sort(a, {}, key);
    std::ranges::sort(b, {}, key);
    return std::ranges::equal(a, b, [&](const auto& x, const auto& y) { return key(x) == key(y); });
}

}  // namespace

TEST_CASE("GFX: AVC420 from dmabufs lists the capture's damage as regions")
{
    DmabufPipeline p;
    auto& pipeline = *p.pipeline;
    REQUIRE(pipeline.accepts_dmabuf());
    const auto frame = dmabuf_frame();

    // The first picture is an IDR: the whole surface, whatever the damage.
    const std::array small{PixelRect{70, 10, 5, 5}};
    auto sent = pipeline.send_dmabuf_frame(frame, small);
    REQUIRE(sent.has_value());
    REQUIRE(sent->has_value());
    CHECK(same_rects(p.regions(), {{0, 0, width, height}}));

    // Later pictures list the tiles the damage touches.
    sent = pipeline.send_dmabuf_frame(frame, small);
    REQUIRE((sent.has_value() && sent->has_value()));
    CHECK(same_rects(p.regions(), {{64, 0, 128, 64}}));
    const std::array corner{PixelRect{60, 60, 10, 10}, PixelRect{149, 99, 50, 50}};
    sent = pipeline.send_dmabuf_frame(frame, corner);
    REQUIRE((sent.has_value() && sent->has_value()));
    CHECK(same_rects(p.regions(),
                     {{0, 0, 64, 64}, {64, 0, 128, 64}, {0, 64, 64, 100}, {64, 64, 128, 100}, {128, 64, 150, 100}}));

    // No damage: nothing to send.
    sent = pipeline.send_dmabuf_frame(frame, {});
    REQUIRE(sent.has_value());
    CHECK_FALSE(sent->has_value());
    CHECK(p.client.take_gfx(p.id).empty());

    // An invalidation (the client asked for a refresh) lists every tile.
    pipeline.invalidate_all();
    sent = pipeline.send_dmabuf_frame(frame, {});
    REQUIRE((sent.has_value() && sent->has_value()));
    CHECK(p.regions().size() == 6);

    CHECK(p.stub.dmabuf_pictures == 4);
    CHECK(p.stub.cpu_pictures == 0);  // not a pixel read
    pipeline.forget_dmabufs();
    CHECK(p.stub.forgotten == 1);
}

TEST_CASE("GFX: a dmabuf the encoder refuses goes out from CPU pixels, which then cover everything")
{
    DmabufPipeline p;
    auto& pipeline = *p.pipeline;
    const auto frame = dmabuf_frame();
    farland::server::TestPattern pattern(width, height);
    const auto image = pattern.render(0);

    // Refused: nothing goes out, and the caller sends the pixels instead.
    p.stub.refuse = true;
    const std::array all{PixelRect{0, 0, width, height}};
    const auto refused = pipeline.send_dmabuf_frame(frame, all);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == farland::Errc::unsupported);
    CHECK(p.client.take_gfx(p.id).empty());
    REQUIRE(pipeline.send_frame(image).has_value());
    CHECK(same_rects(p.regions(), {{0, 0, width, height}}));  // the IDR

    // Back to dmabufs for a frame...
    p.stub.refuse = false;
    const std::array small{PixelRect{0, 0, 1, 1}};
    REQUIRE(pipeline.send_dmabuf_frame(frame, small).value().has_value());
    CHECK(same_rects(p.regions(), {{0, 0, 64, 64}}));

    // ...so the pipeline's copy of the picture is stale: the next CPU frame
    // sends every tile, even though its pixels equal the last CPU frame.
    REQUIRE(pipeline.send_frame(image).has_value());
    CHECK(p.regions().size() == 6);
    CHECK_FALSE(pipeline.send_frame(image).has_value());  // and then diffs again
    CHECK(p.stub.cpu_pictures == 2);
    CHECK(p.stub.dmabuf_pictures == 1);
}

TEST_CASE("GFX: only AVC420 surfaces with an encoder that takes dmabufs accept them")
{
    DmabufStub stub;
    stub.accepts = false;
    const farland::server::H264Factory factory = [&stub](const farland::video::EncoderConfig& config) {
        return farland::Result<std::unique_ptr<farland::video::H264Encoder>>(
            std::make_unique<StubEncoder>(config, &stub));
    };
    for (const TileCodec codec : {TileCodec::avc420, TileCodec::avc444, TileCodec::progressive}) {
        for (const bool accepts : {false, true}) {
            CAPTURE(static_cast<int>(codec), accepts);
            stub.accepts = accepts;
            Client client;
            DynamicChannels channels(client.sink());
            start_channels(client, channels);
            GraphicsPipeline pipeline(channels, width, height, {}, codec, factory);
            static_cast<void>(
                establish_with(client, channels, pipeline, gfx::make_capability_set(gfx::cap_version::v10_7, 0)));
            CHECK(pipeline.accepts_dmabuf() == (accepts && codec == TileCodec::avc420));
        }
    }
}

TEST_CASE("GFX: a picture keeps its hue through the coarse-first ladder")
{
    // A first paint on a slow link arrives coarse and is refined while it
    // stands still. Every stage of that has to look like the picture, not
    // like its opposite: a red and blue that trade places turn an orange
    // logo blue, and a viewer sees it long before any measurement does.
    Client client;
    DynamicChannels channels(client.sink());
    start_channels(client, channels);
    gfx::GfxServerConfig config;
    config.avc420 = config.avc444 = config.avc444v2 = false;
    // direct_tiles = 0 forces the ladder rather than the one-shot pass that
    // small damage takes; both are covered, this one is the slow-link path.
    GraphicsPipeline pipeline(channels, width, height, config, TileCodec::progressive, {},
                              farland::server::PipelineOptions{.direct_tiles = 0});
    const auto id = establish(client, channels, pipeline);

    // Shaded orange: a colour per pixel, so this is a picture to the
    // classifier and goes through Progressive. Flat orange would go through
    // ClearCodec, which is exact and could never show the problem.
    Bytes pixels(std::size_t{width} * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto at = ((std::size_t{y} * width) + x) * 4;
            pixels[at] = static_cast<std::byte>((x + y) % 48);              // B: little
            pixels[at + 1] = static_cast<std::byte>(120 + ((y * 7) % 70));  // G: middling
            pixels[at + 2] = static_cast<std::byte>(200 + ((x * 3) % 56));  // R: most
            pixels[at + 3] = std::byte{0xFF};
        }
    }
    const farland::codec::ImageView frame{pixels, width, height, std::size_t{width} * 4};
    auto decoder = farland::codec::progressive::Decoder::create(width, height).value();

    const auto pixel = [](const farland::codec::ImageView& image, std::uint32_t x, std::uint32_t y) {
        const std::size_t at = (std::size_t{y} * image.stride) + (std::size_t{x} * 4);
        return std::array<int, 3>{std::to_integer<int>(image.data[at + 2]), std::to_integer<int>(image.data[at + 1]),
                                  std::to_integer<int>(image.data[at])};
    };
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 3> probes{
        {{width / 4, height / 4}, {width / 2, height / 2}, {(width * 3) / 4, (height * 3) / 4}}};

    bool refined = false;
    for (int f = 0; f < 8 && !refined; ++f) {
        const auto frame_id = pipeline.send_frame(frame);
        if (!frame_id) {
            continue;
        }
        bool decoded_any = false;
        for (const auto& pdu : client.take_gfx(id)) {
            if (const auto* w2 = std::get_if<gfx::WireToSurface2>(&pdu)) {
                REQUIRE(w2->codec_id == gfx::codec::progressive);
                REQUIRE(decoder.decode(w2->bitmap_data, *frame_id).has_value());
                decoded_any = true;
            }
        }
        if (!decoded_any) {
            continue;
        }
        for (const auto& [x, y] : probes) {
            const auto want = pixel(frame, x, y);
            const auto got = pixel(decoder.image(), x, y);
            INFO("frame " << f << " at " << x << "," << y << ": wanted r=" << want[0] << " g=" << want[1]
                          << " b=" << want[2] << ", got r=" << got[0] << " g=" << got[1] << " b=" << got[2]);
            // The hue survives: red stays the strongest channel and blue the
            // weakest, at every stage and not only at the end.
            CHECK(got[0] > got[1]);
            CHECK(got[1] > got[2]);
            // And the coarse stage is already close, not merely the right
            // way round: a channel swap would be far outside this.
            CHECK(std::abs(got[0] - want[0]) <= 40);
            CHECK(std::abs(got[1] - want[1]) <= 40);
            CHECK(std::abs(got[2] - want[2]) <= 40);
        }
        // Once a stage lands within a couple of steps the ladder has done
        // its work and there is nothing further to check.
        const auto want = pixel(frame, width / 2, height / 2);
        const auto got = pixel(decoder.image(), width / 2, height / 2);
        refined = std::abs(got[0] - want[0]) <= 4 && std::abs(got[2] - want[2]) <= 10;
    }
    CHECK(refined);
}

TEST_CASE("GFX: text through ClearCodec, pictures through Progressive, refined while the picture stands still")
{
    Client client;
    DynamicChannels channels(client.sink());
    start_channels(client, channels);
    gfx::GfxServerConfig config;
    config.avc420 = config.avc444 = config.avc444v2 = false;
    // Progressive with ClearCodec and refinement. direct_tiles is 0 so that
    // this small surface takes the coarse-first ladder, which is what this
    // test is about; the direct pass has a test of its own.
    GraphicsPipeline pipeline(channels, width, height, config, TileCodec::progressive, {},
                              farland::server::PipelineOptions{.direct_tiles = 0});
    const auto id = establish(client, channels, pipeline);

    // The first column of tiles shows black strokes on white, like text; the
    // rest a smooth gradient with a colour per pixel, like a photo.
    Bytes pixels(std::size_t{width} * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto at = ((std::size_t{y} * width) + x) * 4;
            if (x < GraphicsPipeline::tile_size) {
                const auto ink = static_cast<std::byte>(((x / 3) + (y / 5)) % 4 == 0 ? 0x00 : 0xFF);
                pixels[at] = ink;
                pixels[at + 1] = ink;
                pixels[at + 2] = ink;
            } else {
                pixels[at] = static_cast<std::byte>(x * 2);
                pixels[at + 1] = static_cast<std::byte>(y * 2);
                pixels[at + 2] = static_cast<std::byte>(x + y);
            }
            pixels[at + 3] = std::byte{0xFF};
        }
    }
    const farland::codec::ImageView frame{pixels, width, height, std::size_t{width} * 4};

    // The client: Progressive decodes onto its own surface, and ClearCodec
    // regions are painted over it.
    auto progressive_decoder = farland::codec::progressive::Decoder::create(width, height).value();
    farland::codec::clear::Decoder clear_decoder;
    Bytes clear_pixels(pixels.size());
    std::vector<gfx::Rect16> clear_rects;
    std::size_t clear_regions = 0;
    std::size_t planar_tiles = 0;
    const auto receive = [&](std::uint32_t frame_id) {
        for (const auto& pdu : client.take_gfx(id)) {
            if (const auto* w1 = std::get_if<gfx::WireToSurface1>(&pdu)) {
                const auto r = w1->dest_rect;
                const auto out = std::span(clear_pixels).subspan(((std::size_t{r.top} * width) + r.left) * 4);
                if (w1->codec_id == gfx::codec::planar) {
                    // The lossless pass over a tile that stands still.
                    Bytes decoded(std::size_t{r.width()} * r.height() * 4);
                    REQUIRE(farland::codec::planar::decode(w1->bitmap_data, r.width(), r.height(),
                                                           farland::codec::planar::Orientation::top_down, decoded)
                                .has_value());
                    for (std::uint32_t row = 0; row < r.height(); ++row) {
                        std::copy_n(decoded.begin() + static_cast<std::ptrdiff_t>(std::size_t{row} * r.width() * 4),
                                    std::size_t{r.width()} * 4,
                                    out.begin() + static_cast<std::ptrdiff_t>(std::size_t{row} * width * 4));
                    }
                    ++planar_tiles;
                } else {
                    REQUIRE(w1->codec_id == gfx::codec::clearcodec);
                    REQUIRE(clear_decoder.decode(w1->bitmap_data, r.width(), r.height(), out, std::size_t{width} * 4)
                                .has_value());
                    ++clear_regions;
                }
                clear_rects.push_back(r);
            } else if (const auto* w2 = std::get_if<gfx::WireToSurface2>(&pdu)) {
                REQUIRE(w2->codec_id == gfx::codec::progressive);
                REQUIRE(progressive_decoder.decode(w2->bitmap_data, frame_id).has_value());
            }
        }
    };
    const auto composite = [&] {
        const auto image = progressive_decoder.image();
        Bytes out(image.data.begin(), image.data.end());
        for (const auto& r : clear_rects) {
            for (std::uint32_t y = r.top; y < r.bottom; ++y) {
                const auto at = static_cast<std::ptrdiff_t>(((std::size_t{y} * width) + r.left) * 4);
                std::copy_n(clear_pixels.begin() + at, std::size_t{r.width()} * 4, out.begin() + at);
            }
        }
        return out;
    };
    const auto view = [](const Bytes& image) {
        return farland::codec::ImageView{image, width, height, std::size_t{width} * 4};
    };

    const auto first = pipeline.send_frame(frame);
    REQUIRE(first.has_value());
    receive(*first);
    CHECK(clear_regions == 2);  // the text column, in both rows of tiles
    CHECK(pipeline.has_pending_refinement());
    const Bytes coarse = composite();

    // Nothing changes: frames of refinement only, until every tile is at full
    // quality and the lossless pass has made the still picture exact.
    int refinements = 0;
    while (pipeline.has_pending_refinement() && refinements < 50) {
        const auto frame_id = pipeline.send_frame(frame);
        REQUIRE(frame_id.has_value());
        clear_regions = 0;
        receive(*frame_id);
        CHECK(clear_regions == 0);  // the text tiles were exact from the start
        ++refinements;
    }
    CHECK_FALSE(pipeline.has_pending_refinement());
    CHECK(refinements > 0);
    CHECK(planar_tiles == 4);                             // the four Progressive tiles, not the two text ones
    CHECK_FALSE(pipeline.send_frame(frame).has_value());  // and then nothing

    const Bytes fine = composite();
    CHECK(psnr(view(fine), frame) > psnr(view(coarse), frame));
    // A picture that stands still ends up exact: ClearCodec for the text,
    // the lossless pass for what Progressive left quantized.
    CHECK(fine == pixels);
}

TEST_CASE("GFX: frames without a picture refine what a desktop standing still left coarse")
{
    Client client;
    DynamicChannels channels(client.sink());
    start_channels(client, channels);
    gfx::GfxServerConfig config;
    config.avc420 = config.avc444 = config.avc444v2 = false;
    GraphicsPipeline pipeline(channels, width, height, config, TileCodec::progressive, {},
                              farland::server::PipelineOptions{.direct_tiles = 0});
    const auto id = establish(client, channels, pipeline);

    // A gradient with a colour per pixel, like a photo: every tile goes out
    // as Progressive, none as ClearCodec.
    Bytes pixels(std::size_t{width} * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto at = ((std::size_t{y} * width) + x) * 4;
            pixels[at] = static_cast<std::byte>(x * 2);
            pixels[at + 1] = static_cast<std::byte>(y * 2);
            pixels[at + 2] = static_cast<std::byte>(x + y);
            pixels[at + 3] = std::byte{0xFF};
        }
    }
    const farland::codec::ImageView frame{pixels, width, height, std::size_t{width} * 4};

    auto decoder = farland::codec::progressive::Decoder::create(width, height).value();
    Bytes overlay(std::size_t{width} * height * 4);  // what the lossless pass painted over it
    std::vector<gfx::Rect16> overlay_rects;
    const auto receive = [&](std::uint32_t frame_id) {
        std::size_t streams = 0;
        for (const auto& pdu : client.take_gfx(id)) {
            if (const auto* w2 = std::get_if<gfx::WireToSurface2>(&pdu)) {
                REQUIRE(decoder.decode(w2->bitmap_data, frame_id).has_value());
                ++streams;
            } else if (const auto* w1 = std::get_if<gfx::WireToSurface1>(&pdu)) {
                REQUIRE(w1->codec_id == gfx::codec::planar);
                const auto r = w1->dest_rect;
                Bytes decoded(std::size_t{r.width()} * r.height() * 4);
                REQUIRE(farland::codec::planar::decode(w1->bitmap_data, r.width(), r.height(),
                                                       farland::codec::planar::Orientation::top_down, decoded)
                            .has_value());
                for (std::uint32_t row = 0; row < r.height(); ++row) {
                    std::copy_n(decoded.begin() + static_cast<std::ptrdiff_t>(std::size_t{row} * r.width() * 4),
                                std::size_t{r.width()} * 4,
                                overlay.begin() +
                                    static_cast<std::ptrdiff_t>(((std::size_t{r.top + row} * width) + r.left) * 4));
                }
                overlay_rects.push_back(r);
                ++streams;
            }
        }
        return streams;
    };

    const auto first = pipeline.send_frame(frame);
    REQUIRE(first.has_value());
    CHECK(receive(*first) > 0);
    REQUIRE(pipeline.has_pending_refinement());
    const Bytes coarse(decoder.image().data.begin(), decoder.image().data.end());

    // The desktop stands still, so the capture has no frame to give and the
    // session opens frames with no picture in them at all: the refinement
    // has to go out in those.
    int refinements = 0;
    int carried = 0;
    while (pipeline.has_pending_refinement() && refinements < 50) {
        pipeline.begin_frame();
        // A tick can carry nothing: a tile that just reached full quality has
        // to stand still for still_frames before its lossless copy goes out,
        // and the counters only run while frames are asked for.
        if (const auto frame_id = pipeline.end_frame()) {
            carried += receive(*frame_id) > 0 ? 1 : 0;
        }
        ++refinements;
    }
    CHECK(carried > 0);
    CHECK_FALSE(pipeline.has_pending_refinement());
    CHECK(refinements > 0);
    pipeline.begin_frame();
    CHECK_FALSE(pipeline.end_frame().has_value());  // and then nothing

    Bytes fine(decoder.image().data.begin(), decoder.image().data.end());
    const auto view = [](const Bytes& image) {
        return farland::codec::ImageView{image, width, height, std::size_t{width} * 4};
    };
    CHECK(psnr(view(fine), frame) > psnr(view(coarse), frame));
    // And the lossless pass then made the whole still picture exact.
    CHECK(overlay_rects.size() == 6);
    for (const auto& r : overlay_rects) {
        for (std::uint32_t y = r.top; y < r.bottom; ++y) {
            const auto at = static_cast<std::ptrdiff_t>(((std::size_t{y} * width) + r.left) * 4);
            std::copy_n(overlay.begin() + at, std::size_t{r.width()} * 4, fine.begin() + at);
        }
    }
    CHECK(fine == pixels);
}

TEST_CASE("GFX: deferred AVC444 chroma goes out while the desktop stands still")
{
    Client client;
    DynamicChannels channels(client.sink());
    start_channels(client, channels);
    GraphicsPipeline pipeline(channels, width, height, {}, TileCodec::avc444, stub_factory);
    const auto id = establish_with(client, channels, pipeline, gfx::make_capability_set(gfx::cap_version::v10_7, 0));
    REQUIRE(pipeline.codec() == TileCodec::avc444);
    // The congested tier: chroma waits until the picture stops changing.
    pipeline.set_quality({}, {}, true);

    farland::server::TestPattern pattern(width, height);
    const auto frame = pattern.render(0);
    const auto first = pipeline.send_frame(frame);
    REQUIRE(first.has_value());
    const auto layout_of = [&](std::uint32_t frame_id) {
        static_cast<void>(frame_id);
        std::optional<farland::codec::avc::Avc444Layout> layout;
        for (const auto& pdu : client.take_gfx(id)) {
            if (const auto* w = std::get_if<gfx::WireToSurface1>(&pdu)) {
                layout = farland::codec::avc::decode_avc444(w->bitmap_data).value().layout;
            }
        }
        return layout;
    };
    REQUIRE(layout_of(*first) == farland::codec::avc::Avc444Layout::luma);
    REQUIRE(pipeline.has_pending_refinement());  // the chroma of a 4:2:0 picture

    // The desktop stands still from here: the capture has nothing to give and
    // the session opens frames with no picture in them. The chroma still owed
    // has to go out in one of those, or the client keeps a 4:2:0 desktop.
    pipeline.begin_frame();
    const auto second = pipeline.end_frame();
    REQUIRE(second.has_value());
    CHECK(layout_of(*second) == farland::codec::avc::Avc444Layout::chroma);
    CHECK_FALSE(pipeline.has_pending_refinement());
    pipeline.begin_frame();
    CHECK_FALSE(pipeline.end_frame().has_value());  // and then nothing
}

TEST_CASE("GFX: a surface per screen, black surfaces around letterboxed pictures, and a new layout")
{
    Client client;
    DynamicChannels channels(client.sink());
    start_channels(client, channels);
    // Monitors 400x200 (primary) and 200x200 to its right. Screen 0 is
    // 300x200: centred with borders. Screen 1 is 400x400: scaled to 200x200.
    const auto monitors =
        farland::server::DisplayLayout::from_disp(
            {{{.flags = 1, .width = 400, .height = 200}, {.left = 400, .width = 200, .height = 200}}}, {})
            .value();
    const std::array sizes{std::pair<std::uint32_t, std::uint32_t>{300, 200},
                           std::pair<std::uint32_t, std::uint32_t>{400, 400}};
    const auto layout = monitors.place(sizes);
    gfx::GfxServerConfig config;
    config.avc420 = config.avc444 = config.avc444v2 = false;
    const bool client_scales = GENERATE(true, false);
    CAPTURE(client_scales);
    GraphicsPipeline pipeline(channels, layout, config, TileCodec::planar, {},
                              farland::server::PipelineOptions{.scaled_output = client_scales});
    const auto id = std::get<dyn::CreateRequest>(client.take().at(0)).channel_id;
    send(channels, dyn::CreateResponse{id, 0});
    dispatch(channels, pipeline);
    send(channels,
         dyn::Data{id,
                   gfx::encode(gfx::Pdu{gfx::CapsAdvertise{{gfx::make_capability_set(gfx::cap_version::v10_7, 0)}}})});
    dispatch(channels, pipeline);
    REQUIRE(pipeline.ready());
    REQUIRE(pipeline.screen_count() == 2);
    CHECK(pipeline.surface_size(0) == std::pair<std::uint32_t, std::uint32_t>{300, 200});
    CHECK(pipeline.surface_size(1) == (client_scales ? std::pair<std::uint32_t, std::uint32_t>{400, 400}
                                                     : std::pair<std::uint32_t, std::uint32_t>{200, 200}));

    auto pdus = client.take_gfx(id);
    REQUIRE(pdus.size() == 14);
    const auto& reset = std::get<gfx::ResetGraphics>(pdus[1]);
    CHECK((reset.width == 600 && reset.height == 200));
    CHECK(reset.monitors == monitors.gfx_monitors());
    const auto s0 = std::get<gfx::CreateSurface>(pdus[2]);
    CHECK((s0.width == 300 && s0.height == 200));
    CHECK(std::get<gfx::MapSurfaceToOutput>(pdus[3]) == gfx::MapSurfaceToOutput{s0.surface_id, 50, 0});
    const auto s1 = std::get<gfx::CreateSurface>(pdus[4]);
    if (client_scales) {
        CHECK((s1.width == 400 && s1.height == 400));
        CHECK(std::get<gfx::MapSurfaceToScaledOutput>(pdus[5]) ==
              gfx::MapSurfaceToScaledOutput{s1.surface_id, 400, 0, 200, 200});
    } else {
        CHECK((s1.width == 200 && s1.height == 200));
        CHECK(std::get<gfx::MapSurfaceToOutput>(pdus[5]) == gfx::MapSurfaceToOutput{s1.surface_id, 400, 0});
    }
    // The borders left and right of screen 0, filled black in a frame.
    const auto left = std::get<gfx::CreateSurface>(pdus[6]);
    CHECK((left.width == 50 && left.height == 200));
    CHECK(std::get<gfx::MapSurfaceToOutput>(pdus[7]) == gfx::MapSurfaceToOutput{left.surface_id, 0, 0});
    const auto right = std::get<gfx::CreateSurface>(pdus[8]);
    CHECK(std::get<gfx::MapSurfaceToOutput>(pdus[9]) == gfx::MapSurfaceToOutput{right.surface_id, 350, 0});
    CHECK(std::holds_alternative<gfx::StartFrame>(pdus[10]));
    const auto& fill = std::get<gfx::SolidFill>(pdus[11]);
    CHECK(fill.surface_id == left.surface_id);
    CHECK(fill.fill_pixel == gfx::Color32{0, 0, 0, 0xFF});
    CHECK(std::get<gfx::SolidFill>(pdus[12]).surface_id == right.surface_id);
    CHECK(std::holds_alternative<gfx::EndFrame>(pdus[13]));

    // One frame carries both screens.
    farland::server::TestPattern first(300, 200);
    const auto [w1, h1] = pipeline.surface_size(1);
    farland::server::TestPattern second(w1, h1);
    pipeline.begin_frame();
    pipeline.add_frame(0, first.render(0));
    pipeline.add_frame(1, second.render(0));
    const auto frame_id = pipeline.end_frame();
    REQUIRE(frame_id.has_value());
    pdus = client.take_gfx(id);
    CHECK(std::get<gfx::StartFrame>(pdus.front()).frame_id == *frame_id);
    CHECK(std::get<gfx::EndFrame>(pdus.back()).frame_id == *frame_id);
    std::size_t on_first = 0;
    std::size_t on_second = 0;
    for (std::size_t i = 1; i + 1 < pdus.size(); ++i) {
        const auto& wire = std::get<gfx::WireToSurface1>(pdus[i]);
        on_first += wire.surface_id == s0.surface_id ? 1 : 0;
        on_second += wire.surface_id == s1.surface_id ? 1 : 0;
    }
    CHECK(on_first == 5 * 4);  // 300x200 in 64-pixel tiles
    CHECK(on_second == ((w1 + 63) / 64) * ((h1 + 63) / 64));
    // Nothing changed: no frame at all.
    pipeline.begin_frame();
    pipeline.add_frame(0, first.render(0));
    CHECK_FALSE(pipeline.end_frame().has_value());

    // A new layout: the old surfaces go, and the new one covers everything.
    const std::array one{std::pair<std::uint32_t, std::uint32_t>{640, 480}};
    pipeline.set_layout(farland::server::DisplayLayout::single(640, 480).place(one));
    pdus = client.take_gfx(id);
    REQUIRE(pdus.size() == 4 + 3);
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(std::holds_alternative<gfx::DeleteSurface>(pdus[i]));
    }
    CHECK(std::get<gfx::ResetGraphics>(pdus[4]).width == 640);
    CHECK(std::get<gfx::CreateSurface>(pdus[5]).width == 640);
    CHECK(std::get<gfx::MapSurfaceToOutput>(pdus[6]).output_origin_x == 0);
    REQUIRE(pipeline.screen_count() == 1);
    farland::server::TestPattern big(640, 480);
    CHECK(pipeline.send_frame(big.render(0)).has_value());
}

namespace {

// A surface with more tiles than PipelineOptions::direct_tiles, so that a
// whole repaint takes the coarse-first ladder and a small change does not.
constexpr std::uint16_t big_width = 320;
constexpr std::uint16_t big_height = 256;
constexpr std::uint32_t big_tiles_x = 5;

/// A gradient with mild noise: far too many colours for ClearCodec, so every
/// tile is a job for Progressive or H.264.
Bytes noisy_gradient(std::uint32_t seed)
{
    Bytes pixels(std::size_t{big_width} * big_height * 4);
    std::uint32_t state = seed | 1U;
    for (std::uint32_t y = 0; y < big_height; ++y) {
        for (std::uint32_t x = 0; x < big_width; ++x) {
            state = (state * 1664525U) + 1013904223U;
            const std::uint32_t n = (state >> 24U) % 7U;
            const auto at = ((std::size_t{y} * big_width) + x) * 4;
            pixels[at] = static_cast<std::byte>((x + n) & 0xFFU);
            pixels[at + 1] = static_cast<std::byte>(((y * 2U) + n) & 0xFFU);
            pixels[at + 2] = static_cast<std::byte>((x + y + n) & 0xFFU);
            pixels[at + 3] = std::byte{0xFF};
        }
    }
    return pixels;
}

farland::codec::ImageView big_view(const Bytes& pixels)
{
    return {pixels, big_width, big_height, std::size_t{big_width} * 4};
}

/// Repaints the tiles in [tx0, tx1) x [ty0, ty1) with the same kind of
/// picture the surface already holds, shifted by `step`: what a window that
/// scrolls or animates puts there, not white noise, so that quality figures
/// before and after are about the codec and not about the content.
void repaint_tiles(Bytes& pixels, std::uint32_t tx0, std::uint32_t ty0, std::uint32_t tx1, std::uint32_t ty1,
                   std::uint32_t step)
{
    std::uint32_t state = (step * 2654435761U) | 1U;
    for (std::uint32_t y = ty0 * 64; y < std::min<std::uint32_t>(ty1 * 64, big_height); ++y) {
        for (std::uint32_t x = tx0 * 64; x < std::min<std::uint32_t>(tx1 * 64, big_width); ++x) {
            state = (state * 1664525U) + 1013904223U;
            const std::uint32_t n = (state >> 24U) % 7U;
            // Thin strokes on a light ground, as text and a caret are: the
            // detail a coarse first pass smears and a full-quality pass keeps.
            // The ground shifts per pixel, so the tile has far too many
            // colours for ClearCodec.
            const bool ink = (((x + step) / 2U) + (y / 3U)) % 4U == 0;
            const std::uint32_t base = ink ? 20U : 225U;
            const auto at = ((std::size_t{y} * big_width) + x) * 4;
            pixels[at] = static_cast<std::byte>((base + n) & 0xFFU);
            pixels[at + 1] = static_cast<std::byte>((base + n + (y % 5U)) & 0xFFU);
            pixels[at + 2] = static_cast<std::byte>((base + n + (x % 5U)) & 0xFFU);
        }
    }
}

/// PSNR over one rectangle of two big-surface pictures.
double psnr_rect(const farland::codec::ImageView& a, const farland::codec::ImageView& b, std::uint32_t x0,
                 std::uint32_t y0, std::uint32_t w, std::uint32_t h)
{
    double sum = 0;
    for (std::uint32_t y = y0; y < y0 + h; ++y) {
        for (std::uint32_t x = x0; x < x0 + w; ++x) {
            for (std::size_t c = 0; c < 3; ++c) {
                const double d = std::to_integer<int>(a.data[(y * a.stride) + (x * 4) + c]) -
                                 std::to_integer<int>(b.data[(y * b.stride) + (x * 4) + c]);
                sum += d * d;
            }
        }
    }
    const double mse = sum / (static_cast<double>(w) * h * 3);
    return mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

}  // namespace

TEST_CASE("GFX: a small change goes out sharp at once instead of coarse and refined")
{
    // Two pipelines side by side on the same pictures, one that gives small
    // damage the direct full-quality pass and one that does not. The lossless
    // pass is off in both, so the Progressive surface the decoder holds is
    // exactly what the client shows and the two passes can be compared.
    struct Side {
        Client client;
        DynamicChannels channels{client.sink()};
        std::optional<GraphicsPipeline> pipeline;
        std::uint32_t id = 0;
        std::optional<farland::codec::progressive::Decoder> decoder;

        explicit Side(std::uint32_t direct_tiles)
        {
            start_channels(client, channels);
            gfx::GfxServerConfig config;
            config.avc420 = config.avc444 = config.avc444v2 = false;
            // ClearCodec off too: the point is which Progressive pass a tile gets.
            pipeline.emplace(channels, big_width, big_height, config, TileCodec::progressive,
                             farland::server::H264Factory{},
                             farland::server::PipelineOptions{
                                 .clearcodec = false, .direct_tiles = direct_tiles, .lossless_still = false});
            id = establish(client, channels, *pipeline);
            decoder = farland::codec::progressive::Decoder::create(big_width, big_height).value();
        }

        void send(const Bytes& pixels)
        {
            const auto frame_id = pipeline->send_frame(big_view(pixels));
            if (!frame_id) {
                return;  // nothing left to send
            }
            for (const auto& pdu : client.take_gfx(id)) {
                const auto* w2 = std::get_if<gfx::WireToSurface2>(&pdu);
                REQUIRE((w2 != nullptr || !std::holds_alternative<gfx::WireToSurface1>(pdu)));
                if (w2 != nullptr) {
                    REQUIRE(decoder->decode(w2->bitmap_data, *frame_id).has_value());
                }
            }
        }
        [[nodiscard]] double tile_psnr(const Bytes& pixels) const
        {
            return psnr_rect(big_view(pixels), decoder->image(), 0, 0, 64, 64);
        }
        void settle(const Bytes& pixels)
        {
            for (int n = 0; n < 16 && pipeline->has_pending_refinement(); ++n) {
                send(pixels);
            }
        }
    };

    Side direct(4);  // one changed tile is small damage
    Side ladder(0);  // never direct: always coarse first

    // A whole repaint: 20 tiles, more than direct_tiles, so both take the
    // coarse-first ladder. Then both refine until the picture is complete.
    Bytes pixels = noisy_gradient(3);
    direct.send(pixels);
    ladder.send(pixels);
    CHECK(direct.pipeline->has_pending_refinement());
    CHECK(ladder.pipeline->has_pending_refinement());
    direct.settle(pixels);
    ladder.settle(pixels);
    // Refined to the end, both hold the same picture.
    CHECK(direct.tile_psnr(pixels) == Catch::Approx(ladder.tile_psnr(pixels)));

    // A caret blinks: one tile changes, then stands still again. What the eye
    // sees is the frame right after the change, and that is where the two
    // differ -- the ladder starts that tile over at its coarsest stage, the
    // direct pass sends it at full quality at once.
    for (std::uint32_t blink = 1; blink <= 3; ++blink) {
        repaint_tiles(pixels, 0, 0, 1, 1, blink);
        direct.send(pixels);
        ladder.send(pixels);
        const double sharp = direct.tile_psnr(pixels);
        const double coarse = ladder.tile_psnr(pixels);
        INFO("blink " << blink << ": direct " << sharp << " dB, coarse-first " << coarse << " dB");
        // The ladder smears the strokes until its upgrades arrive; the direct
        // pass has them right away.
        CHECK(sharp > coarse + 10.0);
        CHECK(sharp >= 30.0);
        CHECK(coarse < 25.0);
        // Between blinks the ladder catches up, so the next blink starts level.
        direct.settle(pixels);
        ladder.settle(pixels);
    }
}

TEST_CASE("GFX: tiles that keep changing go through H.264 on the Progressive surface")
{
    Client client;
    DynamicChannels channels(client.sink());
    start_channels(client, channels);
    gfx::GfxServerConfig config;
    config.avc420 = true;
    config.avc444 = config.avc444v2 = false;
    GraphicsPipeline pipeline(
        channels, big_width, big_height, config, TileCodec::progressive, stub_factory,
        farland::server::PipelineOptions{
            .clearcodec = false, .direct_tiles = 0, .min_video_tiles = 6, .video_idle_frames = 3});
    const auto id = establish_with(client, channels, pipeline, gfx::make_capability_set(gfx::cap_version::v10_7, 0));
    CHECK(pipeline.codec() == TileCodec::progressive);

    struct Seen {
        std::vector<gfx::Rect16> avc;
        std::size_t progressive_streams = 0;
        std::size_t exact_tiles = 0;
    };
    const auto receive = [&] {
        Seen seen;
        for (const auto& pdu : client.take_gfx(id)) {
            if (const auto* w1 = std::get_if<gfx::WireToSurface1>(&pdu)) {
                if (w1->codec_id == gfx::codec::planar) {
                    ++seen.exact_tiles;  // the lossless pass over a still tile
                    continue;
                }
                REQUIRE(w1->codec_id == gfx::codec::avc420);
                const auto stream = farland::codec::avc::decode_avc420(w1->bitmap_data).value();
                for (const auto& region : stream.regions) {
                    seen.avc.push_back({region.rect.left, region.rect.top, region.rect.right, region.rect.bottom});
                }
            } else if (std::holds_alternative<gfx::WireToSurface2>(pdu)) {
                ++seen.progressive_streams;
            }
        }
        return seen;
    };

    Bytes pixels = noisy_gradient(5);
    auto frame_id = pipeline.send_frame(big_view(pixels));
    REQUIRE(frame_id.has_value());
    CHECK(receive().avc.empty());  // the first repaint is not motion yet

    // Six tiles repaint every frame: after a few frames they count as moving
    // picture and H.264 takes them, while the rest stays Progressive.
    std::size_t avc_frames = 0;
    std::vector<gfx::Rect16> last;
    for (std::uint32_t n = 0; n < 8; ++n) {
        repaint_tiles(pixels, 0, 0, 3, 2, 200 + n);
        frame_id = pipeline.send_frame(big_view(pixels));
        REQUIRE(frame_id.has_value());
        const auto seen = receive();
        if (!seen.avc.empty()) {
            ++avc_frames;
            last = seen.avc;
        }
    }
    CHECK(avc_frames >= 4);
    CHECK(same_rects(last, {{0, 0, 64, 64},
                            {64, 0, 128, 64},
                            {128, 0, 192, 64},
                            {0, 64, 64, 128},
                            {64, 64, 128, 128},
                            {128, 64, 192, 128}}));

    // The video stops: the tiles cool down, go back to Progressive and end up
    // exact again, and the H.264 encoder is dropped.
    std::size_t progressive_after = 0;
    std::size_t exact_after = 0;
    for (std::uint32_t n = 0; n < 40; ++n) {
        pipeline.begin_frame();
        pipeline.add_frame(0, big_view(pixels));
        if (pipeline.end_frame()) {
            const auto seen = receive();
            CHECK(seen.avc.empty());
            progressive_after += seen.progressive_streams;
            exact_after += seen.exact_tiles;
        }
    }
    CHECK(progressive_after > 0);
    CHECK(exact_after >= 6);  // at least the six tiles H.264 had been painting
}
