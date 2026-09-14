// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// OpenH264 backend. The parameter choices follow FreeRDP's
// libfreerdp/codec/h264_openh264.c (Apache-2.0) where they agree with the
// low-latency settings in h264_encoder.hpp.

#include <farland/base/assert.hpp>
#include <farland/base/log.hpp>
#include <farland/video/openh264_abi.hpp>
#include <farland/video/openh264_encoder.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <dlfcn.h>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace farland::video::openh264 {

namespace {

constexpr std::string_view component = "video.openh264";
constexpr std::string_view not_found = "OpenH264 library not found";

#if defined(__APPLE__)
constexpr std::array<std::string_view, 5> library_names{
    "libopenh264.8.dylib",
    "libopenh264.7.dylib",
    "libopenh264.6.dylib",
    "libopenh264.dylib",
    "/opt/homebrew/lib/libopenh264.dylib",
};
#else
constexpr std::array<std::string_view, 4> library_names{
    "libopenh264.so.8",
    "libopenh264.so.7",
    "libopenh264.so.6",
    "libopenh264.so",
};
#endif

/// Structure layouts by release, see openh264_abi.hpp.
enum class Abi : std::uint8_t { v2_1, v2_3, v2_6 };

template <class Fn>
[[nodiscard]] Fn symbol(void* handle, const char* name)
{
    // POSIX requires that the object pointer dlsym returns converts to a
    // function pointer; C++ makes that conversion conditionally-supported.
    return reinterpret_cast<Fn>(dlsym(handle, name));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

/// A dlopen'ed OpenH264 with the entry points farland calls.
class Library {
public:
    Library() = default;
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    Library(Library&& other) noexcept { *this = std::move(other); }
    Library& operator=(Library&& other) noexcept
    {
        if (this != &other) {
            close();
            handle_ = std::exchange(other.handle_, nullptr);
            create_ = other.create_;
            destroy_ = other.destroy_;
            layout_ = other.layout_;
        }
        return *this;
    }
    ~Library() { close(); }

    [[nodiscard]] static Result<Library> open(std::string_view library);

    [[nodiscard]] int create(abi::ISVCEncoder** encoder) const { return create_(encoder); }
    void destroy(abi::ISVCEncoder* encoder) const { destroy_(encoder); }
    [[nodiscard]] Abi layout() const noexcept { return layout_; }

private:
    [[nodiscard]] static Result<Library> open_one(const std::string& name);

    void close() noexcept
    {
        if (handle_ != nullptr) {
            dlclose(handle_);
            handle_ = nullptr;
        }
    }

    void* handle_ = nullptr;
    abi::WelsCreateSVCEncoderFn create_ = nullptr;
    abi::WelsDestroySVCEncoderFn destroy_ = nullptr;
    Abi layout_ = Abi::v2_6;
};

Result<Library> Library::open_one(const std::string& name)
{
    Library lib;
    lib.handle_ = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (lib.handle_ == nullptr) {
        const char* reason = dlerror();
        log::debug(component, "dlopen {}: {}", name, reason != nullptr ? reason : "unknown error");
        return fail(Errc::unsupported, not_found);
    }
    const auto get_version = symbol<abi::WelsGetCodecVersionExFn>(lib.handle_, "WelsGetCodecVersionEx");
    lib.create_ = symbol<abi::WelsCreateSVCEncoderFn>(lib.handle_, "WelsCreateSVCEncoder");
    lib.destroy_ = symbol<abi::WelsDestroySVCEncoderFn>(lib.handle_, "WelsDestroySVCEncoder");
    if (get_version == nullptr || lib.create_ == nullptr || lib.destroy_ == nullptr) {
        log::warn(component, "{} lacks the OpenH264 encoder entry points", name);
        return fail(Errc::unsupported, "library is not OpenH264");
    }
    abi::OpenH264Version version{};
    get_version(&version);
    if (version.uMajor != 2 || version.uMinor < 1 || version.uMinor > 6) {
        log::warn(component, "{} is OpenH264 {}.{}.{}; farland supports 2.1 to 2.6", name, version.uMajor,
                  version.uMinor, version.uRevision);
        return fail(Errc::unsupported, "unsupported OpenH264 version");
    }
    if (version.uMinor >= 6) {
        lib.layout_ = Abi::v2_6;
    } else if (version.uMinor >= 3) {
        lib.layout_ = Abi::v2_3;
    } else {
        lib.layout_ = Abi::v2_1;
    }
    log::debug(component, "loaded {}: OpenH264 {}.{}.{}", name, version.uMajor, version.uMinor, version.uRevision);
    return lib;
}

Result<Library> Library::open(std::string_view library)
{
    if (!library.empty()) {
        return open_one(std::string(library));
    }
    Error last{Errc::unsupported, not_found};
    for (const std::string_view name : library_names) {
        auto lib = open_one(std::string(name));
        if (lib.has_value()) {
            return lib;
        }
        // A library of the wrong version says more than the names that were missing.
        if (last.what == not_found) {
            last = lib.error();
        }
    }
    return std::unexpected(last);
}

/// Routes OpenH264's trace messages into farland's log.
void trace(void* /*context*/, int level, const char* message) noexcept
{
    try {
        std::string_view text = message != nullptr ? message : "";
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.remove_suffix(1);
        }
        if (level <= abi::wels_log_error) {
            log::error(component, "{}", text);
        } else {
            log::warn(component, "{}", text);
        }
    } catch (const std::exception&) {
        // Logging must not unwind through OpenH264.
        static_cast<void>(0);
    }
}

/// OpenH264 rate control for a RateControl, see RateControl::Mode.
struct RcParams {
    int mode = abi::rc_off_mode;
    int qp = 26;
    int target_bps = 0;
    int max_bps = 0;
    int min_qp = 0;

    friend bool operator==(const RcParams&, const RcParams&) = default;
};

[[nodiscard]] RcParams rc_params(const RateControl& rate)
{
    constexpr int kilo = 1000;
    RcParams rc;
    rc.qp = rate.quality;
    if (rate.mode == RateControl::Mode::constant_quality) {
        if (rate.max_bitrate_kbps == 0) {
            rc.mode = abi::rc_off_mode;
        } else {
            rc.mode = abi::rc_bitrate_mode;
            rc.target_bps = static_cast<int>(rate.max_bitrate_kbps) * kilo;
            rc.max_bps = rc.target_bps;
            rc.min_qp = rate.quality;
        }
    } else {
        rc.mode = abi::rc_bitrate_mode;
        rc.target_bps = static_cast<int>(rate.bitrate_kbps) * kilo;
        rc.max_bps = static_cast<int>(std::max(rate.max_bitrate_kbps, rate.bitrate_kbps)) * kilo;
    }
    return rc;
}

class OpenH264Encoder final : public H264Encoder {
public:
    explicit OpenH264Encoder(Library library) : library_(std::move(library)) {}
    OpenH264Encoder(const OpenH264Encoder&) = delete;
    OpenH264Encoder& operator=(const OpenH264Encoder&) = delete;
    OpenH264Encoder(OpenH264Encoder&&) = delete;
    OpenH264Encoder& operator=(OpenH264Encoder&&) = delete;
    ~OpenH264Encoder() override { close(); }

    [[nodiscard]] Backend backend() const noexcept override { return Backend::openh264; }
    [[nodiscard]] const EncoderConfig& config() const noexcept override { return config_; }
    [[nodiscard]] Result<void> configure(const EncoderConfig& config) override;
    [[nodiscard]] Result<void> set_rate_control(const RateControl& rate) override;
    void request_idr() noexcept override { force_idr_ = true; }
    [[nodiscard]] Result<EncodedFrame> encode(const codec::Yuv420View& picture, const FrameOptions& options) override;

private:
    void close() noexcept;
    void fill(abi::SEncParamExt& p) const;
    template <class Layer>
    [[nodiscard]] Result<EncodedFrame> encode_as(const abi::SSourcePicture& picture);
    [[nodiscard]] std::uint8_t average_qp() const;

    Library library_;
    abi::ISVCEncoder* encoder_ = nullptr;
    EncoderConfig config_;
    RcParams rc_;
    bool force_idr_ = true;
    std::uint64_t frames_ = 0;
    std::optional<std::uint64_t> last_timestamp_ms_;
};

void OpenH264Encoder::close() noexcept
{
    if (encoder_ != nullptr) {
        (*encoder_)->Uninitialize(encoder_);
        library_.destroy(encoder_);
        encoder_ = nullptr;
    }
}

void OpenH264Encoder::fill(abi::SEncParamExt& p) const
{
    p.iUsageType = abi::screen_content_real_time;
    p.iPicWidth = static_cast<int>(config_.width);
    p.iPicHeight = static_cast<int>(config_.height);
    p.fMaxFrameRate = static_cast<float>(config_.fps);
    p.iTemporalLayerNum = 1;
    p.iSpatialLayerNum = 1;
    p.uiIntraPeriod = config_.keyint;
    p.iNumRefFrame = static_cast<int>(config_.reference_frames);
    p.eSpsPpsIdStrategy = abi::constant_id;
    p.bPrefixNalAddingCtrl = false;
    p.bEnableSSEI = false;
    p.bSimulcastAVC = false;
    p.iPaddingFlag = 0;
    p.iEntropyCodingModeFlag = 0;  // CAVLC: Constrained Baseline, see openh264_encoder.hpp.
    // OpenH264 holds a bitrate only by dropping pictures; at a fixed QP every
    // picture produces an access unit.
    p.bEnableFrameSkip = rc_.mode == abi::rc_bitrate_mode;
    // Screen content always runs scene-change detection, and without long-term
    // references every scene change is an IDR. AVC444 alternates two views,
    // which looks like a scene change at every picture; scene LTRs let each
    // view predict from its own last picture instead (video::Avc444Encoder).
    // OpenH264 takes LTRs for screen content only on a lossless link, which
    // RDP over TCP is (param_svc.h ParamTranscode).
    p.bEnableLongTermReference = config_.reference_frames > 1;
    if (p.bEnableLongTermReference) {
        p.iLTRRefNum = static_cast<int>(config_.reference_frames);
        p.bIsLosslessLink = true;
    }
    p.iMultipleThreadIdc = static_cast<unsigned short>(config_.threads);
    p.bEnableDenoise = false;
    // Not supported for screen content; OpenH264 would switch them off with a warning.
    p.bEnableBackgroundDetection = false;
    p.bEnableAdaptiveQuant = false;
    p.bEnableSceneChangeDetect = false;  // IDRs only when asked for.
    p.bEnableFrameCroppingFlag = true;

    p.iRCMode = rc_.mode;
    p.iTargetBitrate = rc_.target_bps;
    p.iMaxBitrate = rc_.max_bps;
    if (rc_.min_qp > 0) {
        p.iMinQp = rc_.min_qp;
    }

    abi::SSpatialLayerConfig& layer = p.sSpatialLayers[0];
    layer.iVideoWidth = p.iPicWidth;
    layer.iVideoHeight = p.iPicHeight;
    layer.fFrameRate = p.fMaxFrameRate;
    layer.iSpatialBitrate = rc_.target_bps;
    layer.iMaxSpatialBitrate = rc_.max_bps;
    layer.uiProfileIdc = abi::pro_baseline;
    layer.iDLayerQp = rc_.qp;
    if (config_.threads > 1) {
        layer.sSliceArgument.uiSliceMode = abi::sm_fixedslcnum_slice;
        layer.sSliceArgument.uiSliceNum = config_.threads;
    } else {
        layer.sSliceArgument.uiSliceMode = abi::sm_single_slice;
        layer.sSliceArgument.uiSliceNum = 1;
    }
    // Full-range BT.709 ([MS-RDPEGFX] 3.3.8.3.1), sRGB transfer.
    layer.bVideoSignalTypePresent = true;
    layer.uiVideoFormat = abi::vf_undef;
    layer.bFullRange = true;
    layer.bColorDescriptionPresent = true;
    layer.uiColorPrimaries = abi::cp_bt709;
    layer.uiTransferCharacteristics = abi::trc_iec61966_2_1;
    layer.uiColorMatrix = abi::cm_bt709;
}

Result<void> OpenH264Encoder::configure(const EncoderConfig& config)
{
    FARLAND_TRY_VOID(validate(config));
    close();
    config_ = config;
    rc_ = rc_params(config.rate);

    if (library_.create(&encoder_) != 0 || encoder_ == nullptr) {
        encoder_ = nullptr;
        return fail(Errc::io, "WelsCreateSVCEncoder failed");
    }
    abi::WelsTraceCallback callback = &trace;
    // Warnings are about parameters OpenH264 adjusts on its own; keep errors.
    int level = abi::wels_log_error;
    (*encoder_)->SetOption(encoder_, abi::option::trace_callback, static_cast<void*>(&callback));
    (*encoder_)->SetOption(encoder_, abi::option::trace_level, &level);

    abi::SEncParamExt params{};
    if ((*encoder_)->GetDefaultParams(encoder_, &params) != abi::cm_result_success) {
        close();
        return fail(Errc::io, "OpenH264 GetDefaultParams failed");
    }
    fill(params);
    if ((*encoder_)->InitializeExt(encoder_, &params) != abi::cm_result_success) {
        close();
        return fail(Errc::invalid_value, "OpenH264 rejected the encoder configuration");
    }
    force_idr_ = true;
    frames_ = 0;
    last_timestamp_ms_.reset();
    return {};
}

Result<void> OpenH264Encoder::set_rate_control(const RateControl& rate)
{
    FARLAND_TRY_VOID(validate(rate));
    if (encoder_ == nullptr) {
        return fail(Errc::io, "OpenH264 encoder is not configured");
    }
    const RcParams next = rc_params(rate);
    if (next == rc_) {
        config_.rate = rate;
        return {};
    }
    // Bitrate changes apply to the running stream; mode or QP changes re-open it.
    if (next.mode == abi::rc_bitrate_mode && rc_.mode == abi::rc_bitrate_mode && next.min_qp == rc_.min_qp) {
        abi::SBitrateInfo target{.iLayer = abi::spatial_layer_all, .iBitrate = next.target_bps};
        abi::SBitrateInfo peak{.iLayer = abi::spatial_layer_all, .iBitrate = next.max_bps};
        if ((*encoder_)->SetOption(encoder_, abi::option::bitrate, &target) != abi::cm_result_success ||
            (*encoder_)->SetOption(encoder_, abi::option::max_bitrate, &peak) != abi::cm_result_success) {
            return fail(Errc::invalid_value, "OpenH264 rejected the bitrate");
        }
        rc_ = next;
        config_.rate = rate;
        return {};
    }
    EncoderConfig config = config_;
    config.rate = rate;
    return configure(config);
}

std::uint8_t OpenH264Encoder::average_qp() const
{
    abi::SEncoderStatistics stats{};
    if ((*encoder_)->GetOption(encoder_, abi::option::get_statistics, &stats) != abi::cm_result_success) {
        return static_cast<std::uint8_t>(rc_.qp);
    }
    return static_cast<std::uint8_t>(std::min(stats.uiAverageFrameQP, 51U));
}

template <class Layer>
Result<EncodedFrame> OpenH264Encoder::encode_as(const abi::SSourcePicture& picture)
{
    auto info = std::make_unique<abi::SFrameBSInfo<Layer>>();
    if ((*encoder_)->EncodeFrame(encoder_, &picture, info.get()) != abi::cm_result_success) {
        force_idr_ = true;
        return fail(Errc::io, "OpenH264 EncodeFrame failed");
    }
    EncodedFrame frame;
    if (info->eFrameType == abi::video_frame_type_skip || info->eFrameType == abi::video_frame_type_invalid) {
        return frame;
    }
    if (info->iLayerNum < 0 || info->iLayerNum > abi::max_layer_num_of_frame) {
        force_idr_ = true;
        return fail(Errc::io, "OpenH264 returned an invalid layer count");
    }
    frame.idr = info->eFrameType == abi::video_frame_type_idr;
    if (config_.access_unit_delimiters) {
        // access_unit_delimiter_rbsp ([ITU-H.264-201201] 7.3.2.4): primary_pic_type
        // 0 (I slices) or 1 (I and P slices), then the RBSP stop bit.
        const bool intra = frame.idr || info->eFrameType == abi::video_frame_type_i;
        frame.bitstream = {std::byte{0}, std::byte{0},    std::byte{0},
                           std::byte{1}, std::byte{0x09}, intra ? std::byte{0x10} : std::byte{0x30}};
    }
    const auto layers = std::span(info->sLayerInfo).first(static_cast<std::size_t>(info->iLayerNum));
    for (const Layer& layer : layers) {
        if (layer.iNalCount <= 0) {
            continue;
        }
        if (layer.pNalLengthInByte == nullptr || layer.pBsBuf == nullptr) {
            force_idr_ = true;
            return fail(Errc::io, "OpenH264 returned a layer without data");
        }
        std::size_t size = 0;
        for (const int length : std::span(layer.pNalLengthInByte, static_cast<std::size_t>(layer.iNalCount))) {
            size += static_cast<std::size_t>(std::max(length, 0));
        }
        const auto bytes = std::as_bytes(std::span(layer.pBsBuf, size));
        frame.bitstream.insert(frame.bitstream.end(), bytes.begin(), bytes.end());
    }
    frame.qp = average_qp();
    return frame;
}

Result<EncodedFrame> OpenH264Encoder::encode(const codec::Yuv420View& picture, const FrameOptions& options)
{
    if (encoder_ == nullptr) {
        return fail(Errc::io, "OpenH264 encoder is not configured");
    }
    FARLAND_ASSERT(picture.width == config_.width && picture.height == config_.height);
    FARLAND_ASSERT(picture.y_stride >= picture.width && picture.uv_stride >= picture.width / 2U);
    FARLAND_ASSERT(picture.y.size() >= picture.y_stride * picture.height);
    FARLAND_ASSERT(picture.u.size() >= picture.uv_stride * (picture.height / 2U));
    FARLAND_ASSERT(picture.v.size() >= picture.uv_stride * (picture.height / 2U));

    // OpenH264 time stamps are in milliseconds and must increase.
    std::uint64_t timestamp_ms =
        options.timestamp_us.has_value() ? *options.timestamp_us / 1000U : frames_ * 1000U / config_.fps;
    if (last_timestamp_ms_.has_value() && timestamp_ms <= *last_timestamp_ms_) {
        timestamp_ms = *last_timestamp_ms_ + 1U;
    }

    abi::SSourcePicture source{};
    source.iColorFormat = abi::video_format_i420;
    source.iStride = {static_cast<int>(picture.y_stride), static_cast<int>(picture.uv_stride),
                      static_cast<int>(picture.uv_stride), 0};
    // OpenH264 takes non-const plane pointers but only reads the source picture.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast,cppcoreguidelines-pro-type-reinterpret-cast)
    source.pData = {reinterpret_cast<unsigned char*>(const_cast<std::byte*>(picture.y.data())),
                    reinterpret_cast<unsigned char*>(const_cast<std::byte*>(picture.u.data())),
                    reinterpret_cast<unsigned char*>(const_cast<std::byte*>(picture.v.data())), nullptr};
    // NOLINTEND(cppcoreguidelines-pro-type-const-cast,cppcoreguidelines-pro-type-reinterpret-cast)
    source.iPicWidth = static_cast<int>(picture.width);
    source.iPicHeight = static_cast<int>(picture.height);
    source.uiTimeStamp = static_cast<long long>(timestamp_ms);

    if (force_idr_ || options.force_idr) {
        if ((*encoder_)->ForceIntraFrame(encoder_, true, -1) != 0) {
            return fail(Errc::io, "OpenH264 ForceIntraFrame failed");
        }
    }
    auto frame = library_.layout() == Abi::v2_6 ? encode_as<abi::SLayerBSInfo26>(source)
                                                : encode_as<abi::SLayerBSInfo21>(source);
    if (frame.has_value()) {
        force_idr_ = false;
        ++frames_;
        last_timestamp_ms_ = timestamp_ms;
    }
    return frame;
}

}  // namespace

std::span<const std::string_view> default_library_names() noexcept
{
    return library_names;
}

Result<std::unique_ptr<H264Encoder>> create(const EncoderConfig& config, std::string_view library)
{
    FARLAND_TRY(Library lib, Library::open(library));
    auto encoder = std::make_unique<OpenH264Encoder>(std::move(lib));
    FARLAND_TRY_VOID(encoder->configure(config));
    return encoder;
}

}  // namespace farland::video::openh264
