// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// NVENC backend. Skipped without an NVIDIA GPU and driver; the dmabuf tests
// also need GBM and EGL at build time and access to the render node (group
// render or a seat), the decode checks ffmpeg. Environment:
//   FARLAND_VAAPI_DEVICE  DRM render node of the NVIDIA GPU (default: CUDA device 0)
//   FARLAND_FFMPEG, FARLAND_FFPROBE  tools to use instead of the ones in PATH

#include <farland/base/error.hpp>
#include <farland/base/log.hpp>
#include <farland/codec/avc420.hpp>
#include <farland/codec/h264_nal.hpp>
#include <farland/codec/image.hpp>
#include <farland/codec/yuv.hpp>
#include <farland/server/test_pattern.hpp>
#include <farland/video/h264_encoder.hpp>
#include <farland/video/nvenc_encoder.hpp>

#include "h264_test_support.hpp"
#include "nvenc_test_source.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
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

std::string render_node()
{
    return env_or("FARLAND_VAAPI_DEVICE", "");
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

std::unique_ptr<video::nvenc::NvencEncoder> open_encoder(const video::EncoderConfig& config,
                                                         const video::BackendOptions& options = {},
                                                         const video::nvenc::Tuning& tuning = {})
{
    auto with_node = options;
    if (with_node.render_node.empty()) {
        with_node.render_node = render_node();
    }
    auto created = video::nvenc::create(config, with_node, tuning);
    if (!created && created.error().code == Errc::unsupported) {
        SKIP("NVENC is not available: " + created.error().message());
    }
    REQUIRE(created.has_value());
    return std::unique_ptr<video::nvenc::NvencEncoder>(dynamic_cast<video::nvenc::NvencEncoder*>(created->release()));
}

/// Debug logging for the duration of a test: the dmabuf route logs why a
/// buffer cannot be imported only at that level.
class DebugLog {
public:
    DebugLog() { farland::log::set_level(farland::log::Level::debug); }
    DebugLog(const DebugLog&) = delete;
    DebugLog& operator=(const DebugLog&) = delete;
    DebugLog(DebugLog&&) = delete;
    DebugLog& operator=(DebugLog&&) = delete;
    ~DebugLog() { farland::log::set_level(farland::log::Level::info); }
};

/// The dmabuf route, or a skip saying why not.
std::string dmabuf_node_or_skip(const video::nvenc::NvencEncoder& encoder)
{
    if (!encoder.accepts_dmabuf()) {
        SKIP("no zero-copy dmabuf input (EGL/OpenGL on the NVIDIA device; see the log)");
    }
    if (encoder.device().render_node.empty()) {
        SKIP("the NVIDIA GPU has no render node");
    }
    return encoder.device().render_node;
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

/// Samples of `a` and `b` in the visible `width` x `height` area that differ.
std::size_t visible_mismatches(const codec::Yuv420View& a, const codec::Yuv420View& b, std::uint32_t width,
                               std::uint32_t height)
{
    std::size_t count = 0;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            count += a.y[(y * a.y_stride) + x] != b.y[(y * b.y_stride) + x] ? 1U : 0U;
        }
    }
    for (std::uint32_t y = 0; y < (height + 1U) / 2U; ++y) {
        for (std::uint32_t x = 0; x < (width + 1U) / 2U; ++x) {
            count += a.u[(y * a.uv_stride) + x] != b.u[(y * b.uv_stride) + x] ? 1U : 0U;
            count += a.v[(y * a.uv_stride) + x] != b.v[(y * b.uv_stride) + x] ? 1U : 0U;
        }
    }
    return count;
}

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

TEST_CASE("NVENC describes its device")
{
    const auto info = video::nvenc::probe(render_node());
    if (!info) {
        SKIP("no NVENC device: " + info.error().message());
    }
    std::cout << video::nvenc::describe(*info) << '\n';
    CHECK_FALSE(info->name.empty());
    CHECK(((info->nvenc_api_major << 4U) | info->nvenc_api_minor) >= 0xC0U);
    CHECK(info->max_width >= 1920);
    CHECK(info->max_height >= 1080);
}

TEST_CASE("NVENC converts colour like the CPU")
{
    constexpr std::uint32_t width = 318;  // odd chroma edges, coded as 320x240
    constexpr std::uint32_t height = 238;
    auto encoder = open_encoder(config_for(width, height));

    farland::server::TestPattern pattern(width, height);
    Motion motion(width, height);
    codec::Yuv420Frame cpu(encoder->config().width, encoder->config().height);
    for (const bool moving : {false, true}) {
        CAPTURE(moving);
        const auto image = moving ? motion.render(3) : pattern.render(5);
        const auto gpu = encoder->convert(image);
        REQUIRE(gpu.has_value());
        codec::bgrx_to_yuv420(image, cpu);
        // Full-range BT.709 is the CPU's integer arithmetic, bit for bit.
        CHECK(visible_mismatches(gpu->view(), cpu.view(), width, height) == 0);
        // Outside the picture it is black.
        CHECK(gpu->view().y[gpu->width() - 1] == std::byte{0});
        CHECK(gpu->view().u[gpu->width() / 2 - 1] == std::byte{128});
    }

    // Limited-range BT.601: black 16, white 235, grey chroma 128.
    constexpr std::uint32_t flat_size = 256;
    auto limited = open_encoder(config_for(flat_size, flat_size),
                                {.color = {.matrix = video::ColorSpace::Matrix::bt601, .full_range = false}});
    std::vector<std::byte> flat(std::size_t{flat_size} * flat_size * 4);
    for (const auto& [value, luma] : {std::pair{0, 16}, std::pair{255, 235}, std::pair{128, 126}}) {
        std::ranges::fill(flat, static_cast<std::byte>(value));
        const auto yuv = limited->convert(
            codec::ImageView{.data = flat, .width = flat_size, .height = flat_size, .stride = flat_size * 4});
        REQUIRE(yuv.has_value());
        CHECK(std::to_integer<int>(yuv->view().y[0]) == luma);
        CHECK(std::to_integer<int>(yuv->view().u[0]) == 128);
        CHECK(std::to_integer<int>(yuv->view().v[0]) == 128);
    }
}

TEST_CASE("NVENC converts and encodes dmabufs from GBM")
{
    constexpr std::uint32_t width = 318;  // coded as 320x240
    constexpr std::uint32_t height = 238;
    const auto config = config_for(width, height);
    const DebugLog debug;
    auto encoder = open_encoder(config);
    const auto node = dmabuf_node_or_skip(*encoder);

    // LINEAR first: CPU writes to LINEAR buffers must come before the test
    // writes any buffer through OpenGL (nvenc_test_source.hpp). Then tiled on
    // the same encoder, as after a capture renegotiates.
    for (const bool linear : {true, false}) {
        const char* name = linear ? "GBM LINEAR" : "GBM tiled";
        CAPTURE(name);
        if (linear && !linear_writes_reliable()) {
            std::cout << name << ": not tested, an earlier test wrote a buffer through OpenGL\n";
            continue;
        }
        auto source = make_gbm_dmabuf(node, width, height, linear);
        if (source == nullptr) {
            SKIP(std::string(name) + " buffers are not available");
        }
        std::cout << name << ": modifier 0x" << std::hex << source->frame().modifier << std::dec << ", pitch "
                  << source->frame().planes[0].pitch << '\n';

        // The zero-copy route delivers the pixels exactly: the GPU conversion
        // of the dmabuf equals the CPU conversion of the same pixels.
        farland::server::TestPattern pattern(width, height);
        Motion motion(width, height);
        codec::Yuv420Frame cpu(config.width, config.height);
        for (const bool moving : {false, true}) {
            const auto image = moving ? motion.render(7) : pattern.render(2);
            REQUIRE(source->write(image));
            const auto gpu = encoder->convert(source->frame());
            if (!gpu) {
                FAIL(gpu.error().message());
            }
            codec::bgrx_to_yuv420(image, cpu);
            CHECK(visible_mismatches(gpu->view(), cpu.view(), width, height) == 0);
        }

        std::vector<codec::Yuv420Frame> inputs;
        std::vector<std::byte> stream;
        for (int i = 0; i < 10; ++i) {
            CAPTURE(i);
            const auto image = pattern.render(static_cast<std::uint64_t>(i) * 3);
            REQUIRE(source->write(image));
            codec::bgrx_to_yuv420(image, cpu);
            inputs.push_back(cpu);
            if (i == 6) {
                encoder->request_idr();
            }
            const auto frame = encoder->encode_dmabuf(source->frame(), {});
            if (!frame) {
                FAIL(frame.error().message());
            }
            CHECK(frame->idr == (i == 0 || i == 6));
            const auto units = h264::split_annex_b(frame->bitstream);
            REQUIRE(units.has_value());
            CHECK(units->front().type == h264::nal_type::aud);
            CHECK(h264::contains_idr(*units) == frame->idr);
            stream.insert(stream.end(), frame->bitstream.begin(), frame->bitstream.end());
        }
        if (const auto psnr = worst_psnr(stream, inputs, width, height)) {
            std::cout << name << ": worst PSNR " << *psnr << " dB against the CPU conversion\n";
            CHECK(*psnr >= 35.0);
        }
        encoder->forget_dmabufs();
        CHECK(encoder->encode_dmabuf(source->frame(), {}).has_value());
        encoder->request_idr();

        video::DmabufFrame bad = source->frame();
        bad.width = config.width + 16;
        CHECK(encoder->encode_dmabuf(bad, {}).error().code == Errc::invalid_value);
        bad = source->frame();
        bad.fourcc = video::drm_fourcc::code('N', 'V', '1', '2');
        CHECK(encoder->encode_dmabuf(bad, {}).error().code == Errc::unsupported);
    }
}

TEST_CASE("NVENC's own RGB conversion follows the VUI")
{
    // Evidence for nvenc_encoder.hpp: NVENC converts ARGB input with the
    // matrix and range the VUI signals, rounding to nearest where farland
    // (and FreeRDP) round down.
    constexpr std::uint32_t size = 256;
    const std::string ffmpeg = env_or("FARLAND_FFMPEG", "ffmpeg");
    struct Colour {
        int r, g, b;
    };
    const std::vector<Colour> colours{{255, 255, 255}, {0, 0, 0}, {255, 0, 0}, {0, 0, 255}, {30, 160, 220}};
    for (const bool bt709_full : {true, false}) {
        CAPTURE(bt709_full);
        const video::ColorSpace color =
            bt709_full ? video::ColorSpace{}
                       : video::ColorSpace{.matrix = video::ColorSpace::Matrix::bt601, .full_range = false};
        auto encoder = open_encoder(config_for(size, size), {.color = color}, {.nvenc_rgb_conversion = true});
        const auto node = dmabuf_node_or_skip(*encoder);
        auto source = make_gbm_dmabuf(node, size, size, false);
        if (source == nullptr) {
            SKIP("GBM buffers are not available");
        }
        if (!have_tool(ffmpeg)) {
            SKIP("ffmpeg not found");
        }
        video::RateControl nearly_lossless;
        nearly_lossless.quality = 1;
        REQUIRE(encoder->set_rate_control(nearly_lossless).has_value());

        std::vector<std::byte> pixels(std::size_t{size} * size * 4);
        for (const auto& [r, g, b] : colours) {
            for (std::size_t i = 0; i < pixels.size(); i += 4) {
                pixels[i] = static_cast<std::byte>(b);
                pixels[i + 1] = static_cast<std::byte>(g);
                pixels[i + 2] = static_cast<std::byte>(r);
                pixels[i + 3] = std::byte{0xFF};
            }
            REQUIRE(source->write({.data = pixels, .width = size, .height = size, .stride = std::size_t{size} * 4}));
            encoder->request_idr();
            const auto frame = encoder->encode_dmabuf(source->frame(), {});
            if (!frame) {
                FAIL(frame.error().message());
            }
            const auto decoded = decode_h264(ffmpeg, frame->bitstream);
            REQUIRE(decoded.size() == std::size_t{size} * size * 3 / 2);
            const int y = std::to_integer<int>(decoded[(size * size / 2) + (size / 2)]);
            const int u = std::to_integer<int>(decoded[(size * size) + (size * size / 8)]);
            const int v = std::to_integer<int>(decoded[(size * size * 5 / 4) + (size * size / 8)]);
            // The exact matrix the VUI signals.
            const double kr = bt709_full ? 0.2126 : 0.299;
            const double kb = bt709_full ? 0.0722 : 0.114;
            const double luma = (kr * r) + ((1 - kr - kb) * g) + (kb * b);
            const double ys = bt709_full ? 1.0 : 219.0 / 255.0;
            const double cs = bt709_full ? 1.0 : 224.0 / 255.0;
            const double ey = (bt709_full ? 0.0 : 16.0) + (ys * luma);
            const double eu = 128.0 + (cs * (b - luma) / (2.0 * (1.0 - kb)));
            const double ev = 128.0 + (cs * (r - luma) / (2.0 * (1.0 - kr)));
            std::cout << "NVENC ARGB input (" << r << ", " << g << ", " << b << "), VUI "
                      << (bt709_full ? "full BT.709" : "limited BT.601") << " -> Y " << y << " U " << u << " V " << v
                      << "; exact " << ey << " " << eu << " " << ev << '\n';
            CHECK(std::abs(y - std::clamp(ey, 0.0, 255.0)) <= 1.5);
            CHECK(std::abs(u - std::clamp(eu, 0.0, 255.0)) <= 1.5);
            CHECK(std::abs(v - std::clamp(ev, 0.0, 255.0)) <= 1.5);
        }
    }
}

TEST_CASE("NVENC holds and changes the bitrate")
{
    constexpr std::uint32_t width = 1280;
    constexpr std::uint32_t height = 720;
    constexpr int frames = 90;
    auto config = config_for(width, height);
    config.rate.mode = video::RateControl::Mode::bitrate;
    config.rate.bitrate_kbps = 4000;
    auto encoder = open_encoder(config);
    Motion motion(width, height);
    codec::Yuv420Frame yuv(config.width, config.height);

    // kbit/s over the last two thirds of `frames` pictures at config.fps.
    // Only the first picture after a new rate control mode is an IDR.
    std::uint32_t t = 0;
    auto measure = [&](bool new_mode) {
        std::size_t bytes = 0;
        for (int i = 0; i < frames; ++i) {
            codec::bgrx_to_yuv420(motion.render(t++), yuv);
            const auto frame = encoder->encode(yuv.view(), {});
            REQUIRE(frame.has_value());
            CHECK(frame->idr == (i == 0 && new_mode));
            if (i >= frames / 3) {
                bytes += frame->bitstream.size();
            }
        }
        return static_cast<double>(bytes) * 8.0 * config.fps / (frames - (frames / 3)) / 1000.0;
    };
    const double high = measure(true);
    auto rate = config.rate;
    rate.bitrate_kbps = 1000;
    REQUIRE(encoder->set_rate_control(rate).has_value());
    const double low = measure(false);
    std::cout << "nvenc CBR 1280x720: 4000 kbit/s target -> " << high << " kbit/s; 1000 kbit/s target -> " << low
              << " kbit/s\n";
    CHECK(high > 4000 * 0.7);
    CHECK(high < 4000 * 1.3);
    CHECK(low < 1000 * 1.3);
    CHECK(low > 1000 * 0.5);

    // Constant quality with a cap stays under the cap.
    video::RateControl capped;
    capped.quality = 18;
    capped.max_bitrate_kbps = 1500;
    REQUIRE(encoder->set_rate_control(capped).has_value());
    const double cq = measure(true);
    std::cout << "nvenc constant quality 18 capped at 1500 kbit/s -> " << cq << " kbit/s\n";
    CHECK(cq < 1500 * 1.3);

    // Constant QP: a new mode starts with an IDR, and so does a new QP.
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
    codec::bgrx_to_yuv420(motion.render(t++), yuv);
    frame = encoder->encode(yuv.view(), {});
    REQUIRE(frame.has_value());
    CHECK(frame->idr);
    CHECK(frame->qp == 36);
    codec::bgrx_to_yuv420(motion.render(t++), yuv);
    frame = encoder->encode(yuv.view(), {});
    REQUIRE(frame.has_value());
    CHECK_FALSE(frame->idr);
    CHECK(frame->qp == 36);
}

TEST_CASE("NVENC signals the profile and colour space")
{
    constexpr std::uint32_t width = 320;
    constexpr std::uint32_t height = 240;
    farland::server::TestPattern pattern(width, height);
    codec::Yuv420Frame yuv(width, height);
    codec::bgrx_to_yuv420(pattern.render(1), yuv);
    struct Case {
        video::Profile profile;
        const char* name;
        video::ColorSpace color;
        const char* range;
        const char* space;
    };
    const std::vector<Case> cases{
        {video::Profile::constrained_baseline, "Constrained Baseline", {}, "pc", "bt709"},
        {video::Profile::main, "Main", {}, "pc", "bt709"},
        {video::Profile::high,
         "High",
         {.matrix = video::ColorSpace::Matrix::bt601, .full_range = false},
         "tv",
         "smpte170m"},
    };
    for (const auto& c : cases) {
        CAPTURE(c.name);
        auto config = config_for(width, height);
        config.profile = c.profile;
        auto encoder = open_encoder(config, {.color = c.color});
        std::vector<std::byte> stream;
        for (int i = 0; i < 3; ++i) {
            const auto frame = encoder->encode(yuv.view(), {});
            REQUIRE(frame.has_value());
            stream.insert(stream.end(), frame->bitstream.begin(), frame->bitstream.end());
        }
        const auto profile = probe_one(stream, "profile");
        if (profile.empty()) {
            SKIP("ffprobe not found");
        }
        CHECK(profile == c.name);
        CHECK(probe_one(stream, "color_range") == c.range);
        CHECK(probe_one(stream, "color_space") == c.space);
        CHECK(probe_one(stream, "has_b_frames") == "0");
    }
}
