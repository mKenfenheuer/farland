// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/codec/yuv.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// H.264 encoding for the RDPGFX AVC420 codec (docs/PLAN.md §3.1): one
/// interface, several backends.
///
/// Every backend is set up for low-latency desktop streaming, following the
/// ZeroVDI libx264 bridge (docs/PLAN.md §4):
/// - one access unit comes out for every picture that goes in, immediately:
///   no B-frames, no reordering, no lookahead, no frame threads;
/// - IDR frames come only at the start, on request and (optionally) every
///   `keyint` frames, never from scene-cut detection;
/// - SPS and PPS are repeated before every IDR;
/// - an access unit delimiter leads every access unit (optional);
/// - the VUI signals full-range BT.709, matching codec::bgrx_to_yuv420.
///
/// Encoding is synchronous, so the ZeroVDI ffmpeg backend's idle flush timer
/// has no counterpart here.
///
/// An encoder is not thread-safe. Keep one per RDPGFX surface.
///
/// VA-API and NVENC (M5) will take GPU surfaces and convert colour on the
/// GPU. They get their own encode entry point then; configuration, rate
/// control, IDR requests and EncodedFrame stay the same.
namespace farland::video {

enum class Backend : std::uint8_t {
    openh264,  ///< Cisco OpenH264, loaded at runtime (BSD-2-Clause).
    x264,      ///< libx264, linked at build time with -Dx264=enabled (GPLv2+).
};

[[nodiscard]] std::string_view to_string(Backend backend) noexcept;
[[nodiscard]] std::optional<Backend> parse_backend(std::string_view name) noexcept;

/// Backends compiled into this build, in the default order of preference.
/// OpenH264 is always compiled in; whether it works depends on the library at
/// runtime.
[[nodiscard]] std::span<const Backend> compiled_backends() noexcept;

/// Highest H.264 profile the stream may use. RDPGFX does not tell the server
/// which decoder the client has, and FreeRDP clients that decode with
/// OpenH264 handle only Constrained Baseline, so that is the default. The
/// OpenH264 backend always produces Constrained Baseline.
enum class Profile : std::uint8_t { constrained_baseline, main, high };

struct RateControl {
    enum class Mode : std::uint8_t {
        /// Constant quality: x264 CRF `quality`. OpenH264 has no CRF: without
        /// a cap it encodes at the fixed QP `quality`; with a cap it runs
        /// bitrate control at the cap with `quality` as the minimum QP.
        constant_quality,
        /// Average bitrate `bitrate_kbps`.
        bitrate,
    };

    Mode mode = Mode::constant_quality;
    /// On the H.264 QP scale, 0 (best) to 51.
    std::uint8_t quality = 23;
    std::uint32_t bitrate_kbps = 0;
    /// VBV cap in kbit/s; 0 means none for constant_quality and bitrate_kbps
    /// for bitrate mode.
    std::uint32_t max_bitrate_kbps = 0;
    /// VBV buffer as time at the cap. ZeroVDI used half a second.
    std::uint32_t vbv_window_ms = 500;

    friend bool operator==(const RateControl&, const RateControl&) = default;
};

inline constexpr std::uint32_t max_dimension = 8192;
inline constexpr std::uint32_t max_fps = 240;
inline constexpr std::uint32_t max_threads = 16;
inline constexpr std::uint32_t max_bitrate_kbps = 1'000'000;
inline constexpr std::uint32_t max_vbv_window_ms = 10'000;

struct EncoderConfig {
    /// Coded picture size: multiples of 16 up to max_dimension
    /// (codec::avc::coded_size of the surface size).
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// Nominal frame rate, 1..max_fps. Rate control uses it when frames carry
    /// no timestamp.
    std::uint32_t fps = 30;
    RateControl rate;
    Profile profile = Profile::constrained_baseline;
    /// Slice threads, 1..max_threads. Slices parallelise without the frame
    /// delay of x264's frame threads; ZeroVDI used 1.
    std::uint32_t threads = 1;
    /// Largest distance between IDR frames; 0 means IDR only on request.
    std::uint32_t keyint = 0;
    bool access_unit_delimiters = true;

    friend bool operator==(const EncoderConfig&, const EncoderConfig&) = default;
};

[[nodiscard]] Result<void> validate(const RateControl& rate);
[[nodiscard]] Result<void> validate(const EncoderConfig& config);

struct FrameOptions {
    /// Encode this picture as an IDR.
    bool force_idr = false;
    /// Capture time in microseconds. Must increase from frame to frame
    /// (smaller values are bumped). Without it, frame number / fps is used.
    /// OpenH264's rate control follows it. x264 budgets by `fps` instead,
    /// because its variable-frame-rate mode holds every picture back until
    /// the next one arrives.
    std::optional<std::uint64_t> timestamp_us;
};

struct EncodedFrame {
    /// One access unit in Annex B format. Empty if the encoder dropped the
    /// picture; the caller then keeps its damage for the next one.
    std::vector<std::byte> bitstream;
    bool idr = false;
    /// Average QP of the picture, for RDPGFX_AVC420_QUANT_QUALITY.
    std::uint8_t qp = 0;
};

class H264Encoder {
public:
    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;
    H264Encoder(H264Encoder&&) = delete;
    H264Encoder& operator=(H264Encoder&&) = delete;
    virtual ~H264Encoder() = default;

    [[nodiscard]] virtual Backend backend() const noexcept = 0;
    [[nodiscard]] virtual const EncoderConfig& config() const noexcept = 0;

    /// Re-opens the encoder with a new configuration, such as a resized
    /// surface. The next picture is an IDR. On error the encoder is closed
    /// until configure succeeds.
    [[nodiscard]] virtual Result<void> configure(const EncoderConfig& config) = 0;

    /// Changes rate control. A change the backend can apply to the running
    /// stream (a new bitrate or CRF within the same mode) takes effect with
    /// the next picture; anything else re-opens the encoder, which makes the
    /// next picture an IDR.
    [[nodiscard]] virtual Result<void> set_rate_control(const RateControl& rate) = 0;

    /// Makes the next picture an IDR.
    virtual void request_idr() noexcept = 0;

    /// Encodes one picture of exactly config().width x config().height
    /// (asserted). The result owns its bytes.
    [[nodiscard]] virtual Result<EncodedFrame> encode(const codec::Yuv420View& picture,
                                                      const FrameOptions& options = {}) = 0;

protected:
    H264Encoder() = default;
};

struct BackendOptions {
    /// OpenH264 library to load: a path or a file name for dlopen. Empty
    /// tries the names in openh264::default_library_names.
    std::string openh264_library;
};

/// Creates and configures an encoder of one backend. Errc::unsupported if the
/// backend is not compiled in or its library is unavailable (the reason is
/// logged under "video.*").
[[nodiscard]] Result<std::unique_ptr<H264Encoder>> create_encoder(Backend backend, const EncoderConfig& config,
                                                                  const BackendOptions& options = {});

/// Tries the backends in `order` and returns the first that configures. An
/// invalid configuration fails at once; otherwise the error says that no
/// backend is available.
[[nodiscard]] Result<std::unique_ptr<H264Encoder>>
create_encoder(std::span<const Backend> order, const EncoderConfig& config, const BackendOptions& options = {});

}  // namespace farland::video
