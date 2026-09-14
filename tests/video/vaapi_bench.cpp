// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// farland-vaapi-bench: CPU time and latency of VA-API encoding, for the M5
// exit criterion (4K30 with less than 10% of one core, docs/ROADMAP.md).
// Not part of `meson test`.
//
// It encodes pre-rendered BGRX dmabufs (VA surfaces exported from a second
// device handle, as a compositor would hand them over) paced at --fps, and
// reports the process CPU time per frame (all threads, libva and driver
// included) and the time each frame takes. --cpu adds the path without
// dmabufs: BGRX->I420 on the CPU, upload, encode.
//
//   farland-vaapi-bench [--device /dev/dri/renderD128] [--width 3840] [--height 2160]
//                       [--fps 30] [--frames 300] [--rate cqp:23|cbr:KBPS|vbr:KBPS]
//                       [--profile baseline|main|high] [--unpaced] [--cpu]

#include <farland/codec/avc420.hpp>
#include <farland/codec/image.hpp>
#include <farland/codec/yuv.hpp>
#include <farland/video/vaapi_encoder.hpp>

#include "vaapi_test_source.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace video = farland::video;
namespace codec = farland::codec;

namespace {

struct Options {
    std::string device;
    std::uint32_t width = 3840;
    std::uint32_t height = 2160;
    std::uint32_t fps = 30;
    int frames = 300;
    video::RateControl rate;
    video::Profile profile = video::Profile::constrained_baseline;
    bool paced = true;
    bool cpu = false;
};

double process_cpu_ms()
{
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (static_cast<double>(ts.tv_sec) * 1e3) + (static_cast<double>(ts.tv_nsec) / 1e6);
}

/// Frame `t` of a scrolling, high-contrast texture over a desktop-like
/// background: every macroblock changes every frame.
void render(std::vector<std::byte>& pixels, std::uint32_t width, std::uint32_t height, std::uint32_t t)
{
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t u = x + (t * 7);
            const std::size_t at = ((std::size_t{y} * width) + x) * 4;
            pixels[at] = static_cast<std::byte>((y / 4) + ((u / 16) * 24));
            pixels[at + 1] = static_cast<std::byte>((u ^ y) & 0xF0U);
            pixels[at + 2] = static_cast<std::byte>((u * 3) + (y / 2));
            pixels[at + 3] = std::byte{0xFF};
        }
    }
}

struct Stats {
    std::vector<double> wall_ms;
    double cpu_ms = 0;
    std::size_t bytes = 0;
    std::size_t idr_bytes = 0;

    void print(std::string_view name, const Options& options) const
    {
        auto sorted = wall_ms;
        std::ranges::sort(sorted);
        double mean = 0;
        for (const double ms : sorted) {
            mean += ms;
        }
        const auto n = static_cast<double>(sorted.size());
        mean /= n;
        const double cpu_per_frame = cpu_ms / n;
        std::cout << name << ": CPU " << cpu_per_frame << " ms/frame = " << (cpu_per_frame * options.fps / 10.0)
                  << "% of one core at " << options.fps << " fps; latency mean " << mean << " ms, p99 "
                  << sorted[static_cast<std::size_t>(n * 0.99)] << " ms, max " << sorted.back() << " ms; "
                  << (static_cast<double>(bytes) * 8.0 * options.fps / n / 1000.0) << " kbit/s (IDR "
                  << idr_bytes / 1024 << " KiB)\n";
    }
};

bool parse(int argc, char** argv, Options& options)
{
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto value = [&]() -> std::string {
            return i + 1 < args.size() ? std::string(args[++i]) : std::string();
        };
        if (args[i] == "--device") {
            options.device = value();
        } else if (args[i] == "--width") {
            options.width = static_cast<std::uint32_t>(std::stoul(value()));
        } else if (args[i] == "--height") {
            options.height = static_cast<std::uint32_t>(std::stoul(value()));
        } else if (args[i] == "--fps") {
            options.fps = static_cast<std::uint32_t>(std::stoul(value()));
        } else if (args[i] == "--frames") {
            options.frames = std::stoi(value());
        } else if (args[i] == "--rate") {
            const auto rate = value();
            const auto colon = rate.find(':');
            const auto kind = rate.substr(0, colon);
            const auto number = static_cast<std::uint32_t>(std::stoul(rate.substr(colon + 1)));
            if (kind == "cqp") {
                options.rate.quality = static_cast<std::uint8_t>(number);
            } else {
                options.rate.mode = video::RateControl::Mode::bitrate;
                options.rate.bitrate_kbps = number;
                options.rate.max_bitrate_kbps = kind == "vbr" ? number * 2 : 0;
            }
        } else if (args[i] == "--profile") {
            const auto name = value();
            options.profile = name == "high"   ? video::Profile::high
                              : name == "main" ? video::Profile::main
                                               : video::Profile::constrained_baseline;
        } else if (args[i] == "--unpaced") {
            options.paced = false;
        } else if (args[i] == "--cpu") {
            options.cpu = true;
        } else {
            std::cerr << "unknown option " << args[i] << "; see the comment at the top of vaapi_bench.cpp\n";
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parse(argc, argv, options)) {
        return 2;
    }
    const auto info = video::vaapi::probe(options.device);
    if (!info) {
        std::cerr << "no VA-API device: " << info.error().message() << '\n';
        return 1;
    }
    std::cout << video::vaapi::describe(*info) << '\n';

    video::EncoderConfig config;
    config.width = codec::avc::coded_size(options.width);
    config.height = codec::avc::coded_size(options.height);
    config.fps = options.fps;
    config.rate = options.rate;
    config.profile = options.profile;
    auto created = video::vaapi::create(config, {.render_node = info->render_node});
    if (!created) {
        std::cerr << "cannot create the encoder: " << created.error().message() << '\n';
        return 1;
    }
    auto& encoder = **created;

    // Eight different frames, rendered and uploaded before timing starts.
    constexpr std::uint32_t distinct = 8;
    std::vector<std::byte> pixels(std::size_t{options.width} * options.height * 4);
    std::vector<std::unique_ptr<farland::test::DmabufSource>> sources;
    std::vector<std::vector<std::byte>> images;
    for (std::uint32_t i = 0; i < distinct; ++i) {
        render(pixels, options.width, options.height, i);
        const codec::ImageView image{
            .data = pixels, .width = options.width, .height = options.height, .stride = std::size_t{options.width} * 4};
        auto source = farland::test::make_va_source(info->render_node, options.width, options.height);
        if (source == nullptr || !source->write(image)) {
            std::cerr << "cannot create a BGRX dmabuf through VA-API\n";
            return 1;
        }
        sources.push_back(std::move(source));
        images.push_back(pixels);
    }
    std::cout << "source dmabufs: modifier 0x" << std::hex << sources.front()->frame().modifier << std::dec << ", "
              << sources.front()->frame().plane_count << " plane(s)\n";

    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(1'000'000'000LL / options.fps);
    auto run = [&](bool dmabuf) -> Stats {
        Stats stats;
        codec::Yuv420Frame yuv(config.width, config.height);
        encoder.request_idr();
        auto deadline = clock::now();
        for (int i = 0; i < options.frames + 1; ++i) {
            if (options.paced) {
                std::this_thread::sleep_until(deadline);
                deadline += period;
            }
            const auto index = static_cast<std::size_t>(i) % distinct;
            const double cpu0 = process_cpu_ms();
            const auto t0 = clock::now();
            video::EncodedFrame frame;
            if (dmabuf) {
                auto encoded = encoder.encode_dmabuf(sources[index]->frame(), {});
                if (!encoded) {
                    std::cerr << "encode_dmabuf: " << encoded.error().message() << '\n';
                    std::exit(1);
                }
                frame = std::move(*encoded);
            } else {
                const codec::ImageView image{.data = images[index],
                                             .width = options.width,
                                             .height = options.height,
                                             .stride = std::size_t{options.width} * 4};
                codec::bgrx_to_yuv420(image, yuv);
                auto encoded = encoder.encode(yuv.view(), {});
                if (!encoded) {
                    std::cerr << "encode: " << encoded.error().message() << '\n';
                    std::exit(1);
                }
                frame = std::move(*encoded);
            }
            const auto t1 = clock::now();
            const double cpu1 = process_cpu_ms();
            if (i == 0) {
                stats.idr_bytes = frame.bitstream.size();
                continue;  // The first IDR sets things up; not counted.
            }
            stats.wall_ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            stats.cpu_ms += cpu1 - cpu0;
            stats.bytes += frame.bitstream.size();
        }
        return stats;
    };

    std::cout << options.width << "x" << options.height << " (coded " << config.width << "x" << config.height << "), "
              << options.frames << " frames" << (options.paced ? " paced at " : " unpaced, nominal ") << options.fps
              << " fps\n";
    if (encoder.accepts_dmabuf()) {
        run(true).print("dmabuf -> VPP -> encode", options);
    } else {
        std::cout << "the driver has no video processing: no dmabuf path\n";
    }
    if (options.cpu) {
        run(false).print("CPU BGRX->I420 -> upload -> encode", options);
    }
    return 0;
}
