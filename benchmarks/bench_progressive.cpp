// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// RemoteFX Progressive benchmark: whole 1920 x 1080 frames through the
// encoder and decoder, and the RemoteFX primitives one stage at a time over
// the 510 tiles of such a frame. Not part of `meson test`; build and run with
//   meson compile -C build bench-progressive && ./build/benchmarks/bench-progressive
// in a release build (--buildtype=release) for meaningful numbers.

#include <farland/codec/image.hpp>
#include <farland/codec/progressive.hpp>
#include <farland/codec/rfx_common.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace progressive = farland::codec::progressive;
namespace rfx = farland::codec::rfx;
using farland::codec::ImageView;

constexpr std::uint32_t width = 1920;
constexpr std::uint32_t height = 1080;
constexpr std::size_t frame_bytes = std::size_t{width} * height * 4;

struct Image {
    std::vector<std::byte> data = std::vector<std::byte>(frame_bytes);
    [[nodiscard]] ImageView view() const
    {
        return {.data = data, .width = width, .height = height, .stride = std::size_t{width} * 4};
    }
    void set(std::uint32_t x, std::uint32_t y, int r, int g, int b)
    {
        const std::size_t at = ((std::size_t{y} * width) + x) * 4;
        data[at] = static_cast<std::byte>(std::clamp(b, 0, 255));
        data[at + 1] = static_cast<std::byte>(std::clamp(g, 0, 255));
        data[at + 2] = static_cast<std::byte>(std::clamp(r, 0, 255));
    }
};

/// Gradients, soft shapes and mild noise: a stand-in for a photo.
Image natural_image()
{
    Image img;
    std::uint32_t state = 1;
    const auto noise = [&] {
        state = (state * 1664525U) + 1013904223U;
        return static_cast<int>((state >> 24U) % 9U) - 4;
    };
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const double fx = static_cast<double>(x) / width;
            const double fy = static_cast<double>(y) / height;
            const double wave = std::sin((fx * 9.0) + (fy * 4.0)) * 40.0;
            const double dx = fx - 0.6;
            const double dy = fy - 0.4;
            const double blob = std::exp(-((dx * dx) + (dy * dy)) * 18.0) * 90.0;
            img.set(x, y, static_cast<int>(60 + (fx * 150) + wave) + noise(),
                    static_cast<int>(90 + (fy * 100) + blob) + noise(),
                    static_cast<int>(140 - (fx * 60) + blob) + noise());
        }
    }
    return img;
}

/// Flat panels with thin dark "text" strokes: a stand-in for a desktop.
Image ui_image()
{
    Image img;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const bool panel = ((x / 480) + (y / 270)) % 2 == 0;
            int v = panel ? 240 : 200;
            const bool text_row = (y % 18) >= 4 && (y % 18) < 14;
            const bool glyph = ((x * 7) + ((y % 18) * 3)) % 11 < 2 && (x % 90) < 70;
            if (text_row && glyph && x % 480 > 20) {
                v = 30;
            }
            img.set(x, y, v, v, panel ? v : v + 15);
        }
    }
    return img;
}

struct Timing {
    double best_ms = 0;
    double median_ms = 0;
};

/// Runs `body` repeatedly for about `seconds` (at least 3 times).
Timing measure(const std::function<void()>& body, double seconds = 1.0)
{
    using clock = std::chrono::steady_clock;
    std::vector<double> runs;
    const auto start = clock::now();
    while (runs.size() < 3 || std::chrono::duration<double>(clock::now() - start).count() < seconds) {
        const auto t0 = clock::now();
        body();
        runs.push_back(std::chrono::duration<double, std::milli>(clock::now() - t0).count());
    }
    std::ranges::sort(runs);
    return {.best_ms = runs.front(), .median_ms = runs[runs.size() / 2]};
}

void report(std::string_view what, const Timing& t, std::size_t bytes = frame_bytes)
{
    const double mbps = static_cast<double>(bytes) / (t.median_ms / 1000.0) / 1e6;
    std::printf("  %-44s %8.2f ms/frame (best %7.2f)  %8.1f MB/s\n", std::string(what).c_str(), t.median_ms, t.best_ms,
                mbps);
}

std::size_t total_size(const std::vector<std::vector<std::byte>>& streams)
{
    std::size_t n = 0;
    for (const auto& s : streams) {
        n += s.size();
    }
    return n;
}

void bench_frames(std::string_view name, const Image& img)
{
    std::printf("%s image, 1920 x 1080:\n", std::string(name).c_str());
    const std::array full{progressive::Rect{.x = 0, .y = 0, .width = width, .height = height}};
    for (const auto& [label, quant] : {std::pair{"quant_default", progressive::quant_default},
                                       std::pair{"quant_highest", progressive::quant_highest}}) {
        progressive::Encoder encoder(width, height, {.quant = quant});
        std::vector<std::vector<std::byte>> streams;
        const Timing enc = measure([&] { streams = encoder.encode(img.view(), full); });
        char what[96];
        std::snprintf(what, sizeof what, "encode, %s (%zu KB, %zu streams)", label, total_size(streams) / 1024,
                      streams.size());
        report(what, enc);
        std::uint32_t frame = 0;
        auto decoder = progressive::Decoder::create(width, height);
        const Timing dec = measure([&] {
            ++frame;
            for (const auto& s : streams) {
                static_cast<void>(decoder->decode(s, frame));
            }
        });
        std::snprintf(what, sizeof what, "decode, %s", label);
        report(what, dec);
    }
}

void bench_primitives(const Image& img)
{
    std::printf("primitives over the 510 tiles of the natural image:\n");
    constexpr std::uint32_t grid_w = (width + 63) / 64;
    constexpr std::uint32_t grid_h = (height + 63) / 64;
    std::vector<rfx::Planes> planes(std::size_t{grid_w} * grid_h);
    rfx::Coefficients scratch{};
    const auto each_tile = [&](const std::function<void(rfx::Planes&, std::uint32_t, std::uint32_t)>& f) {
        for (std::uint32_t ty = 0; ty < grid_h; ++ty) {
            for (std::uint32_t tx = 0; tx < grid_w; ++tx) {
                f(planes[(std::size_t{ty} * grid_w) + tx], tx, ty);
            }
        }
    };
    const auto each_component = [&](const std::function<void(rfx::Coefficients&)>& f) {
        each_tile([&](rfx::Planes& p, std::uint32_t, std::uint32_t) {
            f(p.y);
            f(p.cb);
            f(p.cr);
        });
    };

    report("load_tile (RGB to YCbCr)", measure([&] {
               each_tile([&](rfx::Planes& p, auto tx, auto ty) { rfx::load_tile(img.view(), tx * 64, ty * 64, p); });
           }));
    const std::vector<rfx::Planes> pixels = planes;
    report("dwt_encode", measure([&] {
               planes = pixels;
               each_component([&](rfx::Coefficients& c) { rfx::dwt_encode(c, scratch); });
           }));
    const std::vector<rfx::Planes> transformed = planes;
    report("quantize (quant_default)", measure([&] {
               planes = transformed;
               each_component([&](rfx::Coefficients& c) { rfx::quantize(c, progressive::quant_default); });
           }));
    const std::vector<rfx::Planes> quantized = planes;
    std::vector<std::byte> out;
    out.reserve(4 << 20);
    for (const auto mode : {rfx::RlgrMode::rlgr1, rfx::RlgrMode::rlgr3}) {
        const Timing t = measure([&] {
            out.clear();
            each_component([&](rfx::Coefficients& c) { rfx::rlgr_encode(mode, c, out); });
        });
        char what[96];
        std::snprintf(what, sizeof what, "rlgr_encode %s (%zu KB)", mode == rfx::RlgrMode::rlgr1 ? "RLGR1" : "RLGR3",
                      out.size() / 1024);
        report(what, t);
    }

    // Decoder side: RLGR1 per component, then the inverse DWTs and colours.
    std::vector<std::vector<std::byte>> coded;
    each_component([&](rfx::Coefficients& c) {
        coded.emplace_back();
        rfx::rlgr_encode(rfx::RlgrMode::rlgr1, c, coded.back());
    });
    report("rlgr_decode RLGR1", measure([&] {
               std::size_t i = 0;
               each_component([&](rfx::Coefficients& c) {
                   static_cast<void>(rfx::rlgr_decode(rfx::RlgrMode::rlgr1, coded[i++], c));
               });
           }));
    report("dwt_decode", measure([&] {
               planes = transformed;
               each_component([&](rfx::Coefficients& c) { rfx::dwt_decode(c, scratch); });
           }));
    report("dwt_decode_extrapolate", measure([&] {
               planes = transformed;
               each_component([&](rfx::Coefficients& c) { rfx::dwt_decode_extrapolate(c, scratch); });
           }));
    std::vector<std::byte> tile_pixels(rfx::tile_coefficients * 4);
    report("store_tile (YCbCr to RGB)",
           measure([&] { each_tile([&](rfx::Planes& p, auto, auto) { rfx::store_tile(p, tile_pixels); }); }));
    planes = pixels;
}

/// Whole-frame encoding with each instruction set the CPU has.
void bench_isas(const Image& img)
{
    std::printf("encode, quant_default, natural image, per instruction set:\n");
    const std::array full{progressive::Rect{.x = 0, .y = 0, .width = width, .height = height}};
    for (const rfx::Isa isa : rfx::available_isas()) {
        rfx::set_isa(isa);
        progressive::Encoder encoder(width, height);
        report(rfx::isa_name(isa), measure([&] { static_cast<void>(encoder.encode(img.view(), full)); }));
    }
    rfx::set_isa(rfx::available_isas().back());
}

/// Reduce-extrapolate single pass, and refinement: the first pass, then one
/// upgrade() call that takes every tile to full quality.
void bench_refinement(std::string_view name, const Image& img)
{
    std::printf("%s image, reduce-extrapolate and refinement:\n", std::string(name).c_str());
    const std::array full{progressive::Rect{.x = 0, .y = 0, .width = width, .height = height}};
    for (const auto& [label, quant] : {std::pair{"quant_default", progressive::quant_default},
                                       std::pair{"quant_highest", progressive::quant_highest}}) {
        char what[112];
        {
            progressive::Encoder encoder(width, height, {.quant = quant, .reduce_extrapolate = true});
            std::vector<std::vector<std::byte>> streams;
            const Timing t = measure([&] { streams = encoder.encode(img.view(), full); });
            std::snprintf(what, sizeof what, "single pass, extrapolate, %s (%zu KB)", label,
                          total_size(streams) / 1024);
            report(what, t);
        }
        progressive::Encoder encoder(width, height, {.quant = quant, .reduce_extrapolate = true, .refine = true});
        std::vector<std::vector<std::byte>> first;
        const Timing t_first = measure([&] { first = encoder.encode(img.view(), full); });
        std::snprintf(what, sizeof what, "first pass, %s (%zu KB)", label, total_size(first) / 1024);
        report(what, t_first);
        std::vector<std::vector<std::byte>> upgrades;
        const Timing t_up = measure([&] {
            first = encoder.encode(img.view(), full);
            upgrades = encoder.upgrade(std::size_t{64} << 20);
        });
        std::snprintf(what, sizeof what, "first pass + upgrade to full, %s (+%zu KB)", label,
                      total_size(upgrades) / 1024);
        report(what, t_up);
    }
}

}  // namespace

int main()
{
    std::printf("kernels: %s\n", rfx::isa_name(rfx::active_isa()));
    const Image natural = natural_image();
    const Image ui = ui_image();
    bench_frames("natural", natural);
    bench_frames("UI", ui);
    bench_primitives(natural);
    bench_isas(natural);
    bench_refinement("natural", natural);
    bench_refinement("UI", ui);
    return 0;
}
