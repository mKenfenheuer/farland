// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/server/quality_controller.hpp>

#include <algorithm>
#include <format>

namespace farland::server {

namespace {

using Clock = QualityController::Clock;

struct Rung {
    std::string_view name;
    /// Frame rate as a share of the session's, in thirds.
    unsigned fps_thirds;
    /// TS_RFX_CODEC_QUANT order: LL3, LH3, HL3, HH3, LH2, HL2, HH2, LH1, HL1, HH1.
    /// Tier 0 is FreeRDP's default table; lower tiers coarsen the fine bands
    /// first, which costs detail before it costs colour.
    std::array<std::uint8_t, codec::rfx::band_count> quant;
    /// H.264 CRF, and the VBV cap as bits per pixel per frame (ZeroVDI's
    /// TierCrf and TierBpp: about 6.2 Mbit/s at 1080p30 in tier 0).
    std::uint8_t crf;
    double bits_per_pixel;
};

constexpr std::array<Rung, QualityController::tier_count> rungs{{
    {"high", 3, {6, 6, 6, 6, 7, 7, 8, 8, 8, 9}, 23, 0.10},
    {"medium", 3, {6, 6, 6, 6, 7, 7, 9, 9, 9, 10}, 27, 0.06},
    {"low", 2, {6, 7, 7, 7, 8, 8, 10, 10, 10, 11}, 31, 0.035},
    {"minimal", 1, {7, 8, 8, 8, 9, 9, 11, 11, 11, 12}, 35, 0.02},
}};

constexpr unsigned min_fps = 5;
constexpr std::uint32_t min_cap_kbps = 300;

double ms(Clock::duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

}  // namespace

QualityTier QualityController::make_tier(unsigned level, const Config& config,
                                         std::optional<std::uint32_t> bandwidth_kbps)
{
    level = std::min(level, tier_count - 1);
    const Rung& rung = rungs.at(level);
    QualityTier tier;
    tier.level = level;
    tier.name = rung.name;
    const unsigned fps = std::max(config.fps, 1U);
    tier.fps = std::min(fps, std::max(min_fps, (fps * rung.fps_thirds) / 3));
    tier.progressive_quant = codec::rfx::quant_from_rfx_order(rung.quant);
    tier.defer_chroma = level >= 2;

    const double pixels = static_cast<double>(config.width) * config.height;
    tier.budget_kbps = static_cast<std::uint32_t>(rung.bits_per_pixel * pixels * tier.fps / 1000.0);
    // The encoders budget each picture as cap / nominal fps, so the cap is
    // per nominal second: at a reduced frame rate the stream stays at
    // `budget_kbps`.
    double cap = rung.bits_per_pixel * pixels * fps / 1000.0;
    if (bandwidth_kbps) {
        cap = std::min(cap, config.bandwidth_share * *bandwidth_kbps * fps / tier.fps);
    }
    tier.h264.mode = video::RateControl::Mode::constant_quality;
    tier.h264.quality = rung.crf;
    tier.h264.max_bitrate_kbps = std::clamp(static_cast<std::uint32_t>(cap), min_cap_kbps, video::max_bitrate_kbps);
    return tier;
}

QualityController::QualityController(Config config)
    : config_(config), tier_(make_tier(0, config_)), upshift_wait_(config_.upshift_wait)
{
    config_.downshift_samples = std::max(config_.downshift_samples, 1U);
    for (unsigned level = 0; level < tier_count; ++level) {
        budgets_.at(level) = make_tier(level, config_).budget_kbps;
    }
}

unsigned QualityController::fitting_level(std::uint32_t bandwidth_kbps) const noexcept
{
    const double usable = config_.bandwidth_share * bandwidth_kbps;
    for (unsigned level = 0; level < tier_count; ++level) {
        if (budgets_.at(level) <= usable) {
            return level;
        }
    }
    return tier_count - 1;
}

QualityController::Verdict QualityController::classify(const Observation& o, std::string& reason,
                                                       unsigned& fit_level) const
{
    const auto& net = o.network;
    const Clock::duration base = net.base_rtt.value_or(Clock::duration::zero());
    fit_level = 0;

    // Queueing delay: the RTT now (an unanswered probe is at least that late)
    // above the lowest RTT seen.
    std::optional<Clock::duration> queue_delay;
    if (net.rtt && net.base_rtt) {
        const auto now_rtt = std::max(*net.rtt, net.unanswered);
        queue_delay = now_rtt - base;
    }
    std::optional<Clock::duration> ack_delay;
    if (o.ack_round_trip) {
        ack_delay = *o.ack_round_trip > base ? *o.ack_round_trip - base : Clock::duration::zero();
    }

    Verdict verdict = Verdict::hold;
    const auto worse = [&](Verdict v, std::string why) {
        if (v > verdict || (v == verdict && reason.empty())) {
            verdict = v;
            reason = std::move(why);
        }
    };
    if (queue_delay && *queue_delay > config_.queue_delay_limit) {
        worse(*queue_delay > config_.queue_delay_limit * 4 ? Verdict::severe : Verdict::congested,
              std::format("RTT {:.0f} ms is {:.0f} ms above the base RTT",
                          ms(net.rtt.value_or(decltype(net.rtt)::value_type{})), ms(*queue_delay)));
    }
    if (ack_delay && *ack_delay > config_.ack_delay_limit) {
        worse(*ack_delay > config_.ack_delay_limit * 4 ? Verdict::severe : Verdict::congested,
              std::format("frames acknowledged after {:.0f} ms",
                          ms(o.ack_round_trip.value_or(decltype(o.ack_round_trip)::value_type{}))));
    }
    if (o.queue_depth >= config_.queue_depth_limit) {
        worse(Verdict::congested, std::format("{} frames queued in the client", o.queue_depth));
    }
    if (net.bandwidth_kbps) {
        const double usable = config_.bandwidth_share * *net.bandwidth_kbps;
        fit_level = fitting_level(*net.bandwidth_kbps);
        if (fit_level > tier_.level) {
            worse(Verdict::congested, std::format("bandwidth {} kbit/s fits tier {}", *net.bandwidth_kbps, fit_level));
        } else if (send_kbps_ > usable && tier_.level + 1 < tier_count) {
            worse(Verdict::congested, std::format("sending {} of {} kbit/s measured", send_kbps_, *net.bandwidth_kbps));
        }
    }
    if (verdict != Verdict::hold) {
        return verdict;
    }

    // Clear: every signal well inside its limit, and the next tier up would
    // fit the bandwidth.
    const bool quiet = (!queue_delay || *queue_delay < config_.queue_delay_limit / 2) &&
                       (!ack_delay || *ack_delay < config_.ack_delay_limit / 2) && o.queue_depth <= 1 &&
                       o.frames_in_flight < o.max_frames_in_flight;
    const bool room = !net.bandwidth_kbps || tier_.level == 0 ||
                      budgets_.at(tier_.level - 1) <= config_.bandwidth_share * *net.bandwidth_kbps;
    return quiet && room ? Verdict::clear : Verdict::hold;
}

std::optional<QualityController::Change> QualityController::update(const Observation& o, Clock::time_point now)
{
    if (last_sample_ && now - *last_sample_ < config_.sample_interval) {
        return std::nullopt;
    }
    if (last_sample_) {
        const double seconds = std::chrono::duration<double>(now - *last_sample_).count();
        const auto bytes = o.bytes_sent >= last_bytes_ ? o.bytes_sent - last_bytes_ : 0;
        send_kbps_ = static_cast<std::uint32_t>(static_cast<double>(bytes) * 8.0 / 1000.0 / seconds);
    }
    last_sample_ = now;
    last_bytes_ = o.bytes_sent;

    std::string reason;
    unsigned fit_level = 0;
    const Verdict verdict = classify(o, reason, fit_level);
    switch (verdict) {
    case Verdict::severe:
    case Verdict::congested: {
        clear_streak_ = 0;
        ++congested_streak_;
        if (tier_.level + 1 >= tier_count) {
            return std::nullopt;
        }
        const bool settled = !last_downshift_ || now - *last_downshift_ >= config_.settle_time;
        if (verdict == Verdict::congested && (congested_streak_ < config_.downshift_samples || !settled)) {
            return std::nullopt;
        }
        // Fast downshift: straight to the tier the bandwidth allows.
        const unsigned level = std::max(tier_.level + 1, std::min(fit_level, tier_count - 1));
        if (last_upshift_ && now - *last_upshift_ < config_.failed_upshift_window) {
            upshift_wait_ = std::min(upshift_wait_ * 2, config_.max_upshift_wait);
        }
        last_downshift_ = now;
        return change_to(level, o, std::move(reason));
    }
    case Verdict::clear: {
        congested_streak_ = 0;
        ++clear_streak_;
        // A long time without congestion earns back the short upshift wait.
        if (last_downshift_ && now - *last_downshift_ >= config_.max_upshift_wait) {
            upshift_wait_ = config_.upshift_wait;
        }
        // Slow upshift: a clear link for upshift_wait, one tier at a time.
        const auto clear_for = config_.sample_interval * clear_streak_;
        if (tier_.level == 0 || clear_for < upshift_wait_) {
            return std::nullopt;
        }
        last_upshift_ = now;
        return change_to(tier_.level - 1, o,
                         std::format("link clear for {:.1f} s", std::chrono::duration<double>(clear_for).count()));
    }
    case Verdict::hold:
        congested_streak_ = 0;
        clear_streak_ = 0;
        return std::nullopt;
    }
    return std::nullopt;
}

QualityController::Change QualityController::change_to(unsigned level, const Observation& o, std::string reason)
{
    congested_streak_ = 0;
    clear_streak_ = 0;
    tier_ = make_tier(level, config_, o.network.bandwidth_kbps);
    return {tier_, std::move(reason)};
}

}  // namespace farland::server
