// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/audio/pcm.hpp>
#include <farland/base/error.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

/// Opus (RFC 6716) through libopus, loaded at runtime like OpenH264: the
/// build needs no Opus headers, and servers without libopus fall back to PCM.
/// libopus is BSD-3-Clause.
namespace farland::audio::opus {

/// The libopus version string, or an error when no libopus.so.0 loads.
[[nodiscard]] Result<std::string> version();

/// A packet duration Opus supports (2.5 ms steps are not offered).
[[nodiscard]] constexpr bool valid_duration(std::chrono::milliseconds d) noexcept
{
    return d.count() == 10 || d.count() == 20 || d.count() == 40 || d.count() == 60;
}
/// A sample rate Opus supports.
[[nodiscard]] constexpr bool valid_rate(std::uint32_t rate) noexcept
{
    return rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000;
}

struct EncoderConfig {
    PcmFormat format;
    std::chrono::milliseconds packet_duration{20};
    std::uint32_t bitrate = 96000;
    /// OPUS_APPLICATION_RESTRICTED_LOWDELAY: 5 ms of algorithmic delay
    /// instead of 26.5 ms, for music and speech alike.
    bool low_delay = true;
};

/// One Opus packet per encode() call.
[[nodiscard]] Result<std::unique_ptr<Encoder>> create_encoder(const EncoderConfig& config);

/// Decodes Opus packets, for tests and a future client.
class Decoder {
public:
    [[nodiscard]] static Result<std::unique_ptr<Decoder>> create(PcmFormat format);

    /// Only create() can name the key.
    class Key {
        friend class Decoder;
        Key() = default;
    };
    Decoder(Key /*key*/, void* state, PcmFormat format) : state_(state), format_(format) {}
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) = delete;
    Decoder& operator=(Decoder&&) = delete;
    ~Decoder();

    /// Interleaved samples of one packet.
    [[nodiscard]] Result<std::vector<std::int16_t>> decode(std::span<const std::byte> packet);

private:
    void* state_;
    PcmFormat format_;
};

}  // namespace farland::audio::opus
