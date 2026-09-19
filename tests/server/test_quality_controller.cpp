// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The quality ladder over simulated link traces: bandwidth drops, RTT
// spikes, a slow client and recovery, sampled every 500 ms.

#include <farland/codec/progressive.hpp>
#include <farland/server/quality_controller.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

using farland::server::NetworkEstimate;
using farland::server::QualityController;
using Clock = QualityController::Clock;
using namespace std::chrono_literals;

namespace {

const Clock::time_point t0 = Clock::time_point{} + 1h;

/// A link as the session would observe it, plus a clock that advances by
/// one sample interval per step.
struct Trace {
    QualityController controller{QualityController::Config{}};
    Clock::time_point now = t0;
    QualityController::Observation o;
    std::vector<unsigned> levels;  ///< the tier after each step
    std::vector<std::string> reasons;

    Trace()
    {
        o.network.rtt = 20ms;
        o.network.base_rtt = 20ms;
        o.network.last_rtt = 20ms;
        o.ack_round_trip = 35ms;
        o.max_frames_in_flight = 2;
    }

    void rtt(Clock::duration value) { o.network.rtt = o.network.last_rtt = value; }

    /// `steps` samples of the current observation; `kbps` of output meanwhile.
    void run(int steps, std::uint32_t kbps = 1000)
    {
        for (int i = 0; i < steps; ++i) {
            now += 500ms;
            o.bytes_sent += std::uint64_t{kbps} * 1000 / 8 / 2;
            if (auto change = controller.update(o, now)) {
                reasons.push_back(change->reason);
            }
            levels.push_back(controller.tier().level);
        }
    }

    [[nodiscard]] unsigned level() const { return controller.tier().level; }
    /// Samples since the tier last changed.
    [[nodiscard]] std::size_t steps_at_current_level() const
    {
        std::size_t n = 0;
        for (auto it = levels.rbegin(); it != levels.rend() && *it == level(); ++it) {
            ++n;
        }
        return n;
    }
};

}  // namespace

namespace {

QualityController::Config config_default()
{
    QualityController::Config config;
    config.width = 1920;
    config.height = 1080;
    config.fps = 30;
    return config;
}

}  // namespace

TEST_CASE("Tiers honour a configured bitrate floor, ceiling and target", "[server][quality]")
{
    QualityController::Config config;
    config.width = 1920;
    config.height = 1080;
    config.fps = 30;

    const auto plain = QualityController::make_tier(0, config);
    CHECK(plain.h264.mode == farland::video::RateControl::Mode::constant_quality);

    SECTION("a ceiling holds every tier down")
    {
        config.max_bitrate_kbps = 1500;
        for (unsigned level = 0; level < QualityController::tier_count; ++level) {
            const auto tier = QualityController::make_tier(level, config);
            INFO("tier " << level);
            CHECK(tier.h264.max_bitrate_kbps <= 1500);
        }
        // Without it the top tier wants far more than that, so the ceiling
        // is doing something.
        CHECK(plain.h264.max_bitrate_kbps > 1500);
    }

    SECTION("a floor holds the lowest tier up")
    {
        config.min_bitrate_kbps = 4000;
        for (unsigned level = 0; level < QualityController::tier_count; ++level) {
            const auto tier = QualityController::make_tier(level, config);
            INFO("tier " << level);
            CHECK(tier.h264.max_bitrate_kbps >= 4000);
        }
        // The lowest tier is below that on its own.
        CHECK(QualityController::make_tier(QualityController::tier_count - 1, config_default()).h264.max_bitrate_kbps <
              4000);
    }

    SECTION("a target switches to average bitrate, and the ladder still steps")
    {
        config.target_bitrate_kbps = 4000;
        config.min_bitrate_kbps = 0;
        std::uint32_t previous = 0;
        for (unsigned level = 0; level < QualityController::tier_count; ++level) {
            const auto tier = QualityController::make_tier(level, config);
            INFO("tier " << level);
            CHECK(tier.h264.mode == farland::video::RateControl::Mode::bitrate);
            CHECK(tier.h264.bitrate_kbps == tier.h264.max_bitrate_kbps);
            if (level == 0) {
                CHECK(tier.h264.bitrate_kbps == 4000);
            } else {
                CHECK(tier.h264.bitrate_kbps < previous);
            }
            previous = tier.h264.bitrate_kbps;
        }
    }

    SECTION("AVC444's two pictures share the ceiling")
    {
        config.max_bitrate_kbps = 2000;
        config.pictures_per_frame = 2;
        // The cap is per picture, so two views stay inside the ceiling
        // between them rather than each taking all of it.
        CHECK(QualityController::make_tier(0, config).h264.max_bitrate_kbps <= 2000);
    }
}

TEST_CASE("Quality tiers: frame rate, quantization and an H.264 budget that scales with the desktop")
{
    const QualityController::Config config;  // 1920x1080 at 30 fps
    const auto high = QualityController::make_tier(0, config);
    CHECK(high.name == "high");
    CHECK(high.fps == 30);
    CHECK(high.progressive_quant == farland::codec::progressive::quant_default);
    CHECK(high.h264.mode == farland::video::RateControl::Mode::constant_quality);
    CHECK(high.h264.quality == 23);
    CHECK(high.h264.max_bitrate_kbps == 6220);  // 0.10 bit per pixel per frame, ZeroVDI's tier 0
    CHECK(high.budget_kbps == 6220);

    const auto low = QualityController::make_tier(2, config);
    CHECK(low.fps == 20);
    CHECK(low.h264.quality == 31);
    // The encoder budgets per nominal (30 fps) second; at 20 fps the stream stays within budget.
    CHECK(low.budget_kbps == 1451);
    CHECK(low.h264.max_bitrate_kbps == 2177);

    const auto minimal = QualityController::make_tier(3, config);
    // AVC444 holds chroma back from the low tiers on.
    CHECK_FALSE(high.defer_chroma);
    CHECK_FALSE(QualityController::make_tier(1, config).defer_chroma);
    CHECK(low.defer_chroma);
    CHECK(minimal.defer_chroma);
    CHECK(minimal.fps == 10);
    CHECK(minimal.h264.quality == 35);
    for (unsigned level = 1; level < QualityController::tier_count; ++level) {
        const auto finer = QualityController::make_tier(level - 1, config).progressive_quant;
        const auto coarser = QualityController::make_tier(level, config).progressive_quant;
        for (std::size_t band = 0; band < farland::codec::rfx::band_count; ++band) {
            CHECK(coarser.bands.at(band) >= finer.bands.at(band));
            CHECK(coarser.bands.at(band) <= 15);
        }
    }

    // With a measured bandwidth the cap stays within 80 % of it.
    const auto capped = QualityController::make_tier(0, config, 2000);
    CHECK(capped.h264.max_bitrate_kbps == 1600);
    CHECK(QualityController::make_tier(0, config, 10).h264.max_bitrate_kbps == 300);  // floor

    QualityController::Config slow;
    slow.fps = 6;
    CHECK(QualityController::make_tier(3, slow).fps == 5);
    CHECK(QualityController::make_tier(0, slow).fps == 6);
}

TEST_CASE("A healthy link stays at the top tier")
{
    Trace t;
    t.o.network.bandwidth_kbps = 100'000;
    t.run(60, 5000);
    CHECK(t.level() == 0);
    CHECK(t.reasons.empty());
}

TEST_CASE("An RTT spike steps down fast; recovery steps up slowly, one tier at a time")
{
    Trace t;
    t.run(4);
    t.rtt(160ms);  // 140 ms of queueing
    t.run(1);
    CHECK(t.level() == 0);  // one congested sample is not enough
    t.run(1);
    CHECK(t.level() == 1);
    REQUIRE(t.reasons.size() == 1);
    CHECK(t.reasons[0].find("above the base RTT") != std::string::npos);

    // Still congested: the next step waits for the settle time.
    t.run(3);
    CHECK(t.level() == 1);
    t.run(1);
    CHECK(t.level() == 2);

    // Recovery: five clear seconds per tier.
    t.rtt(22ms);
    t.run(9);
    CHECK(t.level() == 2);
    t.run(1);
    CHECK(t.level() == 1);
    t.run(10);
    CHECK(t.level() == 0);
    CHECK(t.reasons.back().find("clear") != std::string::npos);
}

TEST_CASE("Severe congestion steps down at the first sample")
{
    Trace t;
    t.run(2);
    t.rtt(500ms);
    t.run(1);
    CHECK(t.level() == 1);
}

TEST_CASE("An unanswered RTT probe counts as a late answer")
{
    Trace t;
    t.o.network.unanswered = 900ms;  // the link stalled
    t.run(1);
    CHECK(t.level() == 1);
}

TEST_CASE("A bandwidth drop jumps straight to the tier that fits")
{
    Trace t;
    t.o.network.bandwidth_kbps = 50'000;
    t.run(10, 4000);
    CHECK(t.level() == 0);

    // 1.5 Mbit/s: 80 % of it fits only the minimal tier's 414 kbit/s at 1080p10.
    t.o.network.bandwidth_kbps = 1500;
    t.run(2, 1000);
    CHECK(t.level() == 3);
    CHECK(t.reasons.back().find("bandwidth 1500") != std::string::npos);

    // Clear again, but the bandwidth still has no room for the next tier up.
    t.run(40, 300);
    CHECK(t.level() == 3);

    // More bandwidth: back up, tier by tier.
    t.o.network.bandwidth_kbps = 50'000;
    t.run(10, 300);
    CHECK(t.level() == 2);
    t.run(20, 300);
    CHECK(t.level() == 0);
}

TEST_CASE("Sending close to the measured bandwidth counts as congestion")
{
    Trace t;
    t.o.network.bandwidth_kbps = 10'000;  // tier 0 fits: 6220 <= 8000
    t.run(10, 5000);
    CHECK(t.level() == 0);
    t.run(2, 9000);
    CHECK(t.level() == 1);
    CHECK(t.reasons.back().find("sending") != std::string::npos);
}

TEST_CASE("Slow acknowledgements and a deep client queue step down without auto-detect")
{
    Trace acks;
    acks.o.network = NetworkEstimate{};  // no auto-detect
    acks.o.ack_round_trip = 400ms;
    acks.run(2);
    CHECK(acks.level() == 1);
    CHECK(acks.reasons.back().find("acknowledged") != std::string::npos);

    Trace queue;
    queue.o.queue_depth = 4;
    queue.run(2);
    CHECK(queue.level() == 1);
    CHECK(queue.reasons.back().find("queued") != std::string::npos);

    // A full frame window alone is not congestion, but it keeps the tier from rising.
    Trace window;
    window.rtt(200ms);
    window.run(2);
    REQUIRE(window.level() == 1);
    window.rtt(20ms);
    window.o.frames_in_flight = 2;
    window.run(30);
    CHECK(window.level() == 1);
    window.o.frames_in_flight = 1;
    window.run(10);
    CHECK(window.level() == 0);
}

TEST_CASE("Alternating samples do not flap the tier")
{
    Trace t;
    for (int i = 0; i < 40; ++i) {
        t.rtt(i % 2 == 0 ? 160ms : 20ms);
        t.run(1);
    }
    CHECK(t.level() == 0);
    CHECK(t.reasons.empty());
}

TEST_CASE("An upshift that fails at once doubles the wait before the next one")
{
    Trace t;
    t.rtt(160ms);
    t.run(2);
    REQUIRE(t.level() == 1);

    // Clear: back up after 5 s.
    t.rtt(20ms);
    t.run(10);
    REQUIRE(t.level() == 0);
    // Congested again at once: the upshift failed.
    t.rtt(160ms);
    t.run(2);
    REQUIRE(t.level() == 1);

    // The next upshift waits 10 s.
    t.rtt(20ms);
    t.run(19);
    CHECK(t.level() == 1);
    t.run(1);
    CHECK(t.level() == 0);

    // And after another failure 20 s.
    t.rtt(160ms);
    t.run(2);
    REQUIRE(t.level() == 1);
    t.rtt(20ms);
    t.run(39);
    CHECK(t.level() == 1);
    t.run(1);
    CHECK(t.level() == 0);

    // A minute without congestion earns the short wait back.
    t.run(120);
    t.rtt(160ms);
    t.run(2);
    REQUIRE(t.level() == 1);
    t.rtt(20ms);
    t.run(40);
    CHECK(t.level() == 0);
    CHECK(t.steps_at_current_level() == 31);  // back up at the tenth clear sample: the short 5 s wait
}

TEST_CASE("Samples closer together than the interval are ignored")
{
    QualityController controller{QualityController::Config{}};
    QualityController::Observation o;
    o.network.rtt = 900ms;
    o.network.base_rtt = 20ms;
    CHECK(controller.update(o, t0).has_value());  // severe: at once
    CHECK_FALSE(controller.update(o, t0 + 100ms).has_value());
    CHECK(controller.tier().level == 1);
}
