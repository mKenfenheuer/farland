// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// H.264 encoder backends. Tests that need a backend skip when it is missing
// (CI has neither OpenH264 nor x264); the decode checks also need ffmpeg and
// ffprobe. Environment:
//   FARLAND_OPENH264_LIBRARY  OpenH264 to load instead of the default names
//   FARLAND_VAAPI_DEVICE      DRM render node for VA-API (and NVENC) instead of the first that works
//   FARLAND_FFMPEG, FARLAND_FFPROBE  tools to use instead of the ones in PATH
// The latency benchmark is hidden: farland-unit-tests "[benchmark]".

#include <farland/base/error.hpp>
#include <farland/codec/avc420.hpp>
#include <farland/codec/h264_nal.hpp>
#include <farland/codec/image.hpp>
#include <farland/codec/yuv.hpp>
#include <farland/codec/yuv444.hpp>
#include <farland/server/test_pattern.hpp>
#include <farland/video/avc444_encoder.hpp>
#include <farland/video/h264_encoder.hpp>

#include "h264_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace video = farland::video;
namespace codec = farland::codec;
namespace avc = farland::codec::avc;
namespace h264 = farland::codec::h264;
using farland::Errc;
using namespace farland::test;  // NOLINT(google-build-using-namespace)

namespace {

constexpr std::uint32_t surface_width = 318;  // Odd sizes: coded as 320x240.
constexpr std::uint32_t surface_height = 238;
constexpr int frame_count = 12;

video::BackendOptions backend_options()
{
    return {.openh264_library = env_or("FARLAND_OPENH264_LIBRARY", ""),
            .render_node = env_or("FARLAND_VAAPI_DEVICE", "")};
}

video::EncoderConfig small_config()
{
    video::EncoderConfig config;
    config.width = avc::coded_size(surface_width);
    config.height = avc::coded_size(surface_height);
    config.fps = 30;
    config.rate.quality = 23;
    return config;
}

std::unique_ptr<video::H264Encoder> open_or_skip(video::Backend backend, const video::EncoderConfig& config)
{
    auto encoder = video::create_encoder(backend, config, backend_options());
    if (!encoder.has_value() && encoder.error().code == Errc::unsupported) {
        SKIP(std::string(video::to_string(backend)) + " is not available: " + encoder.error().message());
    }
    REQUIRE(encoder.has_value());
    return std::move(*encoder);
}

bool has_nal(const std::vector<h264::NalUnit>& units, std::uint8_t type)
{
    return std::ranges::any_of(units, [type](const h264::NalUnit& unit) { return unit.type == type; });
}

/// Encodes test pattern frames with IDRs at 0 (first), 5 (forced) and 8
/// (requested), checks the access units, and decodes them with ffmpeg.
void check_backend(video::Backend backend)
{
    const auto config = small_config();
    auto encoder = open_or_skip(backend, config);
    CHECK(encoder->backend() == backend);

    farland::server::TestPattern pattern(surface_width, surface_height);
    codec::Yuv420Frame yuv(config.width, config.height);
    std::vector<codec::Yuv420Frame> inputs;
    std::vector<std::vector<std::byte>> sources;
    std::vector<std::byte> stream;
    for (int i = 0; i < frame_count; ++i) {
        CAPTURE(i);
        const auto image = pattern.render(static_cast<std::uint64_t>(i) * 3);
        codec::bgrx_to_yuv420(image, yuv);
        inputs.push_back(yuv);
        sources.emplace_back(image.data.begin(), image.data.end());

        video::FrameOptions options;
        options.force_idr = i == 5;
        if (i == 8) {
            encoder->request_idr();
        }
        const bool expect_idr = i == 0 || i == 5 || i == 8;
        const auto frame = encoder->encode(yuv.view(), options);
        REQUIRE(frame.has_value());
        REQUIRE_FALSE(frame->bitstream.empty());
        CHECK(frame->idr == expect_idr);
        CHECK(frame->qp > 0);
        CHECK(frame->qp <= avc::max_qp);

        const auto units = h264::split_annex_b(frame->bitstream);
        REQUIRE(units.has_value());
        CHECK(units->front().type == h264::nal_type::aud);
        CHECK(h264::contains_idr(*units) == expect_idr);
        CHECK(has_nal(*units, h264::nal_type::sps) == expect_idr);
        CHECK(has_nal(*units, h264::nal_type::pps) == expect_idr);

        // The AVC420 stream as the RDPGFX channel would send it.
        const avc::Region region{
            .rect = {.left = 0, .top = 0, .right = surface_width, .bottom = surface_height},
            .quant = {.qp = frame->qp, .progressive = false, .quality = avc::quality_from_qp(frame->qp)},
        };
        const auto wrapped = avc::encode_avc420(std::span(&region, 1), frame->bitstream);
        const auto parsed = avc::decode_avc420(wrapped);
        REQUIRE(parsed.has_value());
        CHECK(std::ranges::equal(parsed->bitstream, frame->bitstream));

        stream.insert(stream.end(), frame->bitstream.begin(), frame->bitstream.end());
    }

    const std::string ffmpeg = env_or("FARLAND_FFMPEG", "ffmpeg");
    const std::string ffprobe = env_or("FARLAND_FFPROBE", "ffprobe");
    if (!have_tool(ffmpeg) || !have_tool(ffprobe)) {
        SKIP("ffmpeg or ffprobe not found: encoded, but not decoded");
    }

    const TempDir dir;
    const auto h264_path = dir.file("stream.h264");
    const auto yuv_path = dir.file("decoded.yuv");
    write_file(h264_path, stream);
    // No -pix_fmt: the raw frames keep the decoder's layout (yuvj420p), unconverted.
    REQUIRE(run({ffmpeg, "-v", "error", "-f", "h264", "-i", h264_path, "-f", "rawvideo", "-y", yuv_path}) == 0);
    const auto decoded = read_file(yuv_path);
    const std::size_t luma = std::size_t{config.width} * config.height;
    const std::size_t frame_size = luma * 3 / 2;
    REQUIRE(decoded.size() == frame_size * frame_count);

    double worst_yuv = 99;
    double worst_rgb = 99;
    double worst_subsampling = 99;
    std::vector<std::byte> rgb(std::size_t{surface_width} * surface_height * 4);
    std::vector<std::byte> reference(rgb.size());
    for (int i = 0; i < frame_count; ++i) {
        const auto bytes = std::span(decoded).subspan(static_cast<std::size_t>(i) * frame_size, frame_size);
        const codec::Yuv420View view{
            .y = bytes.first(luma),
            .u = bytes.subspan(luma, luma / 4),
            .v = bytes.subspan(luma + (luma / 4), luma / 4),
            .width = config.width,
            .height = config.height,
            .y_stride = config.width,
            .uv_stride = config.width / 2,
        };
        const auto input = inputs.at(static_cast<std::size_t>(i)).view();
        Psnr yuv_psnr;
        yuv_psnr.add(input.y, view.y);
        yuv_psnr.add(input.u, view.u);
        yuv_psnr.add(input.v, view.v);
        worst_yuv = std::min(worst_yuv, yuv_psnr.db());

        // What a client shows (FreeRDP's YUV->RGB, cropped to the surface),
        // against the same conversion of the picture before encoding. The
        // loss of 4:2:0 itself (sharp colour edges in the test pattern) is
        // not the encoder's and is only reported.
        codec::yuv420_to_bgrx(view, surface_width, surface_height, rgb);
        codec::yuv420_to_bgrx(input, surface_width, surface_height, reference);
        Psnr rgb_psnr;
        rgb_psnr.add(reference, rgb, 4);
        worst_rgb = std::min(worst_rgb, rgb_psnr.db());
        Psnr subsampling;
        subsampling.add(sources.at(static_cast<std::size_t>(i)), reference, 4);
        worst_subsampling = std::min(worst_subsampling, subsampling.db());
    }
    std::cout << video::to_string(backend) << ": worst PSNR " << worst_yuv << " dB (YUV), " << worst_rgb
              << " dB (RGB, visible area); BGRX->I420->BGRX alone " << worst_subsampling << " dB\n";
    CHECK(worst_yuv >= 35.0);
    CHECK(worst_rgb >= 35.0);

    // Frame types, profile and the VUI colour description.
    const auto probe_path = dir.file("probe.txt");
    REQUIRE(run({ffprobe, "-v", "error", "-f", "h264", "-show_entries",
                 "stream=profile,width,height,color_range,color_space:frame=key_frame,pict_type", "-of", "default=nw=1",
                 h264_path},
                probe_path) == 0);
    std::ifstream probe(probe_path);
    std::vector<std::string> key_frames;
    std::vector<std::string> pict_types;
    std::string profile;
    std::string color_range;
    std::string color_space;
    std::string width;
    std::string height;
    for (std::string line; std::getline(probe, line);) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const auto key = line.substr(0, eq);
        const auto value = line.substr(eq + 1);
        if (key == "key_frame") {
            key_frames.push_back(value);
        } else if (key == "pict_type") {
            pict_types.push_back(value);
        } else if (key == "profile") {
            profile = value;
        } else if (key == "color_range") {
            color_range = value;
        } else if (key == "color_space") {
            color_space = value;
        } else if (key == "width") {
            width = value;
        } else if (key == "height") {
            height = value;
        }
    }
    CHECK(key_frames == std::vector<std::string>{"1", "0", "0", "0", "0", "1", "0", "0", "1", "0", "0", "0"});
    CHECK(pict_types == std::vector<std::string>{"I", "P", "P", "P", "P", "I", "P", "P", "I", "P", "P", "P"});
    CHECK(profile == "Constrained Baseline");
    CHECK(color_range == "pc");
    CHECK(color_space == "bt709");
    CHECK(width == "320");
    CHECK(height == "240");
}

/// Rate control changes and a resize.
void check_reconfiguration(video::Backend backend)
{
    auto encoder = open_or_skip(backend, small_config());
    farland::server::TestPattern pattern(surface_width, surface_height);
    codec::Yuv420Frame yuv(encoder->config().width, encoder->config().height);
    auto encode = [&](std::uint64_t n) {
        codec::bgrx_to_yuv420(pattern.render(n), yuv);
        auto frame = encoder->encode(yuv.view());
        REQUIRE(frame.has_value());
        REQUIRE_FALSE(frame->bitstream.empty());
        return *frame;
    };
    CHECK(encode(0).idr);
    CHECK_FALSE(encode(1).idr);

    video::RateControl rate;
    rate.mode = video::RateControl::Mode::bitrate;
    // Generous for 320x240, so OpenH264's rate control does not drop pictures.
    rate.bitrate_kbps = 8000;
    REQUIRE(encoder->set_rate_control(rate).has_value());
    CHECK(encoder->config().rate == rate);
    static_cast<void>(encode(2));  // A mode change may re-open the encoder.

    rate.bitrate_kbps = 4000;
    REQUIRE(encoder->set_rate_control(rate).has_value());
    CHECK_FALSE(encode(3).idr);  // A new bitrate applies to the running stream.

    video::RateControl invalid;
    invalid.mode = video::RateControl::Mode::bitrate;
    const auto rejected = encoder->set_rate_control(invalid);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == Errc::invalid_value);

    auto bigger = small_config();
    bigger.width = 640;
    bigger.height = 480;
    REQUIRE(encoder->configure(bigger).has_value());
    pattern.resize(640, 480);
    yuv = codec::Yuv420Frame(640, 480);
    CHECK(encode(4).idr);
    CHECK_FALSE(encode(5).idr);
}

/// Coloured one-pixel strokes on coloured paper over the lower half of a
/// BGRX picture: the content 4:2:0 smears.
void paint_coloured_text(std::vector<std::byte>& bgrx, std::uint32_t width, std::uint32_t height, std::uint32_t phase)
{
    struct Bgr {
        std::uint8_t b, g, r;
    };
    const Bgr inks[] = {{40, 40, 255}, {120, 10, 10}, {250, 0, 250}, {255, 170, 0}};
    const Bgr papers[] = {{80, 235, 250}, {200, 60, 30}, {255, 255, 255}};
    for (std::uint32_t y = height / 2; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            // Only the left quarter changes from frame to frame, as when typing.
            const std::uint32_t cx = (x + (x < width / 4 ? phase : 0)) % 6;
            const std::uint32_t cy = y % 8;
            const bool stroke = (cx == 1 && cy < 7) || (cy == 3 && cx < 5) || (cx == 4 && cy == 6);
            const Bgr c = stroke ? inks[(y / 16) % 4] : papers[((x / 40) + (y / 16)) % 3];
            const std::size_t at = ((std::size_t{y} * width) + x) * 4;
            bgrx[at] = std::byte{c.b};
            bgrx[at + 1] = std::byte{c.g};
            bgrx[at + 2] = std::byte{c.r};
        }
    }
}

/// AVC444 through a real encoder: the main and auxiliary pictures form one
/// H.264 stream, which one decoder (ffmpeg) decodes as a client's would; the
/// client model (codec::apply_main_view/apply_aux_view per region, FreeRDP's
/// reverse filter) then rebuilds the 4:4:4 picture.
///
/// With FARLAND_AVC444_DUMP set to a directory, the bitmap streams and the
/// source pictures are written there, for decoding with FreeRDP's avc444_decompress
/// as an independent oracle.
void check_avc444(video::Backend backend, codec::Avc444Version version)
{
    const bool v2 = version == codec::Avc444Version::v2;
    const std::uint32_t width = v2 ? 320 : surface_width;  // v2 only at multiples of 32 (codec/yuv444.hpp)
    const std::uint32_t height = surface_height;
    auto config = small_config();
    config.width = avc::coded_size(width);
    config.height = avc::coded_size(height);
    config.reference_frames = 2;
    video::Avc444Encoder encoder(open_or_skip(backend, config), static_cast<std::uint16_t>(width),
                                 static_cast<std::uint16_t>(height), version);

    farland::server::TestPattern pattern(width, height);
    const avc::Rect16 whole{0, 0, static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height)};
    std::vector<std::vector<std::byte>> sources;
    std::vector<std::vector<std::byte>> messages;
    std::vector<codec::Yuv420Frame> inputs;  // the pictures given to the encoder, in stream order
    codec::Yuv444Frame scratch(config.width, config.height);
    std::vector<std::byte> stream;
    std::size_t main_bytes = 0;
    std::size_t aux_bytes = 0;
    std::size_t pictures = 0;
    for (int i = 0; i < frame_count; ++i) {
        const auto rendered = pattern.render(static_cast<std::uint64_t>(i) * 3);
        std::vector<std::byte> pixels(std::size_t{width} * height * 4);
        for (std::uint32_t y = 0; y < height; ++y) {
            std::ranges::copy(rendered.data.subspan(y * rendered.stride, std::size_t{width} * 4),
                              pixels.begin() + static_cast<std::ptrdiff_t>(std::size_t{y} * width * 4));
        }
        paint_coloured_text(pixels, width, height, static_cast<std::uint32_t>(i));
        const codec::ImageView view{.data = pixels, .width = width, .height = height, .stride = std::size_t{width} * 4};
        auto encoded = encoder.encode(view, std::span(&whole, 1));
        REQUIRE(encoded.has_value());
        REQUIRE(encoded->has_value());
        const auto& frame = **encoded;
        CHECK(frame.layout == avc::Avc444Layout::luma_and_chroma);  // coloured content everywhere
        CHECK(frame.idr == (i == 0));
        const auto parsed = avc::decode_avc444(frame.bitmap_stream);
        REQUIRE(parsed.has_value());
        codec::Yuv420Frame main_view(config.width, config.height);
        codec::Yuv420Frame aux_view(config.width, config.height);
        codec::bgrx_to_avc444(view, version, scratch, main_view, aux_view);
        inputs.push_back(std::move(main_view));
        if (parsed->second.has_value()) {
            inputs.push_back(std::move(aux_view));
        }
        stream.insert(stream.end(), parsed->first.bitstream.begin(), parsed->first.bitstream.end());
        main_bytes += parsed->first.bitstream.size();
        ++pictures;
        if (parsed->second.has_value()) {
            stream.insert(stream.end(), parsed->second->bitstream.begin(), parsed->second->bitstream.end());
            aux_bytes += parsed->second->bitstream.size();
            ++pictures;
        }
        messages.push_back(frame.bitmap_stream);
        sources.push_back(std::move(pixels));
    }

    if (const char* dump = std::getenv("FARLAND_AVC444_DUMP"); dump != nullptr && *dump != '\0') {
        const std::filesystem::path dir =
            std::filesystem::path(dump) / (std::string(video::to_string(backend)) + (v2 ? "-v2" : "-v1"));
        std::filesystem::create_directories(dir);
        std::ofstream meta(dir / "meta.txt");
        meta << (v2 ? 15 : 14) << ' ' << width << ' ' << height << ' ' << messages.size() << '\n';
        for (std::size_t i = 0; i < messages.size(); ++i) {
            write_file((dir / ("frame-" + std::to_string(i) + ".bin")).string(), messages[i]);
            write_file((dir / ("source-" + std::to_string(i) + ".bgrx")).string(), sources[i]);
        }
    }

    const std::string ffmpeg = env_or("FARLAND_FFMPEG", "ffmpeg");
    if (!have_tool(ffmpeg)) {
        SKIP("ffmpeg not found: encoded, but not decoded");
    }
    const TempDir dir;
    const auto h264_path = dir.file("avc444.h264");
    const auto yuv_path = dir.file("avc444.yuv");
    write_file(h264_path, stream);
    REQUIRE(run({ffmpeg, "-v", "error", "-f", "h264", "-i", h264_path, "-f", "rawvideo", "-y", yuv_path}) == 0);
    const auto decoded = read_file(yuv_path);
    const std::size_t luma = std::size_t{config.width} * config.height;
    const std::size_t picture_size = luma * 3 / 2;
    REQUIRE(decoded.size() == picture_size * pictures);  // one picture per access unit, in order

    std::size_t next = 0;
    const auto picture = [&] {
        REQUIRE(next < pictures);
        const auto bytes = std::span(decoded).subspan(next++ * picture_size, picture_size);
        return codec::Yuv420View{
            .y = bytes.first(luma),
            .u = bytes.subspan(luma, luma / 4),
            .v = bytes.subspan(luma + (luma / 4), luma / 4),
            .width = config.width,
            .height = config.height,
            .y_stride = config.width,
            .uv_stride = config.width / 2,
        };
    };
    // Coding loss of each view: decoded picture against encoder input.
    double worst_view = 99;
    for (std::size_t k = 0; k < pictures; ++k) {
        const auto bytes = std::span(decoded).subspan(k * picture_size, picture_size);
        const auto input = inputs.at(k).view();
        Psnr p;
        p.add(input.y, bytes.first(luma));
        p.add(input.u, bytes.subspan(luma, luma / 4));
        p.add(input.v, bytes.subspan(luma + (luma / 4), luma / 4));
        worst_view = std::min(worst_view, p.db());
    }
    codec::Yuv444Frame client(config.width, config.height);
    double worst_freerdp = 99;
    double worst_reverse = 99;
    double best_420 = 0;
    std::vector<std::byte> shown(std::size_t{width} * height * 4);
    codec::Yuv420Frame i420(config.width, config.height);
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const auto parsed = avc::decode_avc444(messages[i]).value();
        if (parsed.layout != avc::Avc444Layout::chroma) {
            const auto main = picture();
            for (const auto& region : parsed.first.regions) {
                codec::apply_main_view(main, region.rect, client);
            }
        }
        const auto* chroma = parsed.layout == avc::Avc444Layout::luma_and_chroma ? &*parsed.second
                             : parsed.layout == avc::Avc444Layout::chroma        ? &parsed.first
                                                                                 : nullptr;
        if (chroma != nullptr) {
            const auto aux = picture();
            for (const auto& region : chroma->regions) {
                codec::apply_aux_view(aux, version, region.rect, client);
            }
        }
        for (const auto filter : {codec::ChromaFilter::freerdp, codec::ChromaFilter::reverse}) {
            codec::yuv444_to_bgrx(client.view(), width, height, filter, shown);
            Psnr p;
            p.add(sources[i], shown, 4);
            (filter == codec::ChromaFilter::freerdp ? worst_freerdp : worst_reverse) =
                std::min(filter == codec::ChromaFilter::freerdp ? worst_freerdp : worst_reverse, p.db());
        }
        // What 4:2:0 could show at best: the source through I420, uncoded.
        const codec::ImageView source{
            .data = sources[i], .width = width, .height = height, .stride = std::size_t{width} * 4};
        codec::bgrx_to_yuv420(source, i420);
        codec::yuv420_to_bgrx(i420.view(), width, height, shown);
        Psnr p420;
        p420.add(sources[i], shown, 4);
        best_420 = std::max(best_420, p420.db());
    }
    CHECK(next == pictures);
    std::cout << video::to_string(backend) << (v2 ? " AVC444v2" : " AVC444") << ": views coded at >= " << worst_view
              << " dB (YUV); worst PSNR " << worst_freerdp << " dB (FreeRDP filter), " << worst_reverse
              << " dB (always reversed); uncoded 4:2:0 at best " << best_420 << " dB; " << main_bytes / messages.size()
              << " + " << aux_bytes / messages.size() << " bytes/frame (main + auxiliary)\n";
    CHECK(worst_view >= 35.0);
    CHECK(worst_freerdp >= 30.0);
    CHECK(worst_freerdp > best_420 + 3.0);
}

}  // namespace

TEST_CASE("OpenH264 encodes AVC444 that one decoder turns back into 4:4:4")
{
    check_avc444(video::Backend::openh264, codec::Avc444Version::v1);
    check_avc444(video::Backend::openh264, codec::Avc444Version::v2);
}

TEST_CASE("x264 encodes AVC444 that one decoder turns back into 4:4:4")
{
    check_avc444(video::Backend::x264, codec::Avc444Version::v1);
    check_avc444(video::Backend::x264, codec::Avc444Version::v2);
}

TEST_CASE("H.264 encoder configurations are validated")
{
    auto config = small_config();
    CHECK(video::validate(config).has_value());

    auto bad = config;
    bad.width = 318;
    CHECK(video::validate(bad).error().code == Errc::invalid_value);
    bad = config;
    bad.height = 8208;
    CHECK(video::validate(bad).error().code == Errc::invalid_value);
    bad = config;
    bad.fps = 0;
    CHECK(video::validate(bad).error().code == Errc::invalid_value);
    bad = config;
    bad.threads = 0;
    CHECK(video::validate(bad).error().code == Errc::invalid_value);
    bad = config;
    bad.rate.quality = 52;
    CHECK(video::validate(bad).error().code == Errc::invalid_value);
    bad = config;
    bad.rate.mode = video::RateControl::Mode::bitrate;
    CHECK(video::validate(bad).error().code == Errc::invalid_value);
    bad.rate.bitrate_kbps = 5000;
    CHECK(video::validate(bad).has_value());
    bad.rate.max_bitrate_kbps = 4000;
    CHECK(video::validate(bad).error().code == Errc::invalid_value);
}

TEST_CASE("H.264 backends have names")
{
    REQUIRE_FALSE(video::compiled_backends().empty());
#if defined(FARLAND_HAVE_NVENC)
    CHECK(video::compiled_backends().front() == video::Backend::nvenc);
#elif defined(FARLAND_HAVE_VAAPI)
    CHECK(video::compiled_backends().front() == video::Backend::vaapi);
#else
    CHECK(video::compiled_backends().front() == video::Backend::openh264);
#endif
    for (const auto backend :
         {video::Backend::openh264, video::Backend::x264, video::Backend::vaapi, video::Backend::nvenc}) {
        CHECK(video::parse_backend(video::to_string(backend)) == backend);
    }
    CHECK_FALSE(video::parse_backend("qsv").has_value());
}

TEST_CASE("A missing OpenH264 library is a clean error")
{
    const auto missing = video::create_encoder(video::Backend::openh264, small_config(),
                                               {.openh264_library = "/nonexistent/libopenh264.so.8"});
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == Errc::unsupported);

#if defined(__APPLE__)
    const char* not_openh264 = "/usr/lib/libz.1.dylib";
#else
    const char* not_openh264 = "libc.so.6";
#endif
    const auto wrong =
        video::create_encoder(video::Backend::openh264, small_config(), {.openh264_library = not_openh264});
    REQUIRE_FALSE(wrong.has_value());
    CHECK(wrong.error().code == Errc::unsupported);
}

TEST_CASE("The H.264 factory tries backends in order")
{
    const video::BackendOptions no_openh264{.openh264_library = "/nonexistent/libopenh264.so.8"};
    const auto none = video::create_encoder(std::span<const video::Backend>{}, small_config(), no_openh264);
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().code == Errc::unsupported);

    auto bad = small_config();
    bad.width = 100;
    const auto invalid = video::create_encoder(video::compiled_backends(), bad, no_openh264);
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().code == Errc::invalid_value);

    const bool x264_built =
        std::ranges::find(video::compiled_backends(), video::Backend::x264) != video::compiled_backends().end();
    if (!x264_built) {
        const auto x264 = video::create_encoder(video::Backend::x264, small_config());
        REQUIRE_FALSE(x264.has_value());
        CHECK(x264.error().code == Errc::unsupported);
        const std::array order{video::Backend::x264, video::Backend::openh264};
        CHECK_FALSE(video::create_encoder(order, small_config(), no_openh264).has_value());
    }
}

TEST_CASE("OpenH264 encodes decodable AVC420 access units")
{
    check_backend(video::Backend::openh264);
}

TEST_CASE("x264 encodes decodable AVC420 access units")
{
    check_backend(video::Backend::x264);
}

TEST_CASE("VA-API encodes decodable AVC420 access units")
{
    check_backend(video::Backend::vaapi);
}

TEST_CASE("VA-API changes rate control and size")
{
    check_reconfiguration(video::Backend::vaapi);
}

TEST_CASE("NVENC encodes decodable AVC420 access units")
{
    check_backend(video::Backend::nvenc);
}

TEST_CASE("NVENC changes rate control and size")
{
    check_reconfiguration(video::Backend::nvenc);
}

TEST_CASE("OpenH264 changes rate control and size")
{
    check_reconfiguration(video::Backend::openh264);
}

TEST_CASE("x264 changes rate control and size")
{
    check_reconfiguration(video::Backend::x264);
}

TEST_CASE("H.264 encode latency at 1080p", "[.][benchmark]")
{
    constexpr std::uint32_t width = 1920;
    constexpr std::uint32_t height = 1080;
    constexpr int frames = 120;
    const auto threads = static_cast<std::uint32_t>(std::stoul(env_or("FARLAND_BENCH_THREADS", "1")));

    // Scrolling high-contrast texture: every macroblock changes every frame.
    std::vector<std::byte> motion(std::size_t{width} * height * 4);
    auto render_motion = [&](int t) {
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::uint32_t u = x + (static_cast<std::uint32_t>(t) * 5);
                const std::size_t at = ((std::size_t{y} * width) + x) * 4;
                motion[at] = static_cast<std::byte>((y * 2) + ((u / 8) * 16));
                motion[at + 1] = static_cast<std::byte>(u ^ y);
                motion[at + 2] = static_cast<std::byte>((u * 3) + y);
                motion[at + 3] = std::byte{0xFF};
            }
        }
        return codec::ImageView{.data = motion, .width = width, .height = height, .stride = std::size_t{width} * 4};
    };

    for (const auto backend : video::compiled_backends()) {
        video::EncoderConfig config;
        config.width = avc::coded_size(width);
        config.height = avc::coded_size(height);
        config.fps = 60;
        config.threads = threads;
        config.rate.quality = 23;
        auto created = video::create_encoder(backend, config, backend_options());
        if (!created.has_value()) {
            std::cout << video::to_string(backend) << ": " << created.error().message() << '\n';
            continue;
        }
        auto& encoder = **created;
        farland::server::TestPattern pattern(width, height);
        codec::Yuv420Frame yuv(config.width, config.height);

        for (const bool full_motion : {false, true}) {
            using clock = std::chrono::steady_clock;
            double convert_ms = 0;
            double encode_ms = 0;
            double worst_ms = 0;
            double idr_ms = 0;
            std::size_t bytes = 0;
            encoder.request_idr();
            for (int i = 0; i < frames; ++i) {
                const auto image = full_motion ? render_motion(i) : pattern.render(static_cast<std::uint64_t>(i));
                const auto t0 = clock::now();
                codec::bgrx_to_yuv420(image, yuv);
                const auto t1 = clock::now();
                const auto frame = encoder.encode(yuv.view());
                const auto t2 = clock::now();
                REQUIRE(frame.has_value());
                const double ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
                if (i == 0) {
                    idr_ms = ms;
                    continue;
                }
                convert_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
                encode_ms += ms;
                worst_ms = std::max(worst_ms, ms);
                bytes += frame->bitstream.size();
            }
            const double n = frames - 1;
            std::cout << video::to_string(backend) << " 1920x1080 threads=" << threads
                      << (full_motion ? " full motion" : " test pattern") << ": IDR " << idr_ms << " ms, P mean "
                      << encode_ms / n << " ms, max " << worst_ms << " ms, " << bytes / (frames - 1)
                      << " bytes/frame; BGRX->I420 " << convert_ms / n << " ms\n";
        }
    }
    SUCCEED();
}
