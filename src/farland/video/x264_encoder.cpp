// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// x264 backend. Settings ported from the ZeroVDI server bridge
// (KSol.ZeroVDI/RDP/Bridge/Libx264H264Encoder.cs, same author).

#include <farland/base/assert.hpp>
#include <farland/base/log.hpp>
#include <farland/video/x264_encoder.hpp>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

// x264.h needs the fixed-width integer types first.
#include <x264.h>

namespace farland::video::x264 {

namespace {

constexpr std::string_view component = "video.x264";
constexpr const char* preset = "veryfast";
constexpr std::uint64_t microseconds = 1'000'000;

/// Routes x264's log into farland's log.
void log_callback(void* /*context*/, int level, const char* format, va_list args) noexcept
{
    try {
        std::array<char, 512> buffer{};
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,cert-err33-c)
        const int written = std::vsnprintf(buffer.data(), buffer.size(), format, args);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        if (written <= 0) {
            return;
        }
        std::string_view text(buffer.data(), std::min(static_cast<std::size_t>(written), buffer.size() - 1));
        while (!text.empty() && text.back() == '\n') {
            text.remove_suffix(1);
        }
        if (level <= X264_LOG_ERROR) {
            log::error(component, "{}", text);
        } else {
            log::warn(component, "{}", text);
        }
    } catch (const std::exception&) {
        // Logging must not unwind through x264.
        static_cast<void>(0);
    }
}

[[nodiscard]] const char* profile_name(Profile profile)
{
    switch (profile) {
    case Profile::constrained_baseline:
        return "baseline";  // x264's baseline is constrained (no FMO, ASO or redundant slices).
    case Profile::main:
        return "main";
    case Profile::high:
        return "high";
    }
    return "baseline";
}

void apply_rate(x264_param_t& p, const RateControl& rate)
{
    std::uint32_t cap = rate.max_bitrate_kbps;
    if (rate.mode == RateControl::Mode::constant_quality) {
        p.rc.i_rc_method = X264_RC_CRF;
        p.rc.f_rf_constant = static_cast<float>(rate.quality);
    } else {
        p.rc.i_rc_method = X264_RC_ABR;
        p.rc.i_bitrate = static_cast<int>(rate.bitrate_kbps);
        cap = std::max(cap, rate.bitrate_kbps);
    }
    // Capped CRF: CRF sets the quality and VBV clamps bursts, with a buffer of
    // vbv_window_ms at the cap so latency stays bounded.
    p.rc.i_vbv_max_bitrate = static_cast<int>(cap);
    p.rc.i_vbv_buffer_size =
        cap == 0 ? 0 : static_cast<int>(std::max<std::uint64_t>(1, std::uint64_t{cap} * rate.vbv_window_ms / 1000U));
}

[[nodiscard]] Result<void> build_params(const EncoderConfig& config, x264_param_t& p)
{
    if (x264_param_default_preset(&p, preset, "zerolatency") < 0) {
        return fail(Errc::unsupported, "x264 does not know the veryfast preset");
    }
    p.pf_log = &log_callback;
    p.i_log_level = X264_LOG_WARNING;

    p.i_width = static_cast<int>(config.width);
    p.i_height = static_cast<int>(config.height);
    p.i_csp = X264_CSP_I420;
    p.i_threads = static_cast<int>(config.threads);
    p.b_sliced_threads = 1;
    p.i_sync_lookahead = 0;
    p.rc.i_lookahead = 0;
    p.rc.b_mb_tree = 0;

    // Constant-frame-rate rate control at `fps`. With b_vfr_input, x264 holds
    // every picture back until the next one arrives, to learn its duration
    // (one frame of latency, even with zerolatency). Time stamps are in
    // microseconds and only order the pictures.
    p.i_fps_num = config.fps;
    p.i_fps_den = 1;
    p.i_timebase_num = 1;
    p.i_timebase_den = static_cast<std::uint32_t>(microseconds);
    p.b_vfr_input = 0;

    p.i_bframe = 0;
    p.i_frame_reference = static_cast<int>(config.reference_frames);
    p.i_keyint_max = config.keyint == 0 ? X264_KEYINT_MAX_INFINITE : static_cast<int>(config.keyint);
    p.i_scenecut_threshold = 0;
    p.b_intra_refresh = 0;

    p.b_repeat_headers = 1;
    p.b_annexb = 1;
    p.b_aud = config.access_unit_delimiters ? 1 : 0;

    // Full-range BT.709 ([MS-RDPEGFX] 3.3.8.3.1), sRGB transfer.
    p.vui.b_fullrange = 1;
    p.vui.i_colorprim = 1;
    p.vui.i_transfer = 13;
    p.vui.i_colmatrix = 1;

    apply_rate(p, config.rate);
    if (x264_param_apply_profile(&p, profile_name(config.profile)) < 0) {
        return fail(Errc::invalid_value, "x264 rejected the H.264 profile");
    }
    return {};
}

class X264Encoder final : public H264Encoder {
public:
    X264Encoder() = default;
    X264Encoder(const X264Encoder&) = delete;
    X264Encoder& operator=(const X264Encoder&) = delete;
    X264Encoder(X264Encoder&&) = delete;
    X264Encoder& operator=(X264Encoder&&) = delete;
    ~X264Encoder() override { close(); }

    [[nodiscard]] Backend backend() const noexcept override { return Backend::x264; }
    [[nodiscard]] const EncoderConfig& config() const noexcept override { return config_; }
    [[nodiscard]] Result<void> configure(const EncoderConfig& config) override;
    [[nodiscard]] Result<void> set_rate_control(const RateControl& rate) override;
    void request_idr() noexcept override { force_idr_ = true; }
    [[nodiscard]] Result<EncodedFrame> encode(const codec::Yuv420View& picture, const FrameOptions& options) override;

private:
    void close() noexcept
    {
        if (encoder_ != nullptr) {
            x264_encoder_close(encoder_);
            encoder_ = nullptr;
        }
    }

    x264_t* encoder_ = nullptr;
    x264_param_t params_{};
    EncoderConfig config_;
    bool force_idr_ = true;
    std::uint64_t frames_ = 0;
    std::optional<std::int64_t> last_pts_;
};

Result<void> X264Encoder::configure(const EncoderConfig& config)
{
    FARLAND_TRY_VOID(validate(config));
    close();
    x264_param_t params{};
    FARLAND_TRY_VOID(build_params(config, params));
    encoder_ = x264_encoder_open(&params);
    if (encoder_ == nullptr) {
        return fail(Errc::invalid_value, "x264 rejected the encoder configuration");
    }
    params_ = params;
    config_ = config;
    force_idr_ = true;
    frames_ = 0;
    last_pts_.reset();
    return {};
}

Result<void> X264Encoder::set_rate_control(const RateControl& rate)
{
    FARLAND_TRY_VOID(validate(rate));
    if (encoder_ == nullptr) {
        return fail(Errc::io, "x264 encoder is not configured");
    }
    x264_param_t params = params_;
    apply_rate(params, rate);
    // x264_encoder_reconfig cannot switch between CRF and ABR, nor turn VBV on
    // or off; those re-open the encoder.
    const bool same_method = params.rc.i_rc_method == params_.rc.i_rc_method;
    const bool same_vbv = (params.rc.i_vbv_max_bitrate > 0) == (params_.rc.i_vbv_max_bitrate > 0);
    if (!same_method || !same_vbv) {
        EncoderConfig config = config_;
        config.rate = rate;
        return configure(config);
    }
    if (x264_encoder_reconfig(encoder_, &params) < 0) {
        return fail(Errc::invalid_value, "x264 rejected the rate control");
    }
    params_ = params;
    config_.rate = rate;
    return {};
}

Result<EncodedFrame> X264Encoder::encode(const codec::Yuv420View& picture, const FrameOptions& options)
{
    if (encoder_ == nullptr) {
        return fail(Errc::io, "x264 encoder is not configured");
    }
    FARLAND_ASSERT(picture.width == config_.width && picture.height == config_.height);
    FARLAND_ASSERT(picture.y_stride >= picture.width && picture.uv_stride >= picture.width / 2U);
    FARLAND_ASSERT(picture.y.size() >= picture.y_stride * picture.height);
    FARLAND_ASSERT(picture.u.size() >= picture.uv_stride * (picture.height / 2U));
    FARLAND_ASSERT(picture.v.size() >= picture.uv_stride * (picture.height / 2U));

    auto pts = static_cast<std::int64_t>(options.timestamp_us.has_value() ? *options.timestamp_us
                                                                          : frames_ * microseconds / config_.fps);
    if (last_pts_.has_value() && pts <= *last_pts_) {
        pts = *last_pts_ + 1;
    }

    x264_picture_t in;
    x264_picture_init(&in);
    in.img.i_csp = X264_CSP_I420;
    in.img.i_plane = 3;
    in.img.i_stride[0] = static_cast<int>(picture.y_stride);
    in.img.i_stride[1] = static_cast<int>(picture.uv_stride);
    in.img.i_stride[2] = static_cast<int>(picture.uv_stride);
    // x264 takes non-const plane pointers but copies the picture without writing to it.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast,cppcoreguidelines-pro-type-reinterpret-cast)
    in.img.plane[0] = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(picture.y.data()));
    in.img.plane[1] = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(picture.u.data()));
    in.img.plane[2] = reinterpret_cast<std::uint8_t*>(const_cast<std::byte*>(picture.v.data()));
    // NOLINTEND(cppcoreguidelines-pro-type-const-cast,cppcoreguidelines-pro-type-reinterpret-cast)
    in.i_pts = pts;
    in.i_type = (force_idr_ || options.force_idr) ? X264_TYPE_IDR : X264_TYPE_AUTO;

    x264_picture_t out;
    x264_picture_init(&out);
    x264_nal_t* nals = nullptr;
    int count = 0;
    const int size = x264_encoder_encode(encoder_, &nals, &count, &in, &out);
    if (size < 0) {
        force_idr_ = true;
        return fail(Errc::io, "x264_encoder_encode failed");
    }
    if (size == 0 || count <= 0 || nals == nullptr) {
        // Only possible with a delay (lookahead, B-frames, frame threads),
        // which build_params rules out.
        force_idr_ = true;
        return fail(Errc::io, "x264 held back a frame");
    }

    EncodedFrame frame;
    frame.bitstream.reserve(static_cast<std::size_t>(size));
    for (const x264_nal_t& nal : std::span(nals, static_cast<std::size_t>(count))) {
        const auto bytes = std::as_bytes(std::span(nal.p_payload, static_cast<std::size_t>(nal.i_payload)));
        frame.bitstream.insert(frame.bitstream.end(), bytes.begin(), bytes.end());
    }
    frame.idr = out.i_type == X264_TYPE_IDR;
    // On output, i_qpplus1 is the frame's average QP plus one.
    frame.qp = static_cast<std::uint8_t>(std::clamp(out.i_qpplus1 - 1, 0, 51));
    force_idr_ = false;
    ++frames_;
    last_pts_ = pts;
    return frame;
}

}  // namespace

Result<std::unique_ptr<H264Encoder>> create(const EncoderConfig& config)
{
    auto encoder = std::make_unique<X264Encoder>();
    FARLAND_TRY_VOID(encoder->configure(config));
    return encoder;
}

}  // namespace farland::video::x264
