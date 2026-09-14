// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// VA-API backend. The parameter buffers follow libva's va_enc_h264.h and
// va_vpp.h; the choices where drivers differ are explained in
// vaapi_encoder.hpp.

#include <farland/base/assert.hpp>
#include <farland/base/log.hpp>
#include <farland/codec/h264_nal.hpp>
#include <farland/video/h264_bitstream.hpp>
#include <farland/video/vaapi_encoder.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_h264.h>
#include <va/va_vpp.h>
#include <vector>

namespace farland::video::vaapi {

namespace {

constexpr std::string_view component = "video.vaapi";
/// Dmabuf imports kept as VA surfaces. Compositors cycle through 2 to 4
/// buffers per stream.
constexpr std::size_t max_imports = 8;
constexpr std::uint32_t log2_max_frame_num = 16;
constexpr std::uint32_t log2_max_poc_lsb = 16;
/// Keeps picture order counts (2 per frame) far inside int32_t.
constexpr std::uint32_t max_frames_between_idr = 1U << 29U;
/// intra_period when IDRs come only on request; drivers budget I-frame bits by it.
constexpr std::uint32_t idle_intra_period = 3600;
/// The encoder's input surface, then the two reconstructed pictures it alternates.
constexpr std::size_t input_surface = 0;
constexpr std::size_t surface_count = 3;

constexpr std::uint8_t slice_type_p = 0;
constexpr std::uint8_t slice_type_i = 2;

bool succeeded(VAStatus status, std::string_view call)
{
    if (status == VA_STATUS_SUCCESS) {
        return true;
    }
    log::warn(component, "{} failed: {}", call, vaErrorStr(status));
    return false;
}

std::string_view trim(const char* message)
{
    std::string_view text = message != nullptr ? message : "";
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

/// libva's messages ("libva info: VA-API version ...") go to our log, not stderr.
void info_callback(void* /*context*/, const char* message) noexcept
{
    try {
        log::debug(component, "{}", trim(message));
    } catch (const std::exception&) {
        static_cast<void>(0);  // Logging must not unwind through libva.
    }
}

void error_callback(void* /*context*/, const char* message) noexcept
{
    try {
        log::warn(component, "{}", trim(message));
    } catch (const std::exception&) {
        static_cast<void>(0);
    }
}

[[nodiscard]] VAProfile va_profile(Profile profile)
{
    switch (profile) {
    case Profile::constrained_baseline:
        return VAProfileH264ConstrainedBaseline;
    case Profile::main:
        return VAProfileH264Main;
    case Profile::high:
        return VAProfileH264High;
    }
    return VAProfileH264ConstrainedBaseline;
}

[[nodiscard]] std::string_view profile_name(Profile profile)
{
    switch (profile) {
    case Profile::constrained_baseline:
        return "constrained baseline";
    case Profile::main:
        return "main";
    case Profile::high:
        return "high";
    }
    return "?";
}

[[nodiscard]] std::string rate_control_names(std::uint32_t modes)
{
    constexpr std::array<std::pair<std::uint32_t, std::string_view>, 8> names{{
        {VA_RC_CBR, "CBR"},
        {VA_RC_VBR, "VBR"},
        {VA_RC_CQP, "CQP"},
        {VA_RC_VBR_CONSTRAINED, "VBR_CONSTRAINED"},
        {VA_RC_ICQ, "ICQ"},
        {VA_RC_QVBR, "QVBR"},
        {VA_RC_AVBR, "AVBR"},
        {VA_RC_TCBRC, "TCBRC"},
    }};
    std::string text;
    for (const auto& [bit, name] : names) {
        if ((modes & bit) != 0) {
            text += text.empty() ? "" : " ";
            text += name;
        }
    }
    return text.empty() ? "none" : text;
}

[[nodiscard]] std::string packed_header_names(std::uint32_t packed)
{
    constexpr std::array<std::pair<std::uint32_t, std::string_view>, 5> names{{
        {VA_ENC_PACKED_HEADER_SEQUENCE, "sequence"},
        {VA_ENC_PACKED_HEADER_PICTURE, "picture"},
        {VA_ENC_PACKED_HEADER_SLICE, "slice"},
        {VA_ENC_PACKED_HEADER_MISC, "misc"},
        {VA_ENC_PACKED_HEADER_RAW_DATA, "raw"},
    }};
    std::string text;
    for (const auto& [bit, name] : names) {
        if ((packed & bit) != 0) {
            text += text.empty() ? "" : " ";
            text += name;
        }
    }
    return text.empty() ? "none" : text;
}

[[nodiscard]] std::uint32_t attribute_value(const VAConfigAttrib& attribute)
{
    return attribute.value == VA_ATTRIB_NOT_SUPPORTED ? 0U : attribute.value;
}

/// The VA fourcc with the memory layout of a DRM fourcc (both on little-endian
/// machines: DRM names the bits of a word, VA the bytes in memory).
[[nodiscard]] Result<std::uint32_t> va_fourcc(std::uint32_t drm)
{
    switch (drm) {
    case drm_fourcc::xrgb8888:
        return std::uint32_t{VA_FOURCC_BGRX};
    case drm_fourcc::argb8888:
        return std::uint32_t{VA_FOURCC_BGRA};
    case drm_fourcc::xbgr8888:
        return std::uint32_t{VA_FOURCC_RGBX};
    case drm_fourcc::abgr8888:
        return std::uint32_t{VA_FOURCC_RGBA};
    case drm_fourcc::rgbx8888:
        return std::uint32_t{VA_FOURCC_XBGR};
    case drm_fourcc::rgba8888:
        return std::uint32_t{VA_FOURCC_ABGR};
    case drm_fourcc::bgrx8888:
        return std::uint32_t{VA_FOURCC_XRGB};
    case drm_fourcc::bgra8888:
        return std::uint32_t{VA_FOURCC_ARGB};
    default:
        return fail(Errc::unsupported, "the dmabuf format is not packed 32-bit RGB");
    }
}

[[nodiscard]] VAPictureH264 invalid_picture()
{
    VAPictureH264 picture{};
    picture.picture_id = VA_INVALID_SURFACE;
    picture.flags = VA_PICTURE_H264_INVALID;
    return picture;
}

/// An initialised VADisplay on an open render node.
class Display {
public:
    Display() = default;
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;
    Display(Display&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)), display_(std::exchange(other.display_, nullptr)),
          path_(std::move(other.path_))
    {
    }
    Display& operator=(Display&&) = delete;
    ~Display()
    {
        if (display_ != nullptr) {
            vaTerminate(display_);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    [[nodiscard]] static Result<Display> open(const std::string& path)
    {
        Display display;
        display.fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);  // NOLINT(cppcoreguidelines-pro-type-vararg)
        if (display.fd_ < 0) {
            log::debug(component, "cannot open {}: {}", path, std::strerror(errno));
            return fail(Errc::unsupported, "cannot open the DRM render node");
        }
        display.display_ = vaGetDisplayDRM(display.fd_);
        if (display.display_ == nullptr) {
            return fail(Errc::unsupported, "libva has no display for the DRM render node");
        }
        // Errors while probing (the wrong driver for a node) are only debug output.
        vaSetErrorCallback(display.display_, &info_callback, nullptr);
        vaSetInfoCallback(display.display_, &info_callback, nullptr);
        int major = 0;
        int minor = 0;
        const VAStatus status = vaInitialize(display.display_, &major, &minor);
        if (status != VA_STATUS_SUCCESS) {
            log::debug(component, "vaInitialize on {}: {}", path, vaErrorStr(status));
            return fail(Errc::unsupported, "no VA-API driver for the DRM render node");
        }
        vaSetErrorCallback(display.display_, &error_callback, nullptr);
        display.path_ = path;
        return display;
    }

    [[nodiscard]] VADisplay get() const noexcept { return display_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    int fd_ = -1;
    VADisplay display_ = nullptr;
    std::string path_;
};

[[nodiscard]] std::vector<VAEntrypoint> entrypoints(VADisplay display, VAProfile profile)
{
    std::vector<VAEntrypoint> list(static_cast<std::size_t>(std::max(vaMaxNumEntrypoints(display), 1)));
    int count = 0;
    if (vaQueryConfigEntrypoints(display, profile, list.data(), &count) != VA_STATUS_SUCCESS) {
        return {};
    }
    list.resize(static_cast<std::size_t>(std::clamp(count, 0, static_cast<int>(list.size()))));
    return list;
}

/// EncSlice where the driver has it (Mesa, older Intel), else EncSliceLP
/// (newer Intel GPUs have only the low-power VDEnc).
[[nodiscard]] std::optional<VAEntrypoint> encode_entrypoint(VADisplay display, VAProfile profile)
{
    const auto list = entrypoints(display, profile);
    for (const VAEntrypoint wanted : {VAEntrypointEncSlice, VAEntrypointEncSliceLP}) {
        if (std::ranges::find(list, wanted) != list.end()) {
            return wanted;
        }
    }
    return std::nullopt;
}

[[nodiscard]] Result<DeviceInfo> query_device(const Display& display)
{
    VADisplay dpy = display.get();
    DeviceInfo info;
    info.render_node = display.path();
    const char* vendor = vaQueryVendorString(dpy);
    info.vendor = vendor != nullptr ? vendor : "";

    std::vector<VAProfile> profiles(static_cast<std::size_t>(std::max(vaMaxNumProfiles(dpy), 1)));
    int count = 0;
    if (vaQueryConfigProfiles(dpy, profiles.data(), &count) != VA_STATUS_SUCCESS) {
        return fail(Errc::unsupported, "the VA-API driver lists no profiles");
    }
    profiles.resize(static_cast<std::size_t>(std::clamp(count, 0, static_cast<int>(profiles.size()))));
    std::optional<std::pair<VAProfile, VAEntrypoint>> first;
    for (const Profile profile : {Profile::constrained_baseline, Profile::main, Profile::high}) {
        if (std::ranges::find(profiles, va_profile(profile)) == profiles.end()) {
            continue;
        }
        const auto list = entrypoints(dpy, va_profile(profile));
        const bool slice = std::ranges::find(list, VAEntrypointEncSlice) != list.end();
        const bool slice_lp = std::ranges::find(list, VAEntrypointEncSliceLP) != list.end();
        info.enc_slice = info.enc_slice || slice;
        info.enc_slice_lp = info.enc_slice_lp || slice_lp;
        if (slice || slice_lp) {
            info.profiles.push_back(profile);
            if (!first) {
                first.emplace(va_profile(profile), slice ? VAEntrypointEncSlice : VAEntrypointEncSliceLP);
            }
        }
    }
    if (!first) {
        return fail(Errc::unsupported, "the VA-API driver cannot encode H.264");
    }
    const auto processing = entrypoints(dpy, VAProfileNone);
    info.video_proc = std::ranges::find(processing, VAEntrypointVideoProc) != processing.end();

    std::array<VAConfigAttrib, 2> attributes{{{VAConfigAttribRateControl, 0}, {VAConfigAttribEncPackedHeaders, 0}}};
    if (vaGetConfigAttributes(dpy, first->first, first->second, attributes.data(),
                              static_cast<int>(attributes.size())) == VA_STATUS_SUCCESS) {
        info.rate_control_modes = attribute_value(attributes[0]);
        info.packed_headers = attribute_value(attributes[1]);
    }
    return info;
}

[[nodiscard]] Result<std::pair<Display, DeviceInfo>> open_device(const std::string& render_node)
{
    if (!render_node.empty()) {
        FARLAND_TRY(Display display, Display::open(render_node));
        FARLAND_TRY(DeviceInfo info, query_device(display));
        return std::pair{std::move(display), std::move(info)};
    }
    for (int minor = 128; minor < 192; ++minor) {
        const std::string path = std::format("/dev/dri/renderD{}", minor);
        if (::access(path.c_str(), F_OK) != 0) {
            continue;
        }
        auto display = Display::open(path);
        if (!display) {
            continue;
        }
        auto info = query_device(*display);
        if (!info) {
            log::debug(component, "{}: {}", path, info.error().message());
            continue;
        }
        return std::pair{std::move(*display), std::move(*info)};
    }
    return fail(Errc::unsupported, "no DRM render node with a VA-API H.264 encoder");
}

/// VA-API rate control for a RateControl, see vaapi_encoder.hpp.
struct RcParams {
    std::uint32_t mode = VA_RC_CQP;
    std::uint8_t qp = 23;
    /// CBR: the bitrate; VBR: the peak, with the target at target_percentage.
    std::uint32_t bits_per_second = 0;
    std::uint32_t target_percentage = 100;
    std::uint8_t min_qp = 0;
    std::uint32_t hrd_bits = 0;

    friend bool operator==(const RcParams&, const RcParams&) = default;
};

[[nodiscard]] Result<RcParams> rc_params(const RateControl& rate, std::uint32_t supported)
{
    const auto has = [supported](std::uint32_t mode) { return (supported & mode) != 0; };
    RcParams rc;
    rc.qp = rate.quality;
    std::uint64_t cap_kbps = 0;
    if (rate.mode == RateControl::Mode::constant_quality) {
        if (rate.max_bitrate_kbps == 0) {
            if (!has(VA_RC_CQP)) {
                return fail(Errc::unsupported, "the VA-API encoder has no constant-QP rate control");
            }
            return rc;
        }
        cap_kbps = rate.max_bitrate_kbps;
        rc.min_qp = rate.quality;
        rc.mode = has(VA_RC_VBR) ? VA_RC_VBR : VA_RC_CBR;
    } else {
        cap_kbps = std::max(rate.max_bitrate_kbps, rate.bitrate_kbps);
        if (cap_kbps > rate.bitrate_kbps && has(VA_RC_VBR)) {
            rc.mode = VA_RC_VBR;
            rc.target_percentage = static_cast<std::uint32_t>(
                std::max<std::uint64_t>(1, std::uint64_t{rate.bitrate_kbps} * 100U / cap_kbps));
        } else {
            rc.mode = has(VA_RC_CBR) ? VA_RC_CBR : VA_RC_VBR;
            cap_kbps = rate.bitrate_kbps;
        }
    }
    if (!has(rc.mode)) {
        return fail(Errc::unsupported, "the VA-API encoder has no bitrate control");
    }
    rc.bits_per_second = static_cast<std::uint32_t>(cap_kbps * 1000U);
    rc.hrd_bits = static_cast<std::uint32_t>(std::max<std::uint64_t>(1, cap_kbps * rate.vbv_window_ms));
    return rc;
}

/// The parameter buffers of one picture, destroyed with it.
class Buffers {
public:
    explicit Buffers(VADisplay display) noexcept : display_(display) {}
    Buffers(const Buffers&) = delete;
    Buffers& operator=(const Buffers&) = delete;
    Buffers(Buffers&&) = delete;
    Buffers& operator=(Buffers&&) = delete;
    ~Buffers()
    {
        for (const VABufferID id : ids_) {
            vaDestroyBuffer(display_, id);
        }
    }

    [[nodiscard]] Result<void> add(VAContextID context, VABufferType type, const void* data, std::size_t size)
    {
        VABufferID id = VA_INVALID_ID;
        // libva copies the data but takes it as non-const.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        if (!succeeded(
                vaCreateBuffer(display_, context, type, static_cast<unsigned>(size), 1, const_cast<void*>(data), &id),
                "vaCreateBuffer")) {
            return fail(Errc::io, "VA-API could not create a parameter buffer");
        }
        ids_.push_back(id);
        return {};
    }

    template <class T>
    [[nodiscard]] Result<void> add(VAContextID context, VABufferType type, const T& value)
    {
        return add(context, type, &value, sizeof(T));
    }

    /// A VAEncMiscParameterBuffer: the type, then the parameters.
    template <class T>
    [[nodiscard]] Result<void> add_misc(VAContextID context, VAEncMiscParameterType type, const T& value)
    {
        static_assert(sizeof(VAEncMiscParameterBuffer) == sizeof(std::uint32_t));
        std::vector<std::uint32_t> storage(1 + ((sizeof(T) + 3) / sizeof(std::uint32_t)));
        storage[0] = static_cast<std::uint32_t>(type);
        std::memcpy(&storage[1], &value, sizeof(T));
        return add(context, VAEncMiscParameterBufferType, storage.data(), storage.size() * sizeof(std::uint32_t));
    }

    /// A packed header: one NAL unit with its start code, emulation prevention
    /// included, `nal_bits` long (0: all of `nal`).
    [[nodiscard]] Result<void> add_packed(VAContextID context, std::uint32_t type, std::span<const std::byte> nal,
                                          std::size_t nal_bits = 0)
    {
        std::vector<std::byte> data{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};
        data.insert(data.end(), nal.begin(), nal.end());
        VAEncPackedHeaderParameterBuffer header{};
        header.type = type;
        header.bit_length = static_cast<std::uint32_t>(32U + (nal_bits != 0 ? nal_bits : nal.size() * 8U));
        header.has_emulation_bytes = 1;
        FARLAND_TRY_VOID(add(context, VAEncPackedHeaderParameterBufferType, header));
        return add(context, VAEncPackedHeaderDataBufferType, data.data(), data.size());
    }

    [[nodiscard]] std::span<VABufferID> ids() noexcept { return ids_; }

private:
    VADisplay display_;
    std::vector<VABufferID> ids_;
};

/// A mapped VA buffer, unmapped when destroyed.
class MappedBuffer {
public:
    MappedBuffer(VADisplay display, VABufferID buffer) : display_(display), buffer_(buffer)
    {
        if (!succeeded(vaMapBuffer(display, buffer, &data_), "vaMapBuffer")) {
            data_ = nullptr;
        }
    }
    MappedBuffer(const MappedBuffer&) = delete;
    MappedBuffer& operator=(const MappedBuffer&) = delete;
    MappedBuffer(MappedBuffer&&) = delete;
    MappedBuffer& operator=(MappedBuffer&&) = delete;
    ~MappedBuffer()
    {
        if (data_ != nullptr) {
            vaUnmapBuffer(display_, buffer_);
        }
    }

    [[nodiscard]] void* data() const noexcept { return data_; }

private:
    VADisplay display_;
    VABufferID buffer_;
    void* data_ = nullptr;
};

/// What identifies an imported dmabuf: the buffers behind its descriptors
/// (descriptor numbers are reused, inodes of dma-buf files are not) and the
/// layout.
struct ImportKey {
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t planes = 0;
    std::array<std::uint64_t, 4> devices{};
    std::array<std::uint64_t, 4> inodes{};
    std::array<std::uint32_t, 4> offsets{};
    std::array<std::uint32_t, 4> pitches{};

    friend bool operator==(const ImportKey&, const ImportKey&) = default;
};

struct Import {
    ImportKey key;
    VASurfaceID surface = VA_INVALID_SURFACE;
    std::uint64_t last_used = 0;
};

}  // namespace

struct VaapiEncoder::Impl {
    Impl(Display display_in, DeviceInfo device_in, BackendOptions options_in)
        : display(std::move(display_in)), device(std::move(device_in)), options(std::move(options_in))
    {
        const char* packed_env = std::getenv("FARLAND_VAAPI_PACKED_HEADERS");  // NOLINT(concurrency-mt-unsafe)
        packed_allowed = packed_env == nullptr || std::string_view(packed_env) != "0";
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl()
    {
        close_session();
        drop_imports();
    }

    [[nodiscard]] VADisplay dpy() const noexcept { return display.get(); }
    [[nodiscard]] bool is_open() const noexcept { return context != VA_INVALID_ID; }
    [[nodiscard]] std::uint32_t width_mbs() const noexcept { return config.width / 16U; }
    [[nodiscard]] std::uint32_t height_mbs() const noexcept { return config.height / 16U; }
    [[nodiscard]] bitstream::Vui vui() const noexcept
    {
        return {.color = options.color, .fps = config.fps, .max_dec_frame_buffering = 1};
    }

    [[nodiscard]] Result<void> open_session(const EncoderConfig& next);
    void close_session() noexcept;
    [[nodiscard]] Result<void> set_rate_control(const RateControl& rate);
    [[nodiscard]] Result<void> ensure_image();
    [[nodiscard]] Result<void> clear_input();
    [[nodiscard]] Result<void> upload(const codec::Yuv420View& picture);
    [[nodiscard]] Result<void> ensure_vpp();
    [[nodiscard]] Result<VASurfaceID> import(const DmabufFrame& frame);
    void drop_imports() noexcept;
    [[nodiscard]] Result<void> convert(const DmabufFrame& frame);
    [[nodiscard]] Result<codec::Yuv420Frame> read_input();
    [[nodiscard]] Result<void> render(VAContextID target_context, VASurfaceID target, Buffers& buffers);
    [[nodiscard]] Result<void> add_rate_control(Buffers& buffers, bool reset) const;
    [[nodiscard]] VAEncSequenceParameterBufferH264 sequence_params() const;
    [[nodiscard]] Result<void> read_coded();
    [[nodiscard]] Result<EncodedFrame> encode_input(const FrameOptions& options);
    void log_coded(std::string_view what) const;

    Display display;
    DeviceInfo device;
    BackendOptions options;
    bool packed_allowed = true;

    EncoderConfig config;
    VAEntrypoint entrypoint = VAEntrypointEncSlice;
    std::uint32_t rc_modes = 0;
    std::uint32_t packed = 0;
    VAConfigID config_id = VA_INVALID_ID;
    VAContextID context = VA_INVALID_ID;
    std::array<VASurfaceID, surface_count> surfaces{VA_INVALID_SURFACE, VA_INVALID_SURFACE, VA_INVALID_SURFACE};
    VABufferID coded = VA_INVALID_ID;
    VAConfigID vpp_config = VA_INVALID_ID;
    VAContextID vpp_context = VA_INVALID_ID;
    /// NV12 staging image for CPU uploads and read-backs.
    std::optional<VAImage> image;
    std::vector<Import> imports;
    std::uint64_t import_clock = 0;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> failed_imports;

    RcParams rc;
    bool rc_dirty = false;
    std::uint8_t pic_init_qp = 26;
    std::uint8_t level_idc = 40;
    bool force_idr = true;
    std::uint32_t frames_since_idr = 0;
    std::uint16_t idr_pic_id = 0;
    std::size_t recon = 0;
    std::uint32_t previous_frame_num = 0;
    std::int32_t previous_poc = 0;
    bitstream::AccessUnitWriter writer;
    std::vector<std::byte> coded_bytes;
};

Result<void> VaapiEncoder::Impl::open_session(const EncoderConfig& next)
{
    close_session();
    const VAProfile profile = va_profile(next.profile);
    if (std::ranges::find(device.profiles, next.profile) == device.profiles.end()) {
        return fail(Errc::unsupported, "the VA-API driver cannot encode this H.264 profile");
    }
    const auto chosen = encode_entrypoint(dpy(), profile);
    if (!chosen) {
        return fail(Errc::unsupported, "the VA-API driver cannot encode this H.264 profile");
    }
    entrypoint = *chosen;

    std::array<VAConfigAttrib, 3> query{{
        {VAConfigAttribRTFormat, 0},
        {VAConfigAttribRateControl, 0},
        {VAConfigAttribEncPackedHeaders, 0},
    }};
    if (!succeeded(vaGetConfigAttributes(dpy(), profile, entrypoint, query.data(), static_cast<int>(query.size())),
                   "vaGetConfigAttributes")) {
        return fail(Errc::unsupported, "the VA-API driver does not describe its H.264 encoder");
    }
    if ((attribute_value(query[0]) & VA_RT_FORMAT_YUV420) == 0) {
        return fail(Errc::unsupported, "the VA-API encoder takes no 4:2:0 pictures");
    }
    rc_modes = attribute_value(query[1]);
    FARLAND_TRY(rc, rc_params(next.rate, rc_modes));
    // Packed headers where the driver takes them, as ffmpeg does: SPS, PPS
    // and slice headers. Mesa 26's radeonsi takes the parameter sets and the
    // slice's NAL header only from packed headers; intel-media-driver inserts
    // them as given. Without them the driver writes its own.
    packed = packed_allowed ? attribute_value(query[2]) & (VA_ENC_PACKED_HEADER_SEQUENCE |
                                                           VA_ENC_PACKED_HEADER_PICTURE | VA_ENC_PACKED_HEADER_SLICE)
                            : 0U;

    std::vector<VAConfigAttrib> attributes{{VAConfigAttribRTFormat, VA_RT_FORMAT_YUV420},
                                           {VAConfigAttribRateControl, rc.mode}};
    if (packed != 0) {
        attributes.push_back({VAConfigAttribEncPackedHeaders, packed});
    }
    if (!succeeded(vaCreateConfig(dpy(), profile, entrypoint, attributes.data(), static_cast<int>(attributes.size()),
                                  &config_id),
                   "vaCreateConfig")) {
        config_id = VA_INVALID_ID;
        return fail(Errc::unsupported, "the VA-API driver rejected the encoder configuration");
    }
    VASurfaceAttrib format{};
    format.type = VASurfaceAttribPixelFormat;
    format.flags = VA_SURFACE_ATTRIB_SETTABLE;
    format.value.type = VAGenericValueTypeInteger;
    format.value.value.i = static_cast<std::int32_t>(VA_FOURCC_NV12);
    if (!succeeded(vaCreateSurfaces(dpy(), VA_RT_FORMAT_YUV420, next.width, next.height, surfaces.data(),
                                    static_cast<unsigned>(surfaces.size()), &format, 1),
                   "vaCreateSurfaces")) {
        surfaces.fill(VA_INVALID_SURFACE);
        return fail(Errc::io, "VA-API could not create the encoder's surfaces");
    }
    if (!succeeded(vaCreateContext(dpy(), config_id, static_cast<int>(next.width), static_cast<int>(next.height),
                                   VA_PROGRESSIVE, surfaces.data(), static_cast<int>(surfaces.size()), &context),
                   "vaCreateContext")) {
        context = VA_INVALID_ID;
        return fail(Errc::unsupported, "the VA-API driver cannot encode pictures of this size");
    }
    // Room for the worst case: every macroblock as PCM (384 bytes) plus headers.
    const std::size_t coded_size = (std::size_t{next.width} * next.height * 2U) + (std::size_t{64} * 1024U);
    if (!succeeded(
            vaCreateBuffer(dpy(), context, VAEncCodedBufferType, static_cast<unsigned>(coded_size), 1, nullptr, &coded),
            "vaCreateBuffer")) {
        coded = VA_INVALID_ID;
        close_session();
        return fail(Errc::io, "VA-API could not create the coded buffer");
    }

    config = next;
    const std::uint32_t level_kbps = rc.mode == VA_RC_CQP ? 0U : rc.bits_per_second / 1000U;
    level_idc = bitstream::level_for(width_mbs(), height_mbs(), config.fps, level_kbps, config.profile);
    pic_init_qp = rc.mode == VA_RC_CQP ? rc.qp : std::uint8_t{26};
    writer.reset({.access_unit_delimiters = config.access_unit_delimiters, .vui = vui()});
    rc_dirty = false;
    force_idr = true;
    frames_since_idr = 0;
    idr_pic_id = 0;
    recon = 0;
    if (auto cleared = clear_input(); !cleared) {
        close_session();
        return cleared;
    }
    log::info(component, "{}: {}x{} H.264 {} level {}.{} through {}, rate control {} (driver: {}), packed headers {}",
              device.render_node, config.width, config.height, profile_name(config.profile), level_idc / 10,
              level_idc % 10, entrypoint == VAEntrypointEncSliceLP ? "EncSliceLP" : "EncSlice",
              rate_control_names(rc.mode), rate_control_names(rc_modes), packed_header_names(packed));
    return {};
}

void VaapiEncoder::Impl::close_session() noexcept
{
    if (image) {
        vaDestroyImage(dpy(), image->image_id);
        image.reset();
    }
    if (vpp_context != VA_INVALID_ID) {
        vaDestroyContext(dpy(), vpp_context);
        vpp_context = VA_INVALID_ID;
    }
    if (vpp_config != VA_INVALID_ID) {
        vaDestroyConfig(dpy(), vpp_config);
        vpp_config = VA_INVALID_ID;
    }
    if (coded != VA_INVALID_ID) {
        vaDestroyBuffer(dpy(), coded);
        coded = VA_INVALID_ID;
    }
    if (context != VA_INVALID_ID) {
        vaDestroyContext(dpy(), context);
        context = VA_INVALID_ID;
    }
    if (surfaces[0] != VA_INVALID_SURFACE) {
        vaDestroySurfaces(dpy(), surfaces.data(), static_cast<int>(surfaces.size()));
        surfaces.fill(VA_INVALID_SURFACE);
    }
    if (config_id != VA_INVALID_ID) {
        vaDestroyConfig(dpy(), config_id);
        config_id = VA_INVALID_ID;
    }
}

Result<void> VaapiEncoder::Impl::set_rate_control(const RateControl& rate)
{
    FARLAND_TRY(const RcParams next, rc_params(rate, rc_modes));
    if (next == rc) {
        config.rate = rate;
        return {};
    }
    if (next.mode != rc.mode) {
        EncoderConfig reopened = config;
        reopened.rate = rate;
        auto opened = open_session(reopened);
        if (!opened) {
            close_session();
        }
        return opened;
    }
    rc = next;
    config.rate = rate;
    if (rc.mode == VA_RC_CQP) {
        // pic_init_qp is in the PPS, which only an IDR brings along.
        pic_init_qp = rc.qp;
        force_idr = true;
    } else {
        rc_dirty = true;
    }
    return {};
}

Result<void> VaapiEncoder::Impl::ensure_image()
{
    if (image) {
        return {};
    }
    VAImageFormat format{};
    format.fourcc = VA_FOURCC_NV12;
    format.byte_order = VA_LSB_FIRST;
    format.bits_per_pixel = 12;
    VAImage created{};
    if (!succeeded(
            vaCreateImage(dpy(), &format, static_cast<int>(config.width), static_cast<int>(config.height), &created),
            "vaCreateImage")) {
        return fail(Errc::io, "VA-API could not create an NV12 image");
    }
    const std::size_t luma_end =
        std::size_t{created.offsets[0]} + (std::size_t{created.pitches[0]} * (config.height - 1U)) + config.width;
    const std::size_t chroma_end = std::size_t{created.offsets[1]} +
                                   (std::size_t{created.pitches[1]} * ((config.height / 2U) - 1U)) + config.width;
    if (created.num_planes < 2 || created.pitches[0] < config.width || created.pitches[1] < config.width ||
        luma_end > created.data_size || chroma_end > created.data_size) {
        vaDestroyImage(dpy(), created.image_id);
        return fail(Errc::io, "VA-API created an NV12 image of an unexpected layout");
    }
    image = created;
    return {};
}

Result<void> VaapiEncoder::Impl::clear_input()
{
    // Pictures from dmabufs smaller than the coded size leave the rest of the
    // input surface alone; it stays black.
    FARLAND_TRY_VOID(ensure_image());
    {
        const MappedBuffer map(dpy(), image->buf);
        if (map.data() == nullptr) {
            return fail(Errc::io, "VA-API could not map an image");
        }
        const std::span bytes(static_cast<std::byte*>(map.data()), image->data_size);
        const auto black = options.color.full_range ? std::byte{0} : std::byte{16};
        for (std::uint32_t row = 0; row < config.height; ++row) {
            std::ranges::fill(bytes.subspan(image->offsets[0] + (std::size_t{row} * image->pitches[0]), config.width),
                              black);
        }
        for (std::uint32_t row = 0; row < config.height / 2U; ++row) {
            std::ranges::fill(bytes.subspan(image->offsets[1] + (std::size_t{row} * image->pitches[1]), config.width),
                              std::byte{128});
        }
    }
    if (!succeeded(vaPutImage(dpy(), surfaces[input_surface], image->image_id, 0, 0, config.width, config.height, 0, 0,
                              config.width, config.height),
                   "vaPutImage")) {
        return fail(Errc::io, "VA-API could not write the input surface");
    }
    return {};
}

Result<void> VaapiEncoder::Impl::upload(const codec::Yuv420View& picture)
{
    FARLAND_TRY_VOID(ensure_image());
    {
        const MappedBuffer map(dpy(), image->buf);
        if (map.data() == nullptr) {
            return fail(Errc::io, "VA-API could not map an image");
        }
        const std::span bytes(static_cast<std::byte*>(map.data()), image->data_size);
        for (std::uint32_t row = 0; row < config.height; ++row) {
            std::ranges::copy(picture.y.subspan(row * picture.y_stride, config.width),
                              bytes.subspan(image->offsets[0] + (std::size_t{row} * image->pitches[0])).begin());
        }
        const std::size_t chroma_width = config.width / 2U;
        for (std::uint32_t row = 0; row < config.height / 2U; ++row) {
            const auto u = picture.u.subspan(row * picture.uv_stride, chroma_width);
            const auto v = picture.v.subspan(row * picture.uv_stride, chroma_width);
            const auto line = bytes.subspan(image->offsets[1] + (std::size_t{row} * image->pitches[1]), config.width);
            for (std::size_t x = 0; x < chroma_width; ++x) {
                line[2 * x] = u[x];
                line[(2 * x) + 1] = v[x];
            }
        }
    }
    if (!succeeded(vaPutImage(dpy(), surfaces[input_surface], image->image_id, 0, 0, config.width, config.height, 0, 0,
                              config.width, config.height),
                   "vaPutImage")) {
        return fail(Errc::io, "VA-API could not upload the picture");
    }
    return {};
}

Result<codec::Yuv420Frame> VaapiEncoder::Impl::read_input()
{
    FARLAND_TRY_VOID(ensure_image());
    if (!succeeded(vaSyncSurface(dpy(), surfaces[input_surface]), "vaSyncSurface") ||
        !succeeded(vaGetImage(dpy(), surfaces[input_surface], 0, 0, config.width, config.height, image->image_id),
                   "vaGetImage")) {
        return fail(Errc::io, "VA-API could not read the input surface back");
    }
    const MappedBuffer map(dpy(), image->buf);
    if (map.data() == nullptr) {
        return fail(Errc::io, "VA-API could not map an image");
    }
    const std::span bytes(static_cast<const std::byte*>(map.data()), image->data_size);
    codec::Yuv420Frame out(config.width, config.height);
    const auto y = out.y();
    const auto u = out.u();
    const auto v = out.v();
    for (std::uint32_t row = 0; row < config.height; ++row) {
        std::ranges::copy(bytes.subspan(image->offsets[0] + (std::size_t{row} * image->pitches[0]), config.width),
                          y.subspan(std::size_t{row} * out.y_stride()).begin());
    }
    for (std::uint32_t row = 0; row < config.height / 2U; ++row) {
        const auto line = bytes.subspan(image->offsets[1] + (std::size_t{row} * image->pitches[1]), config.width);
        for (std::size_t x = 0; x < out.uv_stride(); ++x) {
            u[(row * out.uv_stride()) + x] = line[2 * x];
            v[(row * out.uv_stride()) + x] = line[(2 * x) + 1];
        }
    }
    return out;
}

Result<void> VaapiEncoder::Impl::ensure_vpp()
{
    if (vpp_context != VA_INVALID_ID) {
        return {};
    }
    if (!device.video_proc) {
        return fail(Errc::unsupported, "the VA-API driver has no video processing for the colour conversion");
    }
    if (vpp_config == VA_INVALID_ID &&
        !succeeded(vaCreateConfig(dpy(), VAProfileNone, VAEntrypointVideoProc, nullptr, 0, &vpp_config),
                   "vaCreateConfig")) {
        vpp_config = VA_INVALID_ID;
        return fail(Errc::unsupported, "the VA-API driver has no video processing for the colour conversion");
    }
    if (!succeeded(vaCreateContext(dpy(), vpp_config, static_cast<int>(config.width), static_cast<int>(config.height),
                                   VA_PROGRESSIVE, &surfaces[input_surface], 1, &vpp_context),
                   "vaCreateContext")) {
        vpp_context = VA_INVALID_ID;
        return fail(Errc::unsupported, "the VA-API driver has no video processing for the colour conversion");
    }
    return {};
}

Result<VASurfaceID> VaapiEncoder::Impl::import(const DmabufFrame& frame)
{
    ImportKey key{.fourcc = frame.fourcc,
                  .modifier = frame.modifier,
                  .width = frame.width,
                  .height = frame.height,
                  .planes = frame.plane_count};
    for (std::uint32_t i = 0; i < frame.plane_count; ++i) {
        const DmabufPlane& plane = frame.planes.at(i);
        struct stat status{};
        if (plane.fd < 0 || ::fstat(plane.fd, &status) != 0) {
            return fail(Errc::invalid_value, "a dmabuf plane has no open descriptor");
        }
        key.devices.at(i) = static_cast<std::uint64_t>(status.st_dev);
        key.inodes.at(i) = static_cast<std::uint64_t>(status.st_ino);
        key.offsets.at(i) = plane.offset;
        key.pitches.at(i) = plane.pitch;
    }
    ++import_clock;
    const auto cached = std::ranges::find(imports, key, &Import::key);
    if (cached != imports.end()) {
        cached->last_used = import_clock;
        return cached->surface;
    }

    FARLAND_TRY(const std::uint32_t fourcc, va_fourcc(frame.fourcc));
    // One object per distinct buffer, one layer with all planes
    // (VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, va_drmcommon.h).
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index): indices are below plane_count <= 4.
    VADRMPRIMESurfaceDescriptor descriptor{};
    descriptor.fourcc = fourcc;
    descriptor.width = frame.width;
    descriptor.height = frame.height;
    descriptor.num_layers = 1;
    descriptor.layers[0].drm_format = frame.fourcc;
    descriptor.layers[0].num_planes = frame.plane_count;
    for (std::uint32_t i = 0; i < frame.plane_count; ++i) {
        std::uint32_t object = 0;
        while (object < descriptor.num_objects &&
               (key.devices.at(object) != key.devices.at(i) || key.inodes.at(object) != key.inodes.at(i))) {
            ++object;
        }
        if (object == descriptor.num_objects) {
            const int fd = frame.planes.at(i).fd;
            const off_t size = ::lseek(fd, 0, SEEK_END);
            if (size <= 0 || size > std::numeric_limits<std::uint32_t>::max()) {
                return fail(Errc::invalid_value, "cannot tell the size of a dmabuf");
            }
            descriptor.objects[object].fd = fd;
            descriptor.objects[object].size = static_cast<std::uint32_t>(size);
            descriptor.objects[object].drm_format_modifier = frame.modifier;
            ++descriptor.num_objects;
            // Later planes compare against this object's buffer.
            key.devices.at(object) = key.devices.at(i);
            key.inodes.at(object) = key.inodes.at(i);
        }
        descriptor.layers[0].object_index[i] = object;
        descriptor.layers[0].offset[i] = frame.planes.at(i).offset;
        descriptor.layers[0].pitch[i] = frame.planes.at(i).pitch;
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

    std::array<VASurfaceAttrib, 2> attributes{};
    attributes[0].type = VASurfaceAttribMemoryType;
    attributes[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attributes[0].value.type = VAGenericValueTypeInteger;
    attributes[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attributes[1].type = VASurfaceAttribExternalBufferDescriptor;
    attributes[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attributes[1].value.type = VAGenericValueTypePointer;
    attributes[1].value.value.p = &descriptor;
    VASurfaceID surface = VA_INVALID_SURFACE;
    const VAStatus status = vaCreateSurfaces(dpy(), VA_RT_FORMAT_RGB32, frame.width, frame.height, &surface, 1,
                                             attributes.data(), static_cast<unsigned>(attributes.size()));
    if (status != VA_STATUS_SUCCESS) {
        const std::pair failed{frame.fourcc, frame.modifier};
        if (std::ranges::find(failed_imports, failed) == failed_imports.end()) {
            failed_imports.push_back(failed);
            log::warn(component, "{} cannot import dmabufs of format {:#010x} with modifier {:#x}: {}",
                      device.render_node, frame.fourcc, frame.modifier, vaErrorStr(status));
        }
        return fail(Errc::unsupported, "the VA-API driver cannot import this dmabuf");
    }

    // The key again with the original per-plane buffers.
    for (std::uint32_t i = 0; i < frame.plane_count; ++i) {
        struct stat plane_status{};
        if (::fstat(frame.planes.at(i).fd, &plane_status) == 0) {
            key.devices.at(i) = static_cast<std::uint64_t>(plane_status.st_dev);
            key.inodes.at(i) = static_cast<std::uint64_t>(plane_status.st_ino);
        }
    }
    if (imports.size() >= max_imports) {
        const auto oldest = std::ranges::min_element(imports, {}, &Import::last_used);
        vaDestroySurfaces(dpy(), &oldest->surface, 1);
        imports.erase(oldest);
    }
    imports.push_back({.key = key, .surface = surface, .last_used = import_clock});
    return surface;
}

void VaapiEncoder::Impl::drop_imports() noexcept
{
    for (Import& entry : imports) {
        vaDestroySurfaces(dpy(), &entry.surface, 1);
    }
    imports.clear();
}

Result<void> VaapiEncoder::Impl::render(VAContextID target_context, VASurfaceID target, Buffers& buffers)
{
    if (!succeeded(vaBeginPicture(dpy(), target_context, target), "vaBeginPicture")) {
        return fail(Errc::io, "VA-API rejected a picture");
    }
    const auto ids = buffers.ids();
    const VAStatus rendered = vaRenderPicture(dpy(), target_context, ids.data(), static_cast<int>(ids.size()));
    const VAStatus ended = vaEndPicture(dpy(), target_context);
    if (!succeeded(rendered, "vaRenderPicture") || !succeeded(ended, "vaEndPicture")) {
        return fail(Errc::io, "VA-API rejected a picture");
    }
    return {};
}

Result<void> VaapiEncoder::Impl::convert(const DmabufFrame& frame)
{
    if (frame.width == 0 || frame.height == 0 || frame.width > config.width || frame.height > config.height) {
        return fail(Errc::invalid_value, "the dmabuf does not fit into the encoder's pictures");
    }
    if (frame.plane_count == 0 || frame.plane_count > frame.planes.size()) {
        return fail(Errc::invalid_value, "a dmabuf has 1 to 4 planes");
    }
    FARLAND_TRY_VOID(ensure_vpp());
    FARLAND_TRY(const VASurfaceID source, import(frame));

    // Input and output in the same standard, so that drivers with colour
    // management (Mesa's VPE) convert only the matrix: RGB in full range to
    // YUV in the configured range, with chroma sited between the luma samples
    // like the 2x2 average of codec::bgrx_to_yuv420.
    const VAProcColorStandardType standard =
        options.color.matrix == ColorSpace::Matrix::bt709 ? VAProcColorStandardBT709 : VAProcColorStandardBT601;
    const VARectangle region{.x = 0,
                             .y = 0,
                             .width = static_cast<std::uint16_t>(frame.width),
                             .height = static_cast<std::uint16_t>(frame.height)};
    VAProcPipelineParameterBuffer params{};
    params.surface = source;
    params.surface_region = &region;
    params.surface_color_standard = standard;
    params.output_region = &region;
    params.output_background_color = 0xFF000000;
    params.output_color_standard = standard;
    params.input_color_properties.color_range = VA_SOURCE_RANGE_FULL;
    params.output_color_properties.color_range =
        options.color.full_range ? VA_SOURCE_RANGE_FULL : VA_SOURCE_RANGE_REDUCED;
    params.output_color_properties.chroma_sample_location =
        VA_CHROMA_SITING_VERTICAL_CENTER | VA_CHROMA_SITING_HORIZONTAL_CENTER;
    Buffers buffers(dpy());
    FARLAND_TRY_VOID(buffers.add(vpp_context, VAProcPipelineParameterBufferType, params));
    return render(vpp_context, surfaces[input_surface], buffers);
}

Result<void> VaapiEncoder::Impl::add_rate_control(Buffers& buffers, bool reset) const
{
    if (rc.mode == VA_RC_CQP) {
        return {};
    }
    VAEncMiscParameterRateControl params{};
    params.bits_per_second = rc.bits_per_second;
    params.target_percentage = rc.target_percentage;
    params.window_size = config.rate.vbv_window_ms;
    params.min_qp = rc.min_qp;
    params.max_qp = 51;
    params.rc_flags.bits.reset = reset;
    // Low latency: never skip a picture, never pad with filler.
    params.rc_flags.bits.disable_frame_skip = 1;
    params.rc_flags.bits.disable_bit_stuffing = 1;
    FARLAND_TRY_VOID(buffers.add_misc(context, VAEncMiscParameterTypeRateControl, params));
    VAEncMiscParameterHRD hrd{};
    hrd.buffer_size = rc.hrd_bits;
    hrd.initial_buffer_fullness = rc.hrd_bits / 4U * 3U;
    return buffers.add_misc(context, VAEncMiscParameterTypeHRD, hrd);
}

VAEncSequenceParameterBufferH264 VaapiEncoder::Impl::sequence_params() const
{
    VAEncSequenceParameterBufferH264 seq{};
    seq.seq_parameter_set_id = 0;
    seq.level_idc = level_idc;
    seq.intra_period = config.keyint != 0 ? config.keyint : idle_intra_period;
    seq.intra_idr_period = seq.intra_period;
    seq.ip_period = 1;
    seq.bits_per_second = rc.mode == VA_RC_CQP ? 0U : rc.bits_per_second;
    seq.max_num_ref_frames = 1;
    seq.picture_width_in_mbs = static_cast<std::uint16_t>(width_mbs());
    seq.picture_height_in_mbs = static_cast<std::uint16_t>(height_mbs());
    seq.seq_fields.bits.chroma_format_idc = 1;
    seq.seq_fields.bits.frame_mbs_only_flag = 1;
    seq.seq_fields.bits.direct_8x8_inference_flag = 1;
    seq.seq_fields.bits.log2_max_frame_num_minus4 = log2_max_frame_num - 4;
    seq.seq_fields.bits.pic_order_cnt_type = 0;
    seq.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = log2_max_poc_lsb - 4;
    seq.vui_parameters_present_flag = 1;
    seq.vui_fields.bits.timing_info_present_flag = 1;
    seq.vui_fields.bits.bitstream_restriction_flag = 1;
    seq.vui_fields.bits.log2_max_mv_length_horizontal = 15;
    seq.vui_fields.bits.log2_max_mv_length_vertical = 15;
    seq.vui_fields.bits.motion_vectors_over_pic_boundaries_flag = 1;
    seq.num_units_in_tick = 1;
    seq.time_scale = 2U * config.fps;
    return seq;
}

Result<void> VaapiEncoder::Impl::read_coded()
{
    const VAStatus synced = vaSyncBuffer(dpy(), coded, VA_TIMEOUT_INFINITE);
    if (synced == VA_STATUS_ERROR_UNIMPLEMENTED) {
        if (!succeeded(vaSyncSurface(dpy(), surfaces[input_surface]), "vaSyncSurface")) {
            return fail(Errc::io, "the VA-API encoder failed");
        }
    } else if (!succeeded(synced, "vaSyncBuffer")) {
        return fail(Errc::io, "the VA-API encoder failed");
    }
    const MappedBuffer map(dpy(), coded);
    if (map.data() == nullptr) {
        return fail(Errc::io, "VA-API could not map the coded buffer");
    }
    coded_bytes.clear();
    bool overflow = false;
    for (auto* segment = static_cast<const VACodedBufferSegment*>(map.data()); segment != nullptr;
         segment = static_cast<const VACodedBufferSegment*>(segment->next)) {
        overflow = overflow || (segment->status & VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK) != 0;
        if (segment->buf != nullptr && segment->size > 0) {
            const std::span bytes(static_cast<const std::byte*>(segment->buf), segment->size);
            coded_bytes.insert(coded_bytes.end(), bytes.begin(), bytes.end());
        }
    }
    if (overflow) {
        return fail(Errc::io, "the H.264 picture did not fit into the coded buffer");
    }
    if (coded_bytes.empty()) {
        return fail(Errc::io, "the VA-API encoder produced no data");
    }
    return {};
}

void VaapiEncoder::Impl::log_coded(std::string_view what) const
{
    // What the driver wrote, for reports about drivers that write unexpected streams.
    std::string types;
    if (const auto units = codec::h264::split_annex_b(coded_bytes)) {
        for (const auto& unit : *units) {
            types += std::format(" {}({})", unit.type, unit.data.size());
        }
    } else {
        types = " (not Annex B)";
    }
    std::string head;
    for (const std::byte b : std::span(coded_bytes).first(std::min<std::size_t>(coded_bytes.size(), 16))) {
        head += std::format(" {:02x}", std::to_integer<unsigned>(b));
    }
    log::warn(component, "{}: {} bytes, NAL units (type(size)):{}; starts with{}", what, coded_bytes.size(), types,
              head);
}

Result<EncodedFrame> VaapiEncoder::Impl::encode_input(const FrameOptions& options_in)
{
    const bool idr = force_idr || options_in.force_idr || frames_since_idr >= max_frames_between_idr ||
                     (config.keyint != 0 && frames_since_idr >= config.keyint);
    const std::uint32_t index = idr ? 0U : frames_since_idr;
    const std::uint32_t frame_num = index % (1U << log2_max_frame_num);
    const auto poc = static_cast<std::int32_t>(2U * index);
    const std::uint16_t pic_id = idr ? static_cast<std::uint16_t>(idr_pic_id + 1U) : idr_pic_id;
    const VASurfaceID reconstructed = surfaces.at(1 + recon);
    const VASurfaceID reference = surfaces.at(2 - recon);

    Buffers buffers(dpy());
    if (idr) {
        FARLAND_TRY_VOID(buffers.add(context, VAEncSequenceParameterBufferType, sequence_params()));
        FARLAND_TRY_VOID(add_rate_control(buffers, false));
        VAEncMiscParameterFrameRate rate{};
        rate.framerate = config.fps;
        FARLAND_TRY_VOID(buffers.add_misc(context, VAEncMiscParameterTypeFrameRate, rate));
        if ((packed & VA_ENC_PACKED_HEADER_SEQUENCE) != 0) {
            const auto sps = bitstream::write_sps({.profile = config.profile,
                                                   .level_idc = level_idc,
                                                   .width_mbs = width_mbs(),
                                                   .height_mbs = height_mbs(),
                                                   .log2_max_frame_num = log2_max_frame_num,
                                                   .log2_max_poc_lsb = log2_max_poc_lsb,
                                                   .max_num_ref_frames = 1,
                                                   .vui = vui()});
            FARLAND_TRY_VOID(buffers.add_packed(context, VAEncPackedHeaderSequence, sps));
        }
        if ((packed & VA_ENC_PACKED_HEADER_PICTURE) != 0) {
            const auto pps = bitstream::write_pps({.profile = config.profile, .pic_init_qp = pic_init_qp});
            FARLAND_TRY_VOID(buffers.add_packed(context, VAEncPackedHeaderPicture, pps));
        }
    } else if (rc_dirty) {
        FARLAND_TRY_VOID(add_rate_control(buffers, true));
    }

    VAPictureH264 reference_picture{};
    reference_picture.picture_id = reference;
    reference_picture.frame_idx = previous_frame_num;
    reference_picture.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    reference_picture.TopFieldOrderCnt = previous_poc;
    reference_picture.BottomFieldOrderCnt = previous_poc;

    VAEncPictureParameterBufferH264 pic{};
    pic.CurrPic.picture_id = reconstructed;
    pic.CurrPic.frame_idx = frame_num;
    pic.CurrPic.TopFieldOrderCnt = poc;
    pic.CurrPic.BottomFieldOrderCnt = poc;
    std::ranges::fill(pic.ReferenceFrames, invalid_picture());
    if (!idr) {
        pic.ReferenceFrames[0] = reference_picture;
    }
    pic.coded_buf = coded;
    pic.frame_num = static_cast<std::uint16_t>(frame_num);
    pic.pic_init_qp = pic_init_qp;
    pic.pic_fields.bits.idr_pic_flag = idr;
    pic.pic_fields.bits.reference_pic_flag = 1;
    pic.pic_fields.bits.entropy_coding_mode_flag = config.profile != Profile::constrained_baseline;
    pic.pic_fields.bits.transform_8x8_mode_flag = config.profile == Profile::high;
    pic.pic_fields.bits.deblocking_filter_control_present_flag = 1;
    FARLAND_TRY_VOID(buffers.add(context, VAEncPictureParameterBufferType, pic));

    // One slice: the GPU does not need slices to go faster.
    VAEncSliceParameterBufferH264 slice{};
    slice.macroblock_address = 0;
    slice.num_macroblocks = width_mbs() * height_mbs();
    slice.macroblock_info = VA_INVALID_ID;
    slice.slice_type = idr ? slice_type_i : slice_type_p;
    slice.idr_pic_id = pic_id;
    slice.pic_order_cnt_lsb = static_cast<std::uint16_t>(static_cast<std::uint32_t>(poc) % (1U << log2_max_poc_lsb));
    std::ranges::fill(slice.RefPicList0, invalid_picture());
    std::ranges::fill(slice.RefPicList1, invalid_picture());
    if (!idr) {
        slice.RefPicList0[0] = reference_picture;
    }
    if ((packed & VA_ENC_PACKED_HEADER_SLICE) != 0) {
        const auto header = bitstream::write_slice_header({.profile = config.profile,
                                                           .idr = idr,
                                                           .frame_num = frame_num,
                                                           .idr_pic_id = pic_id,
                                                           .poc_lsb = slice.pic_order_cnt_lsb,
                                                           .qp_delta = 0,
                                                           .log2_max_frame_num = log2_max_frame_num,
                                                           .log2_max_poc_lsb = log2_max_poc_lsb});
        FARLAND_TRY_VOID(buffers.add_packed(context, VAEncPackedHeaderSlice, header.bytes, header.bits));
    }
    FARLAND_TRY_VOID(buffers.add(context, VAEncSliceParameterBufferType, slice));

    FARLAND_TRY_VOID(render(context, surfaces[input_surface], buffers));
    FARLAND_TRY_VOID(read_coded());
    auto finished = writer.finish(coded_bytes, idr, rc.qp);
    if (!finished) {
        log_coded("unusable access unit");
        return std::unexpected(finished.error());
    }
    EncodedFrame frame = std::move(*finished);

    if (idr) {
        idr_pic_id = pic_id;
    }
    force_idr = false;
    rc_dirty = false;
    previous_frame_num = frame_num;
    previous_poc = poc;
    recon ^= 1U;
    frames_since_idr = index + 1U;
    return frame;
}

VaapiEncoder::VaapiEncoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

VaapiEncoder::~VaapiEncoder() = default;

const EncoderConfig& VaapiEncoder::config() const noexcept
{
    return impl_->config;
}

Result<void> VaapiEncoder::configure(const EncoderConfig& config)
{
    FARLAND_TRY_VOID(validate(config));
    auto opened = impl_->open_session(config);
    if (!opened) {
        impl_->close_session();
    }
    return opened;
}

Result<void> VaapiEncoder::set_rate_control(const RateControl& rate)
{
    FARLAND_TRY_VOID(validate(rate));
    if (!impl_->is_open()) {
        return fail(Errc::io, "VA-API encoder is not configured");
    }
    return impl_->set_rate_control(rate);
}

void VaapiEncoder::request_idr() noexcept
{
    impl_->force_idr = true;
}

Result<EncodedFrame> VaapiEncoder::encode(const codec::Yuv420View& picture, const FrameOptions& options)
{
    if (!impl_->is_open()) {
        return fail(Errc::io, "VA-API encoder is not configured");
    }
    const EncoderConfig& config = impl_->config;
    FARLAND_ASSERT(picture.width == config.width && picture.height == config.height);
    FARLAND_ASSERT(picture.y_stride >= picture.width && picture.uv_stride >= picture.width / 2U);
    FARLAND_ASSERT(picture.y.size() >= picture.y_stride * picture.height);
    FARLAND_ASSERT(picture.u.size() >= picture.uv_stride * (picture.height / 2U));
    FARLAND_ASSERT(picture.v.size() >= picture.uv_stride * (picture.height / 2U));
    auto frame = impl_->upload(picture).and_then([&] { return impl_->encode_input(options); });
    if (!frame) {
        impl_->force_idr = true;
    }
    return frame;
}

bool VaapiEncoder::accepts_dmabuf() const noexcept
{
    return impl_->device.video_proc;
}

Result<EncodedFrame> VaapiEncoder::encode_dmabuf(const DmabufFrame& frame, const FrameOptions& options)
{
    if (!impl_->is_open()) {
        return fail(Errc::io, "VA-API encoder is not configured");
    }
    FARLAND_TRY_VOID(impl_->convert(frame));
    auto encoded = impl_->encode_input(options);
    if (!encoded) {
        impl_->force_idr = true;
    }
    return encoded;
}

Result<codec::Yuv420Frame> VaapiEncoder::convert(const DmabufFrame& frame)
{
    if (!impl_->is_open()) {
        return fail(Errc::io, "VA-API encoder is not configured");
    }
    FARLAND_TRY_VOID(impl_->convert(frame));
    return impl_->read_input();
}

const DeviceInfo& VaapiEncoder::device() const noexcept
{
    return impl_->device;
}

void VaapiEncoder::forget_dmabufs() noexcept
{
    impl_->drop_imports();
}

Result<DeviceInfo> probe(const std::string& render_node)
{
    FARLAND_TRY(auto device, open_device(render_node));
    return std::move(device.second);
}

std::string describe(const DeviceInfo& info)
{
    std::string profiles;
    for (const Profile profile : info.profiles) {
        profiles += profiles.empty() ? "" : ", ";
        profiles += profile_name(profile);
    }
    std::string entrypoints;
    if (info.enc_slice) {
        entrypoints = "EncSlice";
    }
    if (info.enc_slice_lp) {
        entrypoints += entrypoints.empty() ? "EncSliceLP" : " and EncSliceLP";
    }
    return std::format("{} ({}): H.264 {} through {}; rate control {}; packed headers {}; video processing {}",
                       info.render_node, info.vendor, profiles, entrypoints,
                       rate_control_names(info.rate_control_modes), packed_header_names(info.packed_headers),
                       info.video_proc ? "yes" : "no");
}

Result<std::unique_ptr<H264Encoder>> create(const EncoderConfig& config, const BackendOptions& options)
{
    FARLAND_TRY(auto device, open_device(options.render_node));
    auto encoder = std::make_unique<VaapiEncoder>(
        std::make_unique<VaapiEncoder::Impl>(std::move(device.first), std::move(device.second), options));
    FARLAND_TRY_VOID(encoder->configure(config));
    return encoder;
}

}  // namespace farland::video::vaapi
