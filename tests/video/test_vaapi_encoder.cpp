// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The VA-API encoder beyond what every backend is tested for
// (test_h264_encoder.cpp): profiles, both ways of writing headers, dmabuf
// input with the GPU colour conversion, and rate control. Skipped without a
// VA-API device (FARLAND_VAAPI_DEVICE picks one); the decode checks need
// ffmpeg and ffprobe (FARLAND_FFMPEG, FARLAND_FFPROBE).

#include <farland/base/error.hpp>
#include <farland/codec/avc420.hpp>
#include <farland/codec/h264_nal.hpp>
#include <farland/codec/image.hpp>
#include <farland/codec/yuv.hpp>
#include <farland/server/test_pattern.hpp>
#include <farland/video/h264_encoder.hpp>
#include <farland/video/vaapi_encoder.hpp>

#include "h264_test_support.hpp"
#include "vaapi_test_source.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace video = farland::video;
namespace codec = farland::codec;
namespace avc = farland::codec::avc;
namespace h264 = farland::codec::h264;
using farland::Errc;
using namespace farland::test;  // NOLINT(google-build-using-namespace)

namespace {

std::string device_or_skip()
{
    const auto info = video::vaapi::probe(env_or("FARLAND_VAAPI_DEVICE", ""));
    if (!info) {
        SKIP("no VA-API device: " + info.error().message());
    }
    return info->render_node;
}

video::EncoderConfig config_for(std::uint32_t width, std::uint32_t height)
{
    video::EncoderConfig config;
    config.width = avc::coded_size(width);
    config.height = avc::coded_size(height);
    config.fps = 30;
    config.rate.quality = 23;
    return config;
}

std::unique_ptr<video::vaapi::VaapiEncoder> open_encoder(const std::string& node, const video::EncoderConfig& config)
{
    auto created = video::vaapi::create(config, {.render_node = node});
    if (!created && created.error().code == Errc::unsupported) {
        SKIP("VA-API cannot encode this: " + created.error().message());
    }
    REQUIRE(created.has_value());
    return std::unique_ptr<video::vaapi::VaapiEncoder>(dynamic_cast<video::vaapi::VaapiEncoder*>(created->release()));
}

/// Scrolling high-contrast texture: every macroblock changes every frame.
class Motion {
public:
    Motion(std::uint32_t width, std::uint32_t height)
        : width_(width), height_(height), pixels_(std::size_t{width} * height * 4)
    {
    }
    codec::ImageView render(std::uint32_t t)
    {
        for (std::uint32_t y = 0; y < height_; ++y) {
            for (std::uint32_t x = 0; x < width_; ++x) {
                const std::uint32_t u = x + (t * 5);
                const std::size_t at = ((std::size_t{y} * width_) + x) * 4;
                pixels_[at] = static_cast<std::byte>((y * 2) + ((u / 8) * 16));
                pixels_[at + 1] = static_cast<std::byte>(u ^ y);
                pixels_[at + 2] = static_cast<std::byte>((u * 3) + y);
                pixels_[at + 3] = std::byte{0xFF};
            }
        }
        return {.data = pixels_, .width = width_, .height = height_, .stride = std::size_t{width_} * 4};
    }

private:
    std::uint32_t width_;
    std::uint32_t height_;
    std::vector<std::byte> pixels_;
};

/// Decodes `stream` and returns the worst YUV PSNR against `inputs` (visible
/// area only), or nothing without ffmpeg.
std::optional<double> worst_psnr(std::span<const std::byte> stream, const std::vector<codec::Yuv420Frame>& inputs,
                                 std::uint32_t visible_width, std::uint32_t visible_height)
{
    const std::string ffmpeg = env_or("FARLAND_FFMPEG", "ffmpeg");
    if (!have_tool(ffmpeg)) {
        return std::nullopt;
    }
    const auto decoded = decode_h264(ffmpeg, stream);
    const auto& first = inputs.front();
    const std::size_t luma = std::size_t{first.width()} * first.height();
    const std::size_t frame_size = luma * 3 / 2;
    REQUIRE(decoded.size() == frame_size * inputs.size());
    double worst = 99;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto bytes = std::span(decoded).subspan(i * frame_size, frame_size);
        const auto input = inputs[i].view();
        Psnr psnr;
        for (std::uint32_t row = 0; row < visible_height; ++row) {
            psnr.add(input.y.subspan(row * input.y_stride, visible_width),
                     bytes.subspan(row * std::size_t{first.width()}, visible_width));
        }
        const std::size_t cw = first.width() / 2;
        for (std::uint32_t row = 0; row < visible_height / 2; ++row) {
            psnr.add(input.u.subspan(row * input.uv_stride, visible_width / 2),
                     bytes.subspan(luma + (row * cw), visible_width / 2));
            psnr.add(input.v.subspan(row * input.uv_stride, visible_width / 2),
                     bytes.subspan(luma + (luma / 4) + (row * cw), visible_width / 2));
        }
        worst = std::min(worst, psnr.db());
    }
    return worst;
}

std::string probe_one(std::span<const std::byte> stream, const std::string& key)
{
    const std::string ffprobe = env_or("FARLAND_FFPROBE", "ffprobe");
    if (!have_tool(ffprobe)) {
        return {};
    }
    const auto values = probe_h264(ffprobe, stream, "stream=" + key);
    const auto found = values.find(key);
    return found == values.end() || found->second.empty() ? std::string{} : found->second.front();
}

}  // namespace

TEST_CASE("VA-API describes its device")
{
    const auto node = device_or_skip();
    const auto info = video::vaapi::probe(node);
    REQUIRE(info.has_value());
    std::cout << video::vaapi::describe(*info) << '\n';
    CHECK_FALSE(info->profiles.empty());
    CHECK((info->enc_slice || info->enc_slice_lp));
    CHECK(video::vaapi::probe("/nonexistent/renderD128").error().code == Errc::unsupported);
}

TEST_CASE("VA-API encodes every profile, with packed and driver headers")
{
    const auto node = device_or_skip();
    constexpr std::uint32_t width = 318;
    constexpr std::uint32_t height = 238;
    for (const bool packed : {true, false}) {
        // Restored even when an assertion ends the test case.
        struct PackedHeaders {
            explicit PackedHeaders(bool on)
            {
                if (!on) {
                    ::setenv("FARLAND_VAAPI_PACKED_HEADERS", "0", 1);  // NOLINT(concurrency-mt-unsafe)
                }
            }
            PackedHeaders(const PackedHeaders&) = delete;
            PackedHeaders& operator=(const PackedHeaders&) = delete;
            PackedHeaders(PackedHeaders&&) = delete;
            PackedHeaders& operator=(PackedHeaders&&) = delete;
            ~PackedHeaders() { ::unsetenv("FARLAND_VAAPI_PACKED_HEADERS"); }  // NOLINT(concurrency-mt-unsafe)
        } const env(packed);
        // Drivers that take a packed slice header (Mesa 26 radeonsi) write no
        // SPS, PPS or slice NAL header of their own; only report them.
        const bool driver_writes_headers =
            (video::vaapi::probe(node)->packed_headers & 0x4U /* VA_ENC_PACKED_HEADER_SLICE */) == 0;
        for (const auto profile : {video::Profile::constrained_baseline, video::Profile::main, video::Profile::high}) {
            CAPTURE(packed, static_cast<int>(profile));
            if (std::ranges::find(video::vaapi::probe(node)->profiles, profile) ==
                video::vaapi::probe(node)->profiles.end()) {
                continue;
            }
            auto config = config_for(width, height);
            config.profile = profile;
            auto encoder = open_encoder(node, config);
            farland::server::TestPattern pattern(width, height);
            std::vector<codec::Yuv420Frame> inputs;
            std::vector<std::byte> stream;
            for (int i = 0; i < 8; ++i) {
                codec::Yuv420Frame yuv(config.width, config.height);
                codec::bgrx_to_yuv420(pattern.render(static_cast<std::uint64_t>(i) * 3), yuv);
                video::FrameOptions options;
                options.force_idr = i == 4;
                const auto frame = encoder->encode(yuv.view(), options);
                if (!packed && !driver_writes_headers) {
                    std::cout << "driver-written headers: " << (frame ? "work" : frame.error().message()) << '\n';
                    break;
                }
                REQUIRE(frame.has_value());
                CHECK(frame->idr == (i == 0 || i == 4));
                stream.insert(stream.end(), frame->bitstream.begin(), frame->bitstream.end());
                inputs.push_back(std::move(yuv));
            }
            if (inputs.empty()) {
                continue;
            }
            const auto psnr = worst_psnr(stream, inputs, width, height);
            if (!psnr) {
                continue;
            }
            std::cout << "vaapi profile " << static_cast<int>(profile) << (packed ? " packed" : " driver")
                      << " headers: worst PSNR " << *psnr << " dB\n";
            CHECK(*psnr >= 35.0);
            const char* expected = profile == video::Profile::constrained_baseline ? "Constrained Baseline"
                                   : profile == video::Profile::main               ? "Main"
                                                                                   : "High";
            CHECK(probe_one(stream, "profile") == expected);
            CHECK(probe_one(stream, "color_range") == "pc");
            CHECK(probe_one(stream, "color_space") == "bt709");
        }
    }
}

TEST_CASE("VA-API converts dmabufs like the CPU")
{
    const auto node = device_or_skip();
    constexpr std::uint32_t width = 320;
    constexpr std::uint32_t height = 240;
    auto encoder = open_encoder(node, config_for(width, height));
    if (!encoder->accepts_dmabuf()) {
        SKIP("the VA-API driver has no video processing");
    }
    auto source = make_va_source(node, width, height);
    REQUIRE(source != nullptr);
    std::cout << "VA-exported dmabuf: fourcc " << std::hex << source->frame().fourcc << ", modifier "
              << source->frame().modifier << std::dec << ", " << source->frame().plane_count << " planes\n";

    farland::server::TestPattern pattern(width, height);
    Motion motion(width, height);
    for (const bool moving : {false, true}) {
        const auto image = moving ? motion.render(3) : pattern.render(5);
        REQUIRE(source->write(image));
        const auto gpu = encoder->convert(source->frame());
        REQUIRE(gpu.has_value());
        codec::Yuv420Frame cpu(width, height);
        codec::bgrx_to_yuv420(image, cpu);

        auto compare = [](std::span<const std::byte> a, std::span<const std::byte> b, const char* plane) {
            int worst = 0;
            double sum = 0;
            for (std::size_t i = 0; i < a.size(); ++i) {
                const int d = std::abs(std::to_integer<int>(a[i]) - std::to_integer<int>(b[i]));
                worst = std::max(worst, d);
                sum += d;
            }
            const double mean = sum / static_cast<double>(a.size());
            std::cout << "  " << plane << ": mean |GPU - CPU| " << mean << ", max " << worst << '\n';
            return mean;
        };
        std::cout << (moving ? "motion texture" : "test pattern") << ":\n";
        auto gpu_frame = *gpu;
        CHECK(compare(gpu_frame.y(), cpu.y(), "Y") < 1.0);
        CHECK(compare(gpu_frame.u(), cpu.u(), "U") < 2.0);
        CHECK(compare(gpu_frame.v(), cpu.v(), "V") < 2.0);
    }

    video::DmabufFrame bad = source->frame();
    bad.width = width + 16;
    CHECK(encoder->encode_dmabuf(bad, {}).error().code == Errc::invalid_value);
    bad = source->frame();
    bad.fourcc = video::drm_fourcc::code('N', 'V', '1', '2');
    CHECK(encoder->encode_dmabuf(bad, {}).error().code == Errc::unsupported);
}

TEST_CASE("VA-API encodes dmabufs from VA and GBM")
{
    const auto node = device_or_skip();
    constexpr std::uint32_t width = 318;  // coded as 320x240
    constexpr std::uint32_t height = 238;
    const auto config = config_for(width, height);

    struct Kind {
        const char* name;
        std::unique_ptr<DmabufSource> source;
    };
    std::vector<Kind> kinds;
    kinds.push_back({"VA-exported", make_va_source(node, width, height)});
    kinds.push_back({"GBM linear", make_gbm_source(node, width, height, true)});
    kinds.push_back({"GBM driver layout", make_gbm_source(node, width, height, false)});
    for (auto& [name, source] : kinds) {
        CAPTURE(name);
        if (source == nullptr) {
            std::cout << name << ": not available\n";
            continue;
        }
        std::cout << name << ": modifier " << std::hex << source->frame().modifier << std::dec << '\n';
        auto encoder = open_encoder(node, config);
        REQUIRE(encoder->accepts_dmabuf());
        farland::server::TestPattern pattern(width, height);
        std::vector<codec::Yuv420Frame> inputs;
        std::vector<std::byte> stream;
        for (int i = 0; i < 10; ++i) {
            const auto image = pattern.render(static_cast<std::uint64_t>(i) * 3);
            REQUIRE(source->write(image));
            codec::Yuv420Frame yuv(config.width, config.height);
            codec::bgrx_to_yuv420(image, yuv);
            inputs.push_back(std::move(yuv));
            if (i == 6) {
                encoder->request_idr();
            }
            const auto frame = encoder->encode_dmabuf(source->frame(), {});
            REQUIRE(frame.has_value());
            CHECK(frame->idr == (i == 0 || i == 6));
            const auto units = h264::split_annex_b(frame->bitstream);
            REQUIRE(units.has_value());
            CHECK(units->front().type == h264::nal_type::aud);
            stream.insert(stream.end(), frame->bitstream.begin(), frame->bitstream.end());
        }
        // Against the CPU conversion: GPU conversion and coding loss together.
        if (const auto psnr = worst_psnr(stream, inputs, width, height)) {
            std::cout << name << ": worst PSNR " << *psnr << " dB against the CPU conversion\n";
            CHECK(*psnr >= 33.0);
        }
        encoder->forget_dmabufs();
        CHECK(encoder->encode_dmabuf(source->frame(), {}).has_value());
    }
}

TEST_CASE("VA-API holds and changes the bitrate")
{
    const auto node = device_or_skip();
    constexpr std::uint32_t width = 1280;
    constexpr std::uint32_t height = 720;
    constexpr int frames = 90;
    auto config = config_for(width, height);
    config.rate.mode = video::RateControl::Mode::bitrate;
    config.rate.bitrate_kbps = 4000;
    auto encoder = open_encoder(node, config);
    Motion motion(width, height);
    codec::Yuv420Frame yuv(config.width, config.height);

    // kbit/s over the last two thirds of `frames` pictures at config.fps.
    std::uint32_t t = 0;
    auto measure = [&] {
        std::size_t bytes = 0;
        for (int i = 0; i < frames; ++i) {
            codec::bgrx_to_yuv420(motion.render(t++), yuv);
            const auto frame = encoder->encode(yuv.view(), {});
            REQUIRE(frame.has_value());
            if (t > 1) {
                CHECK_FALSE(frame->idr);
            }
            if (i >= frames / 3) {
                bytes += frame->bitstream.size();
            }
        }
        return static_cast<double>(bytes) * 8.0 * config.fps / (frames - (frames / 3)) / 1000.0;
    };
    const double high = measure();
    auto rate = config.rate;
    rate.bitrate_kbps = 1000;
    REQUIRE(encoder->set_rate_control(rate).has_value());
    const double low = measure();
    std::cout << "vaapi CBR 1280x720: 4000 kbit/s target -> " << high << " kbit/s; 1000 kbit/s target -> " << low
              << " kbit/s\n";
    CHECK(high > 4000 * 0.6);
    CHECK(high < 4000 * 1.4);
    CHECK(low < high * 0.5);
    CHECK(low < 1000 * 1.5);

    // Constant QP: the slice QP is the configured one; a new QP starts an IDR.
    video::RateControl cqp;
    cqp.quality = 30;
    REQUIRE(encoder->set_rate_control(cqp).has_value());
    codec::bgrx_to_yuv420(motion.render(t++), yuv);
    auto frame = encoder->encode(yuv.view(), {});
    REQUIRE(frame.has_value());
    CHECK(frame->idr);
    CHECK(frame->qp == 30);
    cqp.quality = 36;
    REQUIRE(encoder->set_rate_control(cqp).has_value());
    frame = encoder->encode(yuv.view(), {});
    REQUIRE(frame.has_value());
    CHECK(frame->idr);
    CHECK(frame->qp == 36);
}
