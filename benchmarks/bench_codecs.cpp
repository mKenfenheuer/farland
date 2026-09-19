// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The M5 exit benchmark (docs/ROADMAP.md): bitrate, CPU and quality of every
// GFX codec over a content corpus, and the "text is sharp at 2 Mbit/s" check.
//
// The corpus is generated, so the numbers are reproducible on any machine and
// need no sample files:
//   text     a dense page of small dark glyphs on white, the hard case for a
//            lossy codec and the one the 2 Mbit/s target is about;
//   ui       flat panels, window chrome and labels: a desktop at rest;
//   photo    gradients, soft blobs and mild noise;
//   mixed    a desktop with a photo in a window and text around it;
//   video    the photo panned and lit differently every frame.
//
// Each codec encodes the corpus the way the graphics pipeline drives it and
// is measured on
//   bitrate  bytes per frame, and kbit/s at 30 frames per second;
//   CPU      milliseconds of one core per frame, encoding only;
//   latency  the time until the client can show the first complete picture
//            of a frame (for the refining pipeline: until the last upgrade);
//   quality  PSNR of the decoded picture against the source, over the whole
//            frame and over the text pixels alone, where a decoder exists.
//
// Not part of `meson test`. Build and run in a release build:
//   meson compile -C build bench-codecs && ./build/benchmarks/bench-codecs

#include <farland/codec/clear.hpp>
#include <farland/codec/image.hpp>
#include <farland/codec/planar.hpp>
#include <farland/codec/progressive.hpp>
#include <farland/codec/rfx_common.hpp>
#include <farland/codec/yuv.hpp>
#include <farland/video/h264_encoder.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace progressive = farland::codec::progressive;
namespace clear = farland::codec::clear;
namespace planar = farland::codec::planar;
namespace video = farland::video;
using farland::codec::ImageView;

constexpr std::uint32_t width = 1920;
constexpr std::uint32_t height = 1080;
constexpr std::uint32_t tile = 64;
constexpr std::uint32_t tiles_x = (width + tile - 1) / tile;
constexpr std::uint32_t tiles_y = (height + tile - 1) / tile;
constexpr std::size_t frame_bytes = std::size_t{width} * height * 4;
constexpr std::uint32_t fps = 30;
/// The pipeline's per-frame refinement budget (server::PipelineOptions).
constexpr std::size_t upgrade_budget = 16 * 1024;
/// The exit criterion: text stays sharp at this rate.
constexpr double text_target_kbps = 2000.0;

struct Image {
    std::vector<std::byte> data = std::vector<std::byte>(frame_bytes);
    /// Pixels that belong to a glyph: where sharpness is judged.
    std::vector<bool> text = std::vector<bool>(std::size_t{width} * height, false);

    [[nodiscard]] ImageView view() const
    {
        return {.data = data, .width = width, .height = height, .stride = std::size_t{width} * 4};
    }
    void set(std::uint32_t x, std::uint32_t y, int r, int g, int b, bool glyph = false)
    {
        const std::size_t at = ((std::size_t{y} * width) + x);
        data[at * 4] = static_cast<std::byte>(std::clamp(b, 0, 255));
        data[(at * 4) + 1] = static_cast<std::byte>(std::clamp(g, 0, 255));
        data[(at * 4) + 2] = static_cast<std::byte>(std::clamp(r, 0, 255));
        data[(at * 4) + 3] = std::byte{0xFF};
        text[at] = glyph;
    }
};

/// A deterministic 32-bit noise source.
struct Noise {
    std::uint32_t state = 1;
    int operator()(int amplitude)
    {
        state = (state * 1664525U) + 1013904223U;
        return static_cast<int>((state >> 24U) % static_cast<std::uint32_t>((2 * amplitude) + 1)) - amplitude;
    }
};

/// A 5 x 9 bitmap alphabet, one row of five bits per line. Real text reuses
/// a few dozen shapes over and over, which is what a text codec's caches are
/// built for; random dots per pixel would be noise, not text.
constexpr std::size_t glyph_count = 16;
constexpr std::array<std::array<std::uint8_t, 9>, glyph_count> alphabet{{
    {0x0E, 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11, 0x00},  // A
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x11, 0x1E, 0x00},  // B
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x10, 0x11, 0x0E, 0x00},  // C
    {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E, 0x00},  // D
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10, 0x1F, 0x00},  // E
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10, 0x10, 0x00},  // F
    {0x0E, 0x11, 0x10, 0x10, 0x13, 0x11, 0x11, 0x0F, 0x00},  // G
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11, 0x11, 0x00},  // H
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E, 0x00},  // I
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11, 0x11, 0x00},  // K
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F, 0x00},  // L
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11, 0x11, 0x00},  // M
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E, 0x00},  // O
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10, 0x10, 0x00},  // P
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00},  // T
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},  // space
}};

/// Draws a line of glyphs from the alphabet, `seed` choosing the letters: the
/// shapes a text codec has to keep, laid out as real text is.
void draw_text_line(Image& img, std::uint32_t x0, std::uint32_t y0, std::uint32_t chars, int ink, int paper,
                    std::uint32_t seed)
{
    std::uint32_t bits = seed | 1U;
    for (std::uint32_t c = 0; c < chars; ++c) {
        bits = (bits * 1103515245U) + 12345U;
        const auto& glyph = alphabet.at((bits >> 16U) % glyph_count);
        for (std::uint32_t gy = 0; gy < 9; ++gy) {
            for (std::uint32_t gx = 0; gx < 5; ++gx) {
                const bool on = ((glyph.at(gy) >> (4U - gx)) & 1U) != 0;
                const std::uint32_t x = x0 + (c * 7) + gx;
                const std::uint32_t y = y0 + gy;
                if (x >= width || y >= height) {
                    continue;
                }
                if (on) {
                    img.set(x, y, ink, ink, ink, true);
                } else {
                    img.set(x, y, paper, paper, paper, false);
                }
            }
        }
    }
}

Image text_page()
{
    Image img;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            img.set(x, y, 250, 250, 248);
        }
    }
    std::uint32_t seed = 7;
    for (std::uint32_t y = 12; y + 12 < height; y += 15) {
        draw_text_line(img, 24, y, (width - 60) / 7, 25, 250, seed);
        seed = (seed * 2654435761U) + 1;
    }
    return img;
}

Image ui_page()
{
    Image img;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const bool chrome = y < 34 || (x % 480) < 3;
            const bool panel = ((x / 480) + (y / 270)) % 2 == 0;
            const int v = chrome ? 60 : (panel ? 244 : 232);
            img.set(x, y, v, v, chrome ? v + 8 : v);
        }
    }
    std::uint32_t seed = 11;
    for (std::uint32_t y = 60; y + 12 < height; y += 42) {
        draw_text_line(img, 40, y, 48, 40, 244, seed);
        seed = (seed * 2246822519U) + 3;
    }
    return img;
}

Image photo(double pan = 0.0, int lift = 0)
{
    Image img;
    Noise noise;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const double fx = (static_cast<double>(x) / width) + pan;
            const double fy = static_cast<double>(y) / height;
            const double wave = std::sin((fx * 9.0) + (fy * 4.0)) * 40.0;
            const double dx = fx - 0.6;
            const double dy = fy - 0.4;
            const double blob = std::exp(-((dx * dx) + (dy * dy)) * 18.0) * 90.0;
            img.set(x, y, static_cast<int>(60 + (fx * 150) + wave) + noise(4) + lift,
                    static_cast<int>(90 + (fy * 100) + blob) + noise(4) + lift,
                    static_cast<int>(140 - (fx * 60) + blob) + noise(4) + lift);
        }
    }
    return img;
}

/// A desktop: UI and text with a photo in a window in the middle.
Image mixed_desktop(double pan = 0.0)
{
    Image img = ui_page();
    const Image picture = photo(pan);
    constexpr std::uint32_t x0 = 640;
    constexpr std::uint32_t y0 = 320;
    constexpr std::uint32_t w = 640;
    constexpr std::uint32_t h = 384;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t from = (((std::size_t{y} * 2) * width) + (std::size_t{x} * 2)) * 4;
            img.set(x0 + x, y0 + y, std::to_integer<int>(picture.data[from + 2]),
                    std::to_integer<int>(picture.data[from + 1]), std::to_integer<int>(picture.data[from]));
        }
    }
    return img;
}

double psnr(const Image& source, std::span<const std::byte> decoded, bool text_only)
{
    double sum = 0;
    std::size_t count = 0;
    for (std::size_t p = 0; p < std::size_t{width} * height; ++p) {
        if (text_only && !source.text[p]) {
            continue;
        }
        for (std::size_t c = 0; c < 3; ++c) {
            const auto a = std::to_integer<int>(source.data[(p * 4) + c]);
            const auto b = std::to_integer<int>(decoded[(p * 4) + c]);
            const double d = a - b;
            sum += d * d;
            ++count;
        }
    }
    if (count == 0) {
        return 0;
    }
    const double mse = sum / static_cast<double>(count);
    return mse <= 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

struct Result {
    std::string codec;
    double bytes_per_frame = 0;
    double encode_ms = 0;
    double latency_ms = 0;  ///< until the client has the complete picture
    double psnr_all = 0;
    double psnr_text = 0;
    bool lossless = false;
    bool has_psnr = true;
};

double now_ms()
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::size_t total(const std::vector<std::vector<std::byte>>& streams)
{
    std::size_t n = 0;
    for (const auto& s : streams) {
        n += s.size();
    }
    return n;
}

/// Progressive in one pass, the profile a client without refinement gets.
Result bench_progressive_single(const Image& img, const farland::codec::rfx::Quant& quant, std::string_view label)
{
    const std::array full{progressive::Rect{.x = 0, .y = 0, .width = width, .height = height}};
    progressive::Encoder encoder(width, height, {.quant = quant, .reduce_extrapolate = true});
    const double t0 = now_ms();
    const auto streams = encoder.encode(img.view(), full);
    const double encode_ms = now_ms() - t0;

    auto decoder = progressive::Decoder::create(width, height);
    for (const auto& s : streams) {
        static_cast<void>(decoder->decode(s, 1));
    }
    return {.codec = std::string(label),
            .bytes_per_frame = static_cast<double>(total(streams)),
            .encode_ms = encode_ms,
            .latency_ms = encode_ms,
            .psnr_all = psnr(img, decoder->image().data, false),
            .psnr_text = psnr(img, decoder->image().data, true)};
}

/// Progressive as the pipeline drives it: a coarse first pass, then upgrades
/// of at most `upgrade_budget` bytes per frame until every tile is at full
/// quality. Latency is the time until that point at `fps`.
Result bench_progressive_refined(const Image& img, const farland::codec::rfx::Quant& quant)
{
    const std::array full{progressive::Rect{.x = 0, .y = 0, .width = width, .height = height}};
    progressive::Encoder encoder(width, height, {.quant = quant, .reduce_extrapolate = true, .refine = true});
    auto decoder = progressive::Decoder::create(width, height);
    const double t0 = now_ms();
    auto streams = encoder.encode(img.view(), full);
    std::size_t bytes = total(streams);
    std::uint32_t frame = 1;
    for (const auto& s : streams) {
        static_cast<void>(decoder->decode(s, frame));
    }
    const double first_ms = now_ms() - t0;
    const auto first_pass_bytes = static_cast<double>(bytes);
    std::uint32_t frames = 1;
    while (encoder.pending_tiles() > 0 && frames < 600) {
        ++frames;
        ++frame;
        streams = encoder.upgrade(upgrade_budget);
        if (streams.empty()) {
            break;
        }
        bytes += total(streams);
        for (const auto& s : streams) {
            static_cast<void>(decoder->decode(s, frame));
        }
    }
    const double encode_ms = now_ms() - t0;
    std::printf("      (refinement: %u frames, %.0f KB of upgrades on top of a %.0f KB first pass"
                " that cost %.1f ms)\n",
                frames, (static_cast<double>(bytes) - first_pass_bytes) / 1024.0, first_pass_bytes / 1024.0, first_ms);
    // Bytes and CPU are what the whole repaint costs, first pass and upgrades
    // together; latency is when the client has the complete picture.
    return {.codec = "progressive refined (first pass + upgrades)",
            .bytes_per_frame = static_cast<double>(bytes),
            .encode_ms = encode_ms,
            .latency_ms = static_cast<double>(frames - 1) * 1000.0 / fps,
            .psnr_all = psnr(img, decoder->image().data, false),
            .psnr_text = psnr(img, decoder->image().data, true)};
}

/// ClearCodec over every tile, as mixed mode sends text and UI.
Result bench_clear(const Image& img)
{
    clear::Encoder encoder;
    clear::Decoder decoder;
    std::vector<std::byte> out(frame_bytes, std::byte{0});
    std::size_t bytes = 0;
    const double t0 = now_ms();
    std::vector<std::vector<std::byte>> streams;
    streams.reserve(std::size_t{tiles_x} * tiles_y);
    for (std::uint32_t ty = 0; ty < tiles_y; ++ty) {
        for (std::uint32_t tx = 0; tx < tiles_x; ++tx) {
            const std::uint32_t x = tx * tile;
            const std::uint32_t y = ty * tile;
            const std::uint32_t w = std::min(tile, width - x);
            const std::uint32_t h = std::min(tile, height - y);
            const std::size_t offset = ((std::size_t{y} * width) + x) * 4;
            const ImageView region{img.view().data.subspan(offset, (((std::size_t{h} - 1) * width) + w) * 4), w, h,
                                   std::size_t{width} * 4};
            streams.push_back(encoder.encode(region));
            bytes += streams.back().size();
        }
    }
    const double encode_ms = now_ms() - t0;
    std::size_t i = 0;
    for (std::uint32_t ty = 0; ty < tiles_y; ++ty) {
        for (std::uint32_t tx = 0; tx < tiles_x; ++tx) {
            const std::uint32_t x = tx * tile;
            const std::uint32_t y = ty * tile;
            const std::uint32_t w = std::min(tile, width - x);
            const std::uint32_t h = std::min(tile, height - y);
            const std::size_t offset = ((std::size_t{y} * width) + x) * 4;
            static_cast<void>(decoder.decode(streams[i++], w, h,
                                             std::span(out).subspan(offset, (((std::size_t{h} - 1) * width) + w) * 4),
                                             std::size_t{width} * 4));
        }
    }
    return {.codec = "clearcodec (lossless)",
            .bytes_per_frame = static_cast<double>(bytes),
            .encode_ms = encode_ms,
            .latency_ms = encode_ms,
            .psnr_all = psnr(img, out, false),
            .psnr_text = psnr(img, out, true),
            .lossless = true};
}

Result bench_planar(const Image& img)
{
    std::vector<std::byte> out(frame_bytes, std::byte{0});
    std::size_t bytes = 0;
    std::vector<std::vector<std::byte>> streams;
    streams.reserve(std::size_t{tiles_x} * tiles_y);
    const double t0 = now_ms();
    for (std::uint32_t ty = 0; ty < tiles_y; ++ty) {
        for (std::uint32_t tx = 0; tx < tiles_x; ++tx) {
            const std::uint32_t x = tx * tile;
            const std::uint32_t y = ty * tile;
            const std::uint32_t w = std::min(tile, width - x);
            const std::uint32_t h = std::min(tile, height - y);
            const std::size_t offset = ((std::size_t{y} * width) + x) * 4;
            const ImageView region{img.view().data.subspan(offset, (((std::size_t{h} - 1) * width) + w) * 4), w, h,
                                   std::size_t{width} * 4};
            streams.push_back(planar::encode(region, {planar::Mode::automatic, planar::Orientation::top_down}));
            bytes += streams.back().size();
        }
    }
    const double encode_ms = now_ms() - t0;
    std::size_t i = 0;
    std::vector<std::byte> tile_out(std::size_t{tile} * tile * 4);
    for (std::uint32_t ty = 0; ty < tiles_y; ++ty) {
        for (std::uint32_t tx = 0; tx < tiles_x; ++tx) {
            const std::uint32_t x = tx * tile;
            const std::uint32_t y = ty * tile;
            const std::uint32_t w = std::min(tile, width - x);
            const std::uint32_t h = std::min(tile, height - y);
            const std::size_t need = std::size_t{w} * h * 4;
            static_cast<void>(
                planar::decode(streams[i++], w, h, planar::Orientation::top_down, std::span(tile_out).first(need)));
            for (std::uint32_t row = 0; row < h; ++row) {
                std::memcpy(out.data() + ((((std::size_t{y} + row) * width) + x) * 4),
                            tile_out.data() + (std::size_t{row} * w * 4), std::size_t{w} * 4);
            }
        }
    }
    return {.codec = "planar (lossless)",
            .bytes_per_frame = static_cast<double>(bytes),
            .encode_ms = encode_ms,
            .latency_ms = encode_ms,
            .psnr_all = psnr(img, out, false),
            .psnr_text = psnr(img, out, true),
            .lossless = true};
}

/// H.264 over the whole surface, at the rate control of a quality tier.
/// There is no H.264 decoder in the tree, so this reports bitrate and CPU
/// only; the picture quality against FreeRDP's decoder is in
/// tests/video/test_avc444_encoder.cpp.
std::vector<Result> bench_h264(const std::vector<Image>& frames, std::uint32_t cap_kbps, std::string_view label)
{
    video::EncoderConfig config;
    config.width = (width + 15U) & ~15U;
    config.height = (height + 15U) & ~15U;
    config.fps = fps;
    config.rate = {.mode = video::RateControl::Mode::constant_quality,
                   .quality = 23,
                   .bitrate_kbps = 0,
                   .max_bitrate_kbps = cap_kbps,
                   .vbv_window_ms = 500};
    auto encoder = video::create_encoder(video::compiled_backends(), config, {});
    if (!encoder) {
        std::printf("    (no H.264 encoder: %s)\n", encoder.error().message().c_str());
        return {};
    }
    farland::codec::Yuv420Frame yuv(config.width, config.height);
    std::size_t bytes = 0;
    double encode_ms = 0;
    double worst_ms = 0;
    for (const Image& f : frames) {
        farland::codec::bgrx_to_yuv420(f.view(), yuv);
        const double t0 = now_ms();
        const auto out = (*encoder)->encode(yuv.view());
        const double dt = now_ms() - t0;
        encode_ms += dt;
        worst_ms = std::max(worst_ms, dt);
        if (out) {
            bytes += out->bitstream.size();
        }
    }
    const auto n = static_cast<double>(frames.size());
    return {Result{.codec = std::string(label) + " [" + std::string(video::to_string((*encoder)->backend())) + "]",
                   .bytes_per_frame = static_cast<double>(bytes) / n,
                   .encode_ms = encode_ms / n,
                   .latency_ms = worst_ms,
                   .psnr_all = 0,
                   .psnr_text = 0,
                   .lossless = false,
                   .has_psnr = false}};
}

void print_header(std::string_view corpus)
{
    std::printf("\n== %s, %u x %u ==\n", std::string(corpus).c_str(), width, height);
    std::printf("  %-46s %9s %10s %9s %9s %8s %8s\n", "codec", "KB/frame", "kbit/s@30", "CPU ms", "latency", "PSNR dB",
                "text dB");
}

void print(const Result& r)
{
    const double kbps = r.bytes_per_frame * 8.0 * fps / 1000.0;
    if (r.has_psnr) {
        char text[16] = "       -";
        if (r.psnr_text > 0) {
            std::snprintf(text, sizeof text, "%8.1f", r.psnr_text);
        }
        std::printf("  %-46s %9.1f %10.0f %9.1f %8.0fms %8.1f %s%s\n", r.codec.c_str(), r.bytes_per_frame / 1024.0,
                    kbps, r.encode_ms, r.latency_ms, r.psnr_all, text, r.lossless ? "  exact" : "");
    } else {
        std::printf("  %-46s %9.1f %10.0f %9.1f %8.0fms %8s %8s\n", r.codec.c_str(), r.bytes_per_frame / 1024.0, kbps,
                    r.encode_ms, r.latency_ms, "-", "-");
    }
}

void run_corpus(std::string_view name, const Image& img, const std::vector<Image>& motion)
{
    print_header(name);
    print(bench_planar(img));
    print(bench_clear(img));
    print(bench_progressive_single(img, progressive::quant_default, "progressive, quant_default"));
    print(bench_progressive_single(img, progressive::quant_highest, "progressive, quant_highest"));
    print(bench_progressive_refined(img, progressive::quant_default));
    if (!motion.empty()) {
        for (const auto& r : bench_h264(motion, 6000, "avc420, 6 Mbit/s cap")) {
            print(r);
        }
        for (const auto& r : bench_h264(motion, 2000, "avc420, 2 Mbit/s cap")) {
            print(r);
        }
    }
}

/// The exit criterion: a page of text has to stay sharp at 2 Mbit/s.
void text_at_two_megabit()
{
    const Image page = text_page();
    std::printf("\n== exit criterion: text at %.0f kbit/s ==\n", text_target_kbps);
    const double frame_budget = text_target_kbps * 1000.0 / 8.0 / fps;
    std::printf("  budget: %.0f bytes per frame at %u fps\n", frame_budget, fps);
    const auto pipeline = bench_clear(page);  // mixed mode routes text tiles here
    const double kbps = pipeline.bytes_per_frame * 8.0 * fps / 1000.0;
    std::printf("  clearcodec (what mixed mode sends for text tiles):\n");
    std::printf("    %.1f KB per full repaint, %.0f kbit/s if the whole page repainted 30 times a second\n",
                pipeline.bytes_per_frame / 1024.0, kbps);
    std::printf("    pixel-exact: %s (PSNR %.1f dB over the glyphs)\n", pipeline.psnr_text >= 98.0 ? "yes" : "NO",
                pipeline.psnr_text);
    // A full repaint of a whole page of text is the worst case; a caret or a
    // typed line touches a few tiles. Report what the budget pays for.
    const double tiles_per_frame = frame_budget / (pipeline.bytes_per_frame / (double{tiles_x} * tiles_y));
    std::printf("    at %.0f kbit/s the budget pays for %.0f text tiles per frame (%.0f x 64 x 64 pixels)\n",
                text_target_kbps, tiles_per_frame, tiles_per_frame);
    const auto prog = bench_progressive_single(page, progressive::quant_default, "progressive");
    std::printf("  progressive on the same page, for comparison: %.1f KB, PSNR %.1f dB over the glyphs\n",
                prog.bytes_per_frame / 1024.0, prog.psnr_text);
    // What a person actually does to a page of text: type into it, so a
    // handful of tiles repaint per frame. That is the case the criterion is
    // about; a full-page repaint is a once-off that costs a second of budget.
    clear::Encoder encoder;
    clear::Decoder decoder;
    std::vector<std::byte> shown(frame_bytes, std::byte{0});
    Image page2 = page;
    std::size_t typed_bytes = 0;
    std::size_t typed_frames = 0;
    constexpr std::uint32_t typed_tiles = 3;  // a caret and the word around it
    for (std::uint32_t n = 0; n < 60; ++n) {
        // One line of glyphs is redrawn: the tiles it sits in change.
        const std::uint32_t y0 = 300 + ((n % 4) * 15);
        draw_text_line(page2, 24, y0, typed_tiles * tile / 7, 25, 250, 4242 + n);
        for (std::uint32_t t = 0; t < typed_tiles; ++t) {
            const std::uint32_t x = 24 - (24 % tile) + (t * tile);
            const std::uint32_t y = y0 - (y0 % tile);
            const std::uint32_t w = std::min(tile, width - x);
            const std::uint32_t h = std::min(tile, height - y);
            const std::size_t offset = ((std::size_t{y} * width) + x) * 4;
            const ImageView region{page2.view().data.subspan(offset, (((std::size_t{h} - 1) * width) + w) * 4), w, h,
                                   std::size_t{width} * 4};
            const auto stream = encoder.encode(region);
            typed_bytes += stream.size();
            static_cast<void>(decoder.decode(stream, w, h,
                                             std::span(shown).subspan(offset, (((std::size_t{h} - 1) * width) + w) * 4),
                                             std::size_t{width} * 4));
        }
        ++typed_frames;
    }
    const double typed_per_frame = static_cast<double>(typed_bytes) / static_cast<double>(typed_frames);
    std::printf("  typing into the page (%u tiles repainted per frame, 60 frames):\n", typed_tiles);
    std::printf("    %.0f bytes per frame, %.0f kbit/s at %u fps\n", typed_per_frame,
                typed_per_frame * 8.0 * fps / 1000.0, fps);

    const bool exact = pipeline.psnr_text >= 98.0;
    const bool typing_fits = typed_per_frame < frame_budget;
    const bool repaint_fits = pipeline.bytes_per_frame < frame_budget * fps;
    std::printf("  verdict: text is %s; typing %s the %.0f kbit/s budget, and a full-page repaint %s one\n"
                "           second of it\n",
                exact ? "pixel-exact" : "NOT exact", typing_fits ? "fits inside" : "does NOT fit into",
                text_target_kbps, repaint_fits ? "fits inside" : "does NOT fit into");
}

}  // namespace

int main()
{
    std::printf("farland codec benchmark (M5 exit), %u x %u at %u fps\n", width, height, fps);

    std::vector<Image> video_frames;
    video_frames.reserve(30);
    for (int i = 0; i < 30; ++i) {
        video_frames.push_back(photo(i * 0.01, i % 7));
    }
    std::vector<Image> desktop_frames;
    desktop_frames.reserve(30);
    for (int i = 0; i < 30; ++i) {
        desktop_frames.push_back(mixed_desktop(i * 0.01));
    }

    run_corpus("text: a dense page of glyphs", text_page(), {});
    run_corpus("ui: panels, chrome and labels", ui_page(), {});
    run_corpus("photo: gradients, blobs and noise", photo(), video_frames);
    run_corpus("mixed: a desktop with a picture in a window", mixed_desktop(), desktop_frames);
    text_at_two_megabit();
    std::printf("\nCPU is one core, encoding only; latency for the refining pipeline is the time\n"
                "until the last upgrade arrives at %u frames per second.\n",
                fps);
    return 0;
}
