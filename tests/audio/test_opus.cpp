// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Needs libopus at runtime; skipped where it does not load.

#include <farland/audio/opus.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <vector>

namespace opus = farland::audio::opus;
using farland::audio::PcmFormat;
using std::chrono::milliseconds;

TEST_CASE("Opus encodes a tone that decodes to the same tone")
{
    if (const auto version = opus::version(); !version) {
        SKIP("libopus not found");
    }
    const PcmFormat format{48000, 2};
    auto encoder = opus::create_encoder(opus::EncoderConfig{.format = format, .packet_duration = milliseconds(20)});
    REQUIRE(encoder);
    CHECK((*encoder)->packet_samples() == 1920);
    auto decoder = opus::Decoder::create(format);
    REQUIRE(decoder);

    // One second of 1 kHz; compare the energy after the codec's delay.
    std::vector<std::int16_t> input(format.samples(milliseconds(1000)));
    for (std::size_t i = 0; i < input.size(); ++i) {
        const double t = static_cast<double>(i / 2) / format.rate;
        input[i] = static_cast<std::int16_t>(10000 * std::sin(2 * std::numbers::pi * 1000 * t));
    }
    std::vector<std::int16_t> output;
    for (std::size_t offset = 0; offset < input.size(); offset += 1920) {
        const auto packet = (*encoder)->encode(std::span(input).subspan(offset, 1920));
        REQUIRE(!packet.empty());
        CHECK(packet.size() < 1920 * 2 / 4);
        const auto pcm = (*decoder)->decode(packet);
        REQUIRE(pcm);
        CHECK(pcm->size() == 1920);
        output.insert(output.end(), pcm->begin(), pcm->end());
    }
    const auto energy = [](std::span<const std::int16_t> s) {
        double sum = 0;
        for (const auto v : s) {
            sum += static_cast<double>(v) * v;
        }
        return sum / static_cast<double>(s.size());
    };
    const auto tail = std::span(output).subspan(output.size() / 2);
    const double ratio = energy(tail) / energy(std::span(input).subspan(input.size() / 2));
    CHECK(ratio > 0.8);
    CHECK(ratio < 1.25);
}

TEST_CASE("Opus refuses formats it does not have")
{
    CHECK_FALSE(opus::create_encoder(opus::EncoderConfig{.format = PcmFormat{44100, 2}}));
    CHECK_FALSE(
        opus::create_encoder(opus::EncoderConfig{.format = PcmFormat{48000, 2}, .packet_duration = milliseconds(15)}));
}
