// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/// Audio sample formats shared by the channels, the encoders and the
/// platform capture (docs/PLAN.md §3.1).
namespace farland::audio {

/// Signed 16-bit little-endian linear PCM, interleaved: the one format audio
/// moves in between capture, encoders and the network.
struct PcmFormat {
    std::uint32_t rate = 48000;
    std::uint16_t channels = 2;

    [[nodiscard]] constexpr std::size_t frames(std::chrono::milliseconds duration) const noexcept
    {
        return static_cast<std::size_t>(rate) * static_cast<std::size_t>(duration.count()) / 1000U;
    }
    /// Samples (not frames) in `duration`.
    [[nodiscard]] constexpr std::size_t samples(std::chrono::milliseconds duration) const noexcept
    {
        return frames(duration) * channels;
    }
    [[nodiscard]] constexpr std::size_t bytes_per_second() const noexcept
    {
        return static_cast<std::size_t>(rate) * channels * 2U;
    }

    friend constexpr bool operator==(const PcmFormat&, const PcmFormat&) = default;
};

/// Turns one packet of PCM into one packet of a compressed format.
class Encoder {
public:
    Encoder() = default;
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    Encoder(Encoder&&) = delete;
    Encoder& operator=(Encoder&&) = delete;
    virtual ~Encoder() = default;

    /// Samples (interleaved, all channels) that one encode() call takes.
    [[nodiscard]] virtual std::size_t packet_samples() const noexcept = 0;
    /// Encodes exactly packet_samples() samples.
    [[nodiscard]] virtual std::vector<std::byte> encode(std::span<const std::int16_t> samples) = 0;
    virtual void set_bitrate(std::uint32_t bits_per_second) = 0;
};

}  // namespace farland::audio
