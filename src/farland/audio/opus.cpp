// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The declarations below follow libopus's public header opus.h and
// opus_defines.h (Xiph.Org Foundation and contributors, BSD-3-Clause). The
// API has been stable since libopus 1.0 (soname 0).

#include <farland/audio/opus.hpp>
#include <farland/base/assert.hpp>
#include <farland/base/log.hpp>

#include <array>
#include <dlfcn.h>
#include <optional>
#include <string_view>

namespace farland::audio::opus {

namespace {

constexpr std::string_view component = "audio.opus";

// opus_defines.h
constexpr int opus_ok = 0;
constexpr int application_audio = 2049;
constexpr int application_restricted_lowdelay = 2051;
constexpr int set_bitrate_request = 4002;
constexpr int set_complexity_request = 4010;
constexpr int set_signal_request = 4024;
constexpr int signal_music = 3002;
/// The largest packet opus_encode may produce for one frame (RFC 6716 3.4:
/// 1275 bytes per frame; 4000 as libopus recommends for the buffer).
constexpr std::size_t max_packet = 4000;
/// 120 ms at 48 kHz, the longest packet a decoder must accept.
constexpr std::size_t max_frames_per_packet = 5760;

#ifdef __APPLE__
constexpr std::array<const char*, 4> library_names{
    "libopus.0.dylib",
    "/opt/homebrew/lib/libopus.0.dylib",
    "/usr/local/lib/libopus.0.dylib",
    "libopus.dylib",
};
#else
constexpr std::array<const char*, 2> library_names{"libopus.so.0", "libopus.so"};
#endif

using EncoderCreateFn = void* (*)(std::int32_t fs, int channels, int application, int* error);
using EncodeFn = std::int32_t (*)(void* st, const std::int16_t* pcm, int frame_size, unsigned char* data,
                                  std::int32_t max_data_bytes);
using EncoderCtlFn = int (*)(void* st, int request, ...);
using EncoderDestroyFn = void (*)(void* st);
using DecoderCreateFn = void* (*)(std::int32_t fs, int channels, int* error);
using DecodeFn = int (*)(void* st, const unsigned char* data, std::int32_t len, std::int16_t* pcm, int frame_size,
                         int decode_fec);
using DecoderDestroyFn = void (*)(void* st);
using VersionFn = const char* (*)();

template <class Fn>
[[nodiscard]] Fn symbol(void* handle, const char* name)
{
    // POSIX requires that the object pointer dlsym returns converts to a
    // function pointer; C++ makes that conversion conditionally-supported.
    return reinterpret_cast<Fn>(dlsym(handle, name));  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

/// libopus, loaded once per process and never unloaded.
struct Library {
    EncoderCreateFn encoder_create = nullptr;
    EncodeFn encode = nullptr;
    EncoderCtlFn encoder_ctl = nullptr;
    EncoderDestroyFn encoder_destroy = nullptr;
    DecoderCreateFn decoder_create = nullptr;
    DecodeFn decode = nullptr;
    DecoderDestroyFn decoder_destroy = nullptr;
    VersionFn version = nullptr;
};

Result<const Library*> load()
{
    static const std::optional<Library> library = []() -> std::optional<Library> {
        for (const char* name : library_names) {
            void* handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (handle == nullptr) {
                continue;
            }
            Library lib{
                .encoder_create = symbol<EncoderCreateFn>(handle, "opus_encoder_create"),
                .encode = symbol<EncodeFn>(handle, "opus_encode"),
                .encoder_ctl = symbol<EncoderCtlFn>(handle, "opus_encoder_ctl"),
                .encoder_destroy = symbol<EncoderDestroyFn>(handle, "opus_encoder_destroy"),
                .decoder_create = symbol<DecoderCreateFn>(handle, "opus_decoder_create"),
                .decode = symbol<DecodeFn>(handle, "opus_decode"),
                .decoder_destroy = symbol<DecoderDestroyFn>(handle, "opus_decoder_destroy"),
                .version = symbol<VersionFn>(handle, "opus_get_version_string"),
            };
            if (lib.encoder_create == nullptr || lib.encode == nullptr || lib.encoder_ctl == nullptr ||
                lib.encoder_destroy == nullptr || lib.decoder_create == nullptr || lib.decode == nullptr ||
                lib.decoder_destroy == nullptr || lib.version == nullptr) {
                log::warn(component, "{} lacks the libopus entry points", name);
                dlclose(handle);
                continue;
            }
            log::debug(component, "loaded {}: {}", name, lib.version());
            return lib;
        }
        return std::nullopt;
    }();
    if (!library) {
        return fail(Errc::unsupported, "libopus not found");
    }
    return &*library;
}

class OpusEncoder final : public Encoder {
public:
    OpusEncoder(const Library& lib, void* state, std::size_t frames, std::uint16_t channels)
        : lib_(lib), state_(state), frames_(frames), channels_(channels)
    {
    }
    OpusEncoder(const OpusEncoder&) = delete;
    OpusEncoder& operator=(const OpusEncoder&) = delete;
    OpusEncoder(OpusEncoder&&) = delete;
    OpusEncoder& operator=(OpusEncoder&&) = delete;
    ~OpusEncoder() override { lib_.encoder_destroy(state_); }

    [[nodiscard]] std::size_t packet_samples() const noexcept override { return frames_ * channels_; }

    [[nodiscard]] std::vector<std::byte> encode(std::span<const std::int16_t> samples) override
    {
        FARLAND_ASSERT(samples.size() == packet_samples());
        std::vector<std::byte> packet(max_packet);
        const std::int32_t size = lib_.encode(
            state_, samples.data(), static_cast<int>(frames_),
            reinterpret_cast<unsigned char*>(packet.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            static_cast<std::int32_t>(packet.size()));
        if (size <= 0) {
            // Only bad arguments or a broken state make libopus fail here.
            log::warn(component, "opus_encode failed ({})", size);
            return {};
        }
        packet.resize(static_cast<std::size_t>(size));
        return packet;
    }

    void set_bitrate(std::uint32_t bits_per_second) override
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): libopus's control interface is variadic
        static_cast<void>(lib_.encoder_ctl(state_, set_bitrate_request, static_cast<std::int32_t>(bits_per_second)));
    }

private:
    const Library& lib_;
    void* state_;
    std::size_t frames_;
    std::uint16_t channels_;
};

}  // namespace

Result<std::string> version()
{
    FARLAND_TRY(const Library* lib, load());
    return std::string(lib->version());
}

Result<std::unique_ptr<Encoder>> create_encoder(const EncoderConfig& config)
{
    if (!valid_rate(config.format.rate) || config.format.channels < 1 || config.format.channels > 2 ||
        !valid_duration(config.packet_duration)) {
        return fail(Errc::unsupported, "Opus does not take this format");
    }
    FARLAND_TRY(const Library* lib, load());
    int error = 0;
    void* state = lib->encoder_create(static_cast<std::int32_t>(config.format.rate), config.format.channels,
                                      config.low_delay ? application_restricted_lowdelay : application_audio, &error);
    if (state == nullptr || error != opus_ok) {
        return fail(Errc::unsupported, "opus_encoder_create failed");
    }
    // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg): libopus's control interface is variadic
    static_cast<void>(lib->encoder_ctl(state, set_complexity_request, 8));
    static_cast<void>(lib->encoder_ctl(state, set_signal_request, signal_music));
    // NOLINTEND(cppcoreguidelines-pro-type-vararg)
    auto encoder = std::make_unique<OpusEncoder>(*lib, state, config.format.frames(config.packet_duration),
                                                 config.format.channels);
    encoder->set_bitrate(config.bitrate);
    return encoder;
}

Result<std::unique_ptr<Decoder>> Decoder::create(PcmFormat format)
{
    if (!valid_rate(format.rate) || format.channels < 1 || format.channels > 2) {
        return fail(Errc::unsupported, "Opus does not take this format");
    }
    FARLAND_TRY(const Library* lib, load());
    int error = 0;
    // NOLINTNEXTLINE(misc-const-correctness): libopus's decoder state, which decoding changes
    void* state = lib->decoder_create(static_cast<std::int32_t>(format.rate), format.channels, &error);
    if (state == nullptr || error != opus_ok) {
        return fail(Errc::unsupported, "opus_decoder_create failed");
    }
    return std::make_unique<Decoder>(Key{}, state, format);
}

Decoder::~Decoder()
{
    if (auto lib = load()) {
        (*lib)->decoder_destroy(state_);
    }
}

Result<std::vector<std::int16_t>> Decoder::decode(std::span<const std::byte> packet)
{
    FARLAND_TRY(const Library* lib, load());
    std::vector<std::int16_t> pcm(max_frames_per_packet * format_.channels);
    const int frames = lib->decode(
        state_,
        reinterpret_cast<const unsigned char*>(packet.data()),  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
        static_cast<std::int32_t>(packet.size()), pcm.data(), static_cast<int>(max_frames_per_packet), 0);
    if (frames < 0) {
        return fail(Errc::invalid_value, "opus_decode rejected the packet");
    }
    pcm.resize(static_cast<std::size_t>(frames) * format_.channels);
    return pcm;
}

}  // namespace farland::audio::opus
