// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The AVC444 encoder's stream layout and LC policy, with a stand-in H.264
// encoder that keeps every picture it is given. A client model (FreeRDP's
// decoder steps: codec::apply_main_view/apply_aux_view per region, then the
// reverse filter) shows what the client ends up with. The real H.264 round
// trip is in test_h264_encoder.cpp.

#include <farland/base/error.hpp>
#include <farland/codec/avc420.hpp>
#include <farland/codec/image.hpp>
#include <farland/codec/yuv.hpp>
#include <farland/codec/yuv444.hpp>
#include <farland/video/avc444_encoder.hpp>
#include <farland/video/h264_encoder.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace codec = farland::codec;
namespace video = farland::video;
namespace avc = farland::codec::avc;
using avc::Avc444Layout;
using avc::Rect16;
using codec::Avc444Version;

namespace {

constexpr std::uint16_t width = 150;  // 3 x 2 tiles, coded as 160 x 112
constexpr std::uint16_t height = 100;

struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

struct Image {
    std::vector<std::byte> data = std::vector<std::byte>(std::size_t{width} * height * 4, std::byte{0xFF});

    void set(std::uint32_t x, std::uint32_t y, Rgb c)
    {
        const std::size_t at = ((std::size_t{y} * width) + x) * 4;
        data[at] = std::byte{c.b};
        data[at + 1] = std::byte{c.g};
        data[at + 2] = std::byte{c.r};
    }

    /// Glyph-like strokes of `ink` on `paper` over [x0, x1) x [y0, y1),
    /// shifted by `phase` so a repaint changes them.
    void text(std::uint32_t x0, std::uint32_t y0, std::uint32_t x1, std::uint32_t y1, Rgb ink, Rgb paper,
              std::uint32_t phase = 0)
    {
        for (std::uint32_t y = y0; y < y1; ++y) {
            for (std::uint32_t x = x0; x < x1; ++x) {
                const std::uint32_t cx = (x + phase) % 6;
                const std::uint32_t cy = y % 8;
                const bool stroke = (cx == 1 && cy < 7) || (cy == 3 && cx < 5) || (cx == 4 && cy == 6);
                set(x, y, stroke ? ink : paper);
            }
        }
    }

    [[nodiscard]] codec::ImageView view() const
    {
        return {.data = data, .width = width, .height = height, .stride = std::size_t{width} * 4};
    }
};

constexpr Rgb red{220, 30, 30};
constexpr Rgb blue{20, 40, 190};
constexpr Rgb yellow{250, 235, 80};
constexpr Rgb black{0, 0, 0};
constexpr Rgb white{255, 255, 255};

/// Coloured text on the left and top, grey text in the bottom-right tile.
Image desktop()
{
    Image image;
    image.text(0, 0, width, 64, red, yellow);
    image.text(0, 64, 128, height, yellow, blue);
    image.text(128, 64, width, height, black, white);
    return image;
}

/// What the stand-in encoder was given, in order.
struct Pictures {
    std::vector<codec::Yuv420Frame> list;
    std::size_t bitstream_size = 8;
    std::size_t calls = 0;
    /// Drop the picture of this encode call (counting from 0).
    std::optional<std::size_t> drop_call;
};

class FakeEncoder final : public video::H264Encoder {
public:
    FakeEncoder(const video::EncoderConfig& config, std::shared_ptr<Pictures> pictures)
        : config_(config), pictures_(std::move(pictures))
    {
    }

    [[nodiscard]] video::Backend backend() const noexcept override { return video::Backend::openh264; }
    [[nodiscard]] const video::EncoderConfig& config() const noexcept override { return config_; }
    [[nodiscard]] farland::Result<void> configure(const video::EncoderConfig& config) override
    {
        config_ = config;
        idr_ = true;
        return {};
    }
    [[nodiscard]] farland::Result<void> set_rate_control(const video::RateControl& rate) override
    {
        config_.rate = rate;
        return {};
    }
    void request_idr() noexcept override { idr_ = true; }

    [[nodiscard]] farland::Result<video::EncodedFrame> encode(const codec::Yuv420View& picture,
                                                              const video::FrameOptions& options) override
    {
        REQUIRE((picture.width == config_.width && picture.height == config_.height));
        if (pictures_->drop_call == pictures_->calls++) {
            return video::EncodedFrame{};
        }
        codec::Yuv420Frame copy(picture.width, picture.height);
        std::ranges::copy(picture.y.first(copy.y().size()), copy.y().begin());
        std::ranges::copy(picture.u.first(copy.u().size()), copy.u().begin());
        std::ranges::copy(picture.v.first(copy.v().size()), copy.v().begin());
        pictures_->list.push_back(std::move(copy));

        video::EncodedFrame frame;
        frame.idr = std::exchange(idr_, false) || options.force_idr;
        frame.qp = 22;
        // An access unit delimiter and filler: only the size matters here.
        frame.bitstream.assign(std::max<std::size_t>(pictures_->bitstream_size, 6), std::byte{0x10});
        frame.bitstream[0] = frame.bitstream[1] = frame.bitstream[2] = std::byte{0};
        frame.bitstream[3] = std::byte{1};
        frame.bitstream[4] = std::byte{0x09};
        return frame;
    }

private:
    video::EncoderConfig config_;
    std::shared_ptr<Pictures> pictures_;
    bool idr_ = true;
};

struct Setup {
    std::shared_ptr<Pictures> pictures = std::make_shared<Pictures>();
    std::unique_ptr<video::Avc444Encoder> encoder;

    explicit Setup(Avc444Version version, video::Avc444Policy policy = {})
    {
        video::EncoderConfig config;
        config.width = avc::coded_size(width);
        config.height = avc::coded_size(height);
        encoder = std::make_unique<video::Avc444Encoder>(std::make_unique<FakeEncoder>(config, pictures), width, height,
                                                         version, policy);
    }
};

/// The client: FreeRDP's decoder steps on a persistent YUV444 surface.
class Client {
public:
    explicit Client(Avc444Version version) : version_(version) {}

    /// Applies one frame; the pictures come from the stand-in encoder in
    /// stream order, as a single decoder would produce them.
    void apply(const video::Avc444Frame& frame, const Pictures& pictures)
    {
        const auto parsed = avc::decode_avc444(frame.bitmap_stream);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->layout == frame.layout);
        const auto next = [&]() -> const codec::Yuv420Frame& {
            REQUIRE(next_ < pictures.list.size());
            return pictures.list[next_++];
        };
        for (const auto& region : parsed->first.regions) {
            CHECK((region.rect.right <= width && region.rect.bottom <= height));
            CHECK(region.rect.top % 16 == 0);  // FreeRDP's v1 decoder needs it
        }
        switch (parsed->layout) {
        case Avc444Layout::luma_and_chroma: {
            const auto& main = next();
            const auto& aux = next();
            for (const auto& region : parsed->first.regions) {
                codec::apply_main_view(main.view(), region.rect, yuv_);
            }
            for (const auto& region : parsed->second->regions) {
                codec::apply_aux_view(aux.view(), version_, region.rect, yuv_);
            }
            break;
        }
        case Avc444Layout::luma: {
            const auto& main = next();
            for (const auto& region : parsed->first.regions) {
                codec::apply_main_view(main.view(), region.rect, yuv_);
            }
            break;
        }
        case Avc444Layout::chroma: {
            const auto& aux = next();
            for (const auto& region : parsed->first.regions) {
                codec::apply_aux_view(aux.view(), version_, region.rect, yuv_);
            }
            break;
        }
        }
        CHECK(next_ == pictures.list.size());  // every picture was used, in order
    }

    /// Largest B, G or R difference to what YUV444 shows of `image`.
    [[nodiscard]] int error(const Image& image) const
    {
        codec::Yuv444Frame source(avc::coded_size(width), avc::coded_size(height));
        codec::bgrx_to_yuv444(image.view(), source);
        std::vector<std::byte> expected(image.data.size());
        std::vector<std::byte> shown(image.data.size());
        codec::yuv444_to_bgrx(source.view(), width, height, codec::ChromaFilter::none, expected);
        codec::yuv444_to_bgrx(yuv_.view(), width, height, codec::ChromaFilter::reverse, shown);
        int worst = 0;
        for (std::size_t i = 0; i < shown.size(); ++i) {
            if (i % 4 != 3) {
                worst = std::max(worst, std::abs(std::to_integer<int>(shown[i]) - std::to_integer<int>(expected[i])));
            }
        }
        return worst;
    }

private:
    Avc444Version version_;
    codec::Yuv444Frame yuv_{avc::coded_size(width), avc::coded_size(height)};
    std::size_t next_ = 0;
};

const Rect16 whole{0, 0, width, height};

video::Avc444Frame encode(Setup& setup, const Image& image, std::span<const Rect16> damage)
{
    auto frame = setup.encoder->encode(image.view(), damage);
    REQUIRE(frame.has_value());
    REQUIRE(frame->has_value());
    return std::move(**frame);
}

}  // namespace

TEST_CASE("AVC444 encoder: the first frame is an IDR with both views")
{
    for (const auto version : {Avc444Version::v1, Avc444Version::v2}) {
        Setup setup(version);
        Client client(version);
        const auto image = desktop();
        const auto frame = encode(setup, image, std::span(&whole, 1));
        CHECK(frame.layout == Avc444Layout::luma_and_chroma);
        CHECK(frame.idr);
        REQUIRE(frame.luma_regions.size() == 1);
        CHECK(frame.luma_regions[0] == whole);
        // Five of the six tiles are coloured; the grey one needs no chroma.
        CHECK(frame.chroma_regions.size() == 5);
        CHECK(std::ranges::find(frame.chroma_regions, Rect16{128, 64, 150, 100}) == frame.chroma_regions.end());
        CHECK(frame.dest_rect == whole);
        CHECK(setup.pictures->list.size() == 2);
        client.apply(frame, *setup.pictures);
        CHECK(client.error(image) <= 5);

        // Nothing changed and nothing waits: nothing to send.
        const auto idle = setup.encoder->encode(image.view(), {});
        REQUIRE(idle.has_value());
        CHECK_FALSE(idle->has_value());
        CHECK_FALSE(setup.encoder->chroma_pending());
    }
}

TEST_CASE("AVC444 encoder: grey changes go out as luma only")
{
    Setup setup(Avc444Version::v2);
    Client client(Avc444Version::v2);
    auto image = desktop();
    client.apply(encode(setup, image, std::span(&whole, 1)), *setup.pictures);

    image.text(128, 64, width, height, black, white, 3);
    const Rect16 damage{130, 70, 140, 90};
    const auto frame = encode(setup, image, std::span(&damage, 1));
    CHECK(frame.layout == Avc444Layout::luma);
    CHECK(frame.luma_regions == std::vector<Rect16>{{128, 64, 150, 100}});
    CHECK(frame.dest_rect == Rect16{128, 64, 150, 100});
    CHECK(frame.chroma_regions.empty());
    client.apply(frame, *setup.pictures);
    CHECK(client.error(image) <= 5);
}

TEST_CASE("AVC444 encoder: coloured changes send both views of the changed tiles")
{
    Setup setup(Avc444Version::v1);
    Client client(Avc444Version::v1);
    auto image = desktop();
    client.apply(encode(setup, image, std::span(&whole, 1)), *setup.pictures);

    image.text(64, 0, 128, 64, blue, yellow, 2);
    const Rect16 damage{64, 0, 128, 64};
    const auto frame = encode(setup, image, std::span(&damage, 1));
    CHECK(frame.layout == Avc444Layout::luma_and_chroma);
    CHECK(frame.luma_regions == std::vector<Rect16>{damage});
    CHECK(frame.chroma_regions == std::vector<Rect16>{damage});
    CHECK_FALSE(frame.idr);
    client.apply(frame, *setup.pictures);
    CHECK(client.error(image) <= 5);
}

TEST_CASE("AVC444 encoder: deferred chroma follows as LC 2")
{
    for (const auto version : {Avc444Version::v1, Avc444Version::v2}) {
        Setup setup(version, {.defer_chroma = true});
        Client client(version);
        auto image = desktop();

        // Even the first frame holds its chroma back...
        auto frame = encode(setup, image, std::span(&whole, 1));
        CHECK(frame.layout == Avc444Layout::luma);
        CHECK(setup.encoder->chroma_pending());
        client.apply(frame, *setup.pictures);
        CHECK(client.error(image) > 40);  // 4:2:0 smears the coloured strokes

        // ...and the next frame without damage brings it.
        frame = encode(setup, image, {});
        CHECK(frame.layout == Avc444Layout::chroma);
        CHECK(frame.luma_regions.empty());
        CHECK(frame.chroma_regions.size() == 5);
        CHECK_FALSE(setup.encoder->chroma_pending());
        client.apply(frame, *setup.pictures);
        CHECK(client.error(image) <= 5);

        // Another coloured change: luma now, chroma once things are quiet.
        image.text(0, 0, 64, 64, blue, white, 1);
        const Rect16 damage{0, 0, 64, 64};
        frame = encode(setup, image, std::span(&damage, 1));
        CHECK(frame.layout == Avc444Layout::luma);
        client.apply(frame, *setup.pictures);
        frame = encode(setup, image, {});
        CHECK(frame.layout == Avc444Layout::chroma);
        CHECK(frame.chroma_regions == std::vector<Rect16>{damage});
        client.apply(frame, *setup.pictures);
        CHECK(client.error(image) <= 5);
    }
}

TEST_CASE("AVC444 encoder: deferred chroma waits at most max_chroma_delay frames")
{
    Setup setup(Avc444Version::v2, {.defer_chroma = true, .max_chroma_delay = 3});
    Client client(Avc444Version::v2);
    auto image = desktop();
    std::vector<Avc444Layout> layouts;
    for (std::uint32_t i = 0; i < 5; ++i) {
        image.text(0, 0, 64, 64, blue, yellow, i);  // the same tile keeps changing
        const auto frame = encode(setup, image, std::span(&whole, 1));
        layouts.push_back(frame.layout);
        client.apply(frame, *setup.pictures);
    }
    // Pending since frame 1: frames 2 and 3 wait, frame 4 is 3 frames later.
    CHECK(layouts == std::vector<Avc444Layout>{Avc444Layout::luma, Avc444Layout::luma, Avc444Layout::luma,
                                               Avc444Layout::luma_and_chroma, Avc444Layout::luma});
}

TEST_CASE("AVC444 encoder: a large main view holds the chroma back")
{
    Setup setup(Avc444Version::v1, {.luma_budget_bytes = 1000});
    Client client(Avc444Version::v1);
    const auto image = desktop();
    setup.pictures->bitstream_size = 2000;
    auto frame = encode(setup, image, std::span(&whole, 1));
    CHECK(frame.layout == Avc444Layout::luma);
    client.apply(frame, *setup.pictures);
    setup.pictures->bitstream_size = 100;
    frame = encode(setup, image, {});
    CHECK(frame.layout == Avc444Layout::chroma);
    client.apply(frame, *setup.pictures);
    CHECK(client.error(image) <= 5);
}

TEST_CASE("AVC444 encoder: dropped pictures and IDR requests")
{
    Setup setup(Avc444Version::v2);
    Client client(Avc444Version::v2);
    auto image = desktop();

    // A dropped main view sends nothing; the same damage goes out next time.
    setup.pictures->drop_call = 0;
    const auto dropped = setup.encoder->encode(image.view(), std::span(&whole, 1));
    REQUIRE(dropped.has_value());
    CHECK_FALSE(dropped->has_value());
    auto frame = encode(setup, image, std::span(&whole, 1));
    CHECK(frame.idr);
    CHECK(frame.layout == Avc444Layout::luma_and_chroma);
    client.apply(frame, *setup.pictures);

    // A dropped chroma view leaves luma only, with the chroma pending.
    image.text(0, 64, 64, height, red, blue, 4);
    const Rect16 damage{0, 64, 64, height};
    setup.pictures->drop_call = setup.pictures->calls + 1;  // the second picture of the frame
    frame = encode(setup, image, std::span(&damage, 1));
    CHECK(frame.layout == Avc444Layout::luma);
    CHECK(setup.encoder->chroma_pending());
    client.apply(frame, *setup.pictures);
    frame = encode(setup, image, {});
    CHECK(frame.layout == Avc444Layout::chroma);
    CHECK(frame.chroma_regions == std::vector<Rect16>{damage});
    client.apply(frame, *setup.pictures);
    CHECK(client.error(image) <= 5);

    // request_idr starts over with the whole surface, even without damage.
    setup.encoder->request_idr();
    frame = encode(setup, image, {});
    CHECK(frame.idr);
    CHECK(frame.luma_regions == std::vector<Rect16>{whole});
    CHECK(frame.layout == Avc444Layout::luma_and_chroma);
    client.apply(frame, *setup.pictures);
    CHECK(client.error(image) <= 5);
}
