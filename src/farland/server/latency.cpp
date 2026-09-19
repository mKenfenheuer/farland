// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/server/latency.hpp>

#include <algorithm>
#include <format>

namespace farland::server {

namespace {

using Duration = LatencyTracker::Duration;

/// The value at `fraction` of a sorted range, nearest rank. Percentiles of a
/// handful of frames mean little, but they are not wrong: with one sample
/// every percentile is that sample.
[[nodiscard]] Duration percentile(std::vector<Duration>& values, double fraction)
{
    if (values.empty()) {
        return Duration{};
    }
    std::ranges::sort(values);
    const auto last = static_cast<double>(values.size() - 1);
    const auto index = static_cast<std::size_t>(fraction * last + 0.5);
    return values[std::min(index, values.size() - 1)];
}

[[nodiscard]] double ms(Duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

}  // namespace

void LatencyTracker::frame_taken(Clock::time_point now, std::optional<Clock::time_point> captured)
{
    Pending pending;
    pending.taken = now;
    // A capture time from after the frame was taken is not one; treat the
    // source as having none rather than reporting a negative wait.
    pending.captured = (captured && *captured <= now) ? captured : std::nullopt;
    if (!pending.captured) {
        ++without_capture_time_;
    }
    current_ = pending;
}

void LatencyTracker::frame_read(Clock::time_point now)
{
    if (current_) {
        current_->read = now;
        current_->has_read = true;
    }
}

void LatencyTracker::frame_encoded(Clock::time_point now)
{
    if (current_) {
        current_->encoded = now;
        current_->has_encoded = true;
    }
}

void LatencyTracker::frame_sent(std::uint32_t frame_id, Clock::time_point now)
{
    if (!current_) {
        return;
    }
    Pending pending = *current_;
    current_.reset();
    pending.frame_id = frame_id;
    pending.sent = now;
    // A stage nobody reported takes no time, rather than borrowing from the
    // next one: reading and encoding then read as 0 and the whole cost shows
    // up where it was actually spent.
    if (!pending.has_read) {
        pending.read = pending.taken;
    }
    if (!pending.has_encoded) {
        pending.encoded = pending.read;
    }
    in_flight_.push_back(pending);
    // A client that never acknowledges must not grow this without end; the
    // scheduler keeps only a couple of frames in flight, so anything beyond
    // that is lost acknowledgements.
    while (in_flight_.size() > 64) {
        in_flight_.pop_front();
    }
}

void LatencyTracker::frame_acknowledged(std::uint32_t frame_id, Clock::time_point now)
{
    const auto it = std::ranges::find(in_flight_, frame_id, &Pending::frame_id);
    if (it == in_flight_.end()) {
        return;
    }
    const Pending pending = *it;
    // Acknowledging a frame acknowledges the ones before it, which are gone
    // unmeasured rather than credited with this frame's time.
    in_flight_.erase(in_flight_.begin(), it + 1);

    Sample sample;
    sample.waiting =
        pending.captured ? std::chrono::duration_cast<Duration>(pending.taken - *pending.captured) : Duration{};
    sample.reading = std::chrono::duration_cast<Duration>(pending.read - pending.taken);
    sample.encoding = std::chrono::duration_cast<Duration>(pending.sent - pending.read);
    sample.client = std::chrono::duration_cast<Duration>(now - pending.sent);
    if (samples_.size() >= capacity_) {
        samples_.erase(samples_.begin());
    }
    samples_.push_back(sample);
}

LatencyTracker::Summary LatencyTracker::summary() const
{
    Summary out;
    out.frames = samples_.size();
    out.without_capture_time = without_capture_time_;
    if (samples_.empty()) {
        return out;
    }
    const auto collect = [this](auto get) {
        std::vector<Duration> values;
        values.reserve(samples_.size());
        for (const auto& sample : samples_) {
            values.push_back(get(sample));
        }
        return values;
    };
    auto waiting = collect([](const Sample& s) { return s.waiting; });
    auto reading = collect([](const Sample& s) { return s.reading; });
    auto encoding = collect([](const Sample& s) { return s.encoding; });
    auto client = collect([](const Sample& s) { return s.client; });
    auto server = collect([](const Sample& s) { return s.server(); });
    auto total = collect([](const Sample& s) { return s.total(); });

    out.waiting_p50 = percentile(waiting, 0.50);
    out.waiting_p95 = percentile(waiting, 0.95);
    out.reading_p50 = percentile(reading, 0.50);
    out.reading_p95 = percentile(reading, 0.95);
    out.encoding_p50 = percentile(encoding, 0.50);
    out.encoding_p95 = percentile(encoding, 0.95);
    out.client_p50 = percentile(client, 0.50);
    out.client_p95 = percentile(client, 0.95);
    out.server_p50 = percentile(server, 0.50);
    out.server_p95 = percentile(server, 0.95);
    out.server_max = server.empty() ? Duration{} : server.back();  // sorted by percentile()
    out.total_p50 = percentile(total, 0.50);
    out.total_p95 = percentile(total, 0.95);
    out.total_max = total.empty() ? Duration{} : total.back();
    return out;
}

std::string LatencyTracker::describe(const Summary& summary)
{
    if (summary.frames == 0) {
        return "no frames were acknowledged, so there is nothing to report";
    }
    std::string out = std::format("{} frames, milliseconds as p50/p95:\n", summary.frames);
    out += std::format("  waiting for the scheduler  {:6.2f} / {:6.2f}\n", ms(summary.waiting_p50),
                       ms(summary.waiting_p95));
    out += std::format("  reading the pixels         {:6.2f} / {:6.2f}\n", ms(summary.reading_p50),
                       ms(summary.reading_p95));
    out += std::format("  encoding and framing       {:6.2f} / {:6.2f}\n", ms(summary.encoding_p50),
                       ms(summary.encoding_p95));
    out += std::format("  server, all three          {:6.2f} / {:6.2f}   (worst {:.2f})\n", ms(summary.server_p50),
                       ms(summary.server_p95), ms(summary.server_max));
    out +=
        std::format("  wire, decode and ack       {:6.2f} / {:6.2f}\n", ms(summary.client_p50), ms(summary.client_p95));
    out += std::format("  measured end to end        {:6.2f} / {:6.2f}   (worst {:.2f})\n", ms(summary.total_p50),
                       ms(summary.total_p95), ms(summary.total_max));
    if (summary.without_capture_time > 0) {
        out += std::format("  ({} frames came without a capture time; their wait is counted as zero)\n",
                           summary.without_capture_time);
    }
    out += "  the client's own compositor and screen are not in this and no server can see them";
    return out;
}

}  // namespace farland::server
