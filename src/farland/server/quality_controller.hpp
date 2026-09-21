// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/codec/rfx_common.hpp>
#include <farland/server/autodetect.hpp>
#include <farland/video/h264_encoder.hpp>

#include <array>
#include <chrono>
#include <deque>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

/// Congestion control for the Graphics Pipeline: a ladder of quality tiers
/// (docs/PLAN.md §3.4, after ZeroVDI's bridge). Tier 0 is full quality;
/// every tier below it trades picture quality, then frame rate, for bytes:
/// coarser Progressive quantization, a higher H.264 CRF with a lower VBV
/// cap, and from tier 2 on fewer frames per second.
///
/// The controller samples the session every `sample_interval` and classifies
/// the link as congested, clear or neither from
/// - queueing delay: the auto-detect RTT above the base RTT, beyond what the
///   link's own jitter explains (`jitter_allowance`); the session's own output
///   queueing up in front of the probes shows here first. The age of an
///   unanswered probe counts too, but only as ordinary congestion: it is a
///   lower bound on the round trip rather than a measurement;
/// - the frame acknowledgement round trip above the *lowest* one seen (the
///   client's own decode cost, which is not congestion), with the same
///   allowance for jitter;
/// - the client's queueDepth (frames waiting for its decoder);
/// - bandwidth: a tier whose bit budget does not fit the measured bandwidth,
///   or output close to it.
///
/// Hysteresis: it steps down after `downshift_samples` congested samples in
/// a row (on the first one when the congestion is severe), straight to the
/// tier the bandwidth allows if that is lower, and never more than one step
/// per `settle_time` -- severe included, so a burst of bad samples at connect
/// cannot walk the ladder to its bottom before the link has been seen. It steps up one tier after `upshift_wait` of clear samples; an
/// upshift that is followed by a downshift within `failed_upshift_window`
/// doubles that wait (up to `max_upshift_wait`), so the ladder does not flap
/// around the link's capacity. Sans-IO: every call takes the time.
namespace farland::server {

struct QualityTier {
    unsigned level = 0;  ///< 0 is the best
    std::string_view name;
    unsigned fps = 30;
    codec::rfx::Quant progressive_quant;
    video::RateControl h264;
    /// The bit rate the tier is designed for, kbit/s (the H.264 budget).
    std::uint32_t budget_kbps = 0;
    /// AVC444 sends the chroma view only once the picture stops changing
    /// (or after a few frames), which halves the H.264 pictures while it moves.
    bool defer_chroma = false;
};

class QualityController {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr unsigned tier_count = 4;

    struct Config {
        /// Surface size and the session's frame rate (tier 0's rate, and the
        /// nominal rate the H.264 encoder budgets with).
        std::uint32_t width = 1920;
        std::uint32_t height = 1080;
        unsigned fps = 30;
        /// H.264 pictures the session puts on the wire for every frame: one,
        /// or two for AVC444, which sends a luma and a chroma view through
        /// the same encoder. The encoder budgets each picture against its
        /// nominal frame rate, so with two views its cap has to be half the
        /// tier's, or the stream comes out at twice the budget.
        unsigned pictures_per_frame = 1;

        Clock::duration sample_interval = std::chrono::milliseconds(500);
        unsigned downshift_samples = 2;
        Clock::duration settle_time = std::chrono::seconds(2);
        Clock::duration upshift_wait = std::chrono::seconds(5);
        Clock::duration max_upshift_wait = std::chrono::seconds(60);
        Clock::duration failed_upshift_window = std::chrono::seconds(15);

        /// RTT above the base RTT that counts as congestion; four times as
        /// much is severe. This is a floor: see `jitter_allowance`.
        Clock::duration queue_delay_limit = std::chrono::milliseconds(80);
        /// Multiples of the measured round-trip variation to allow on top of
        /// the base RTT before a sample counts as queueing, where that is
        /// more than the fixed limits above.
        ///
        /// The base RTT is the *lowest* round trip seen, so on a link that
        /// varies -- mobile above all, and Wi-Fi -- an ordinary sample sits
        /// well above it with nothing queued anywhere. A fixed 80 ms limit
        /// against a link whose jitter is 50-80 ms reads as congestion
        /// permanently, and the ladder then lives at its bottom tier on a
        /// link with tens of Mbit/s to spare. Allowing for the variation the
        /// link actually shows keeps the signal to real queueing.
        unsigned jitter_allowance = 2;
        /// Frame acknowledgement round trip above its own best case that
        /// counts as congestion; four times as much is severe.
        ///
        /// Measured against the lowest acknowledgement round trip seen, not
        /// against the base RTT: an acknowledgement covers the wire *and* the
        /// client decoding the frame, so a phone that steadily needs 250 ms
        /// to decode would otherwise be read as a permanently congested link.
        /// What says a client is falling behind is that time growing.
        Clock::duration ack_delay_limit = std::chrono::milliseconds(250);
        /// Acknowledgement samples the best case is the lowest of.
        std::size_t base_ack_window = 120;
        /// Client queueDepth from which the client counts as behind.
        std::uint32_t queue_depth_limit = 3;
        /// Share of the measured bandwidth the graphics may use.
        double bandwidth_share = 0.8;
        /// How long a bandwidth measurement is worth acting on.
        ///
        /// Continuous measurements ride on a burst of real output, so a
        /// session sitting at a low tier sends too little to trigger one and
        /// keeps whatever connect time measured. A reading that came out too
        /// low then pins the picture for the whole session: the tier is too
        /// low to produce a burst, and without a burst there is no new
        /// reading. Past this age the number stops blocking a step up (and
        /// stops forcing one down); the delay signals still say when the link
        /// is really full, so the ladder probes upward and backs off, which
        /// is also what produces the burst that measures it again.
        Clock::duration bandwidth_freshness = std::chrono::seconds(15);

        /// What the H.264 tiers may spend, in kbit/s. The ladder works out a
        /// cap per tier from the surface, the frame rate and the tier's
        /// bits per pixel; these bound the result, so a link with a known
        /// capacity (or a metered one) can be told about it rather than
        /// discovered.
        ///
        /// `min_bitrate_kbps` is the floor no tier drops below -- the point
        /// at which a smaller picture is worse than a slower one.
        /// `max_bitrate_kbps` is a ceiling on every tier; 0 leaves the
        /// ladder's own numbers alone. `target_bitrate_kbps` replaces
        /// constant quality with average bitrate: the top tier aims at it
        /// and the lower tiers scale down with their bits per pixel, which
        /// is what an administrator asking for "3 Mbit/s" means.
        std::uint32_t min_bitrate_kbps = 300;
        std::uint32_t max_bitrate_kbps = 0;
        std::uint32_t target_bitrate_kbps = 0;
    };

    /// What the session knows at a sample.
    struct Observation {
        NetworkEstimate network;
        std::size_t frames_in_flight = 0;
        std::size_t max_frames_in_flight = 1;
        /// The client's queueDepth from its last frame acknowledgement.
        std::uint32_t queue_depth = 0;
        std::optional<Clock::duration> ack_round_trip;
        /// All bytes the session has sent so far.
        std::uint64_t bytes_sent = 0;
    };

    struct Change {
        QualityTier tier;
        std::string reason;
    };

    /// `initial_bandwidth_kbps` is what connect-time auto-detect measured,
    /// where the client answered it. Without it the ladder starts at its
    /// best tier and finds the link by congesting it -- which the first
    /// frame pays for, and the first frame is the whole screen. With it the
    /// ladder starts where the link already is.
    explicit QualityController(Config config, std::optional<std::uint32_t> initial_bandwidth_kbps = std::nullopt);

    /// Feeds a sample (calls between sample intervals are ignored). Returns
    /// the new tier when it changes.
    [[nodiscard]] std::optional<Change> update(const Observation& observation, Clock::time_point now);

    [[nodiscard]] const QualityTier& tier() const noexcept { return tier_; }
    /// The output rate over the last sample interval, kbit/s.
    [[nodiscard]] std::uint32_t send_kbps() const noexcept { return send_kbps_; }

    /// Tier `level` for this configuration; with a bandwidth, the H.264 cap
    /// stays within the share of it the graphics may use.
    [[nodiscard]] static QualityTier make_tier(unsigned level, const Config& config,
                                               std::optional<std::uint32_t> bandwidth_kbps = std::nullopt);

private:
    enum class Verdict : std::uint8_t { clear, hold, congested, severe };

    [[nodiscard]] Verdict classify(const Observation& o, Clock::time_point now, std::string& reason,
                                   unsigned& fit_level) const;
    [[nodiscard]] Change change_to(unsigned level, const Observation& o, std::string reason);
    /// The lowest level whose budget fits `bandwidth_kbps`.
    [[nodiscard]] unsigned fitting_level(std::uint32_t bandwidth_kbps) const noexcept;

    Config config_;
    QualityTier tier_;
    /// The lowest acknowledgement round trip of the recent ones: what this
    /// client costs when nothing is queued.
    std::deque<Clock::duration> recent_acks_;
    std::optional<Clock::duration> base_ack_;
    std::array<std::uint32_t, tier_count> budgets_{};
    std::optional<Clock::time_point> last_sample_;
    std::uint64_t last_bytes_ = 0;
    std::uint32_t send_kbps_ = 0;
    unsigned congested_streak_ = 0;
    unsigned clear_streak_ = 0;
    Clock::duration upshift_wait_;
    std::optional<Clock::time_point> last_downshift_;
    std::optional<Clock::time_point> last_upshift_;
};

}  // namespace farland::server
