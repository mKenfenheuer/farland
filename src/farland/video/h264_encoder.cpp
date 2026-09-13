// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/video/h264_encoder.hpp>
#include <farland/video/openh264_encoder.hpp>
#if defined(FARLAND_VIDEO_HAVE_X264)
#include <farland/video/x264_encoder.hpp>
#endif

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace farland::video {

namespace {

constexpr std::string_view component = "video.h264";

#if defined(FARLAND_VIDEO_HAVE_X264)
constexpr std::array compiled{Backend::openh264, Backend::x264};
#else
constexpr std::array compiled{Backend::openh264};
#endif

}  // namespace

std::string_view to_string(Backend backend) noexcept
{
    switch (backend) {
    case Backend::openh264:
        return "openh264";
    case Backend::x264:
        return "x264";
    }
    return "unknown";
}

std::optional<Backend> parse_backend(std::string_view name) noexcept
{
    for (const Backend backend : {Backend::openh264, Backend::x264}) {
        if (name == to_string(backend)) {
            return backend;
        }
    }
    return std::nullopt;
}

std::span<const Backend> compiled_backends() noexcept
{
    return compiled;
}

Result<void> validate(const RateControl& rate)
{
    if (rate.quality > 51) {
        return fail(Errc::invalid_value, "H.264 quality must be a QP in 0..51");
    }
    if (rate.bitrate_kbps > max_bitrate_kbps || rate.max_bitrate_kbps > max_bitrate_kbps) {
        return fail(Errc::invalid_value, "H.264 bitrate is above 1 Gbit/s");
    }
    if (rate.mode == RateControl::Mode::bitrate) {
        if (rate.bitrate_kbps == 0) {
            return fail(Errc::invalid_value, "H.264 bitrate mode needs a bitrate");
        }
        if (rate.max_bitrate_kbps != 0 && rate.max_bitrate_kbps < rate.bitrate_kbps) {
            return fail(Errc::invalid_value, "H.264 maximum bitrate is below the target bitrate");
        }
    }
    if (rate.vbv_window_ms == 0 || rate.vbv_window_ms > max_vbv_window_ms) {
        return fail(Errc::invalid_value, "H.264 VBV window must be 1..10000 ms");
    }
    return {};
}

Result<void> validate(const EncoderConfig& config)
{
    for (const std::uint32_t side : {config.width, config.height}) {
        if (side == 0 || side % 16 != 0 || side > max_dimension) {
            return fail(Errc::invalid_value, "H.264 picture sides must be multiples of 16 up to 8192");
        }
    }
    if (config.fps == 0 || config.fps > max_fps) {
        return fail(Errc::invalid_value, "H.264 frame rate must be 1..240");
    }
    if (config.threads == 0 || config.threads > max_threads) {
        return fail(Errc::invalid_value, "H.264 encoder threads must be 1..16");
    }
    return validate(config.rate);
}

Result<std::unique_ptr<H264Encoder>> create_encoder(Backend backend, const EncoderConfig& config,
                                                    const BackendOptions& options)
{
    FARLAND_TRY_VOID(validate(config));
    switch (backend) {
    case Backend::openh264:
        return openh264::create(config, options.openh264_library);
    case Backend::x264:
#if defined(FARLAND_VIDEO_HAVE_X264)
        return x264::create(config);
#else
        return fail(Errc::unsupported, "farland was built without x264 (-Dx264=enabled)");
#endif
    }
    return fail(Errc::unsupported, "unknown H.264 encoder backend");
}

Result<std::unique_ptr<H264Encoder>> create_encoder(std::span<const Backend> order, const EncoderConfig& config,
                                                    const BackendOptions& options)
{
    FARLAND_TRY_VOID(validate(config));
    for (const Backend backend : order) {
        auto encoder = create_encoder(backend, config, options);
        if (encoder.has_value()) {
            log::info(component, "H.264 encoder: {} ({}x{})", to_string(backend), config.width, config.height);
            return encoder;
        }
        log::info(component, "H.264 backend {} is not available: {}", to_string(backend), encoder.error().message());
    }
    return fail(Errc::unsupported, "no H.264 encoder backend is available");
}

}  // namespace farland::video
