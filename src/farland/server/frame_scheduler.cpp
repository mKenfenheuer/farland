// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/server/frame_scheduler.hpp>

#include <algorithm>

namespace farland::server {

namespace {

FrameScheduler::Clock::duration frame_interval(unsigned fps)
{
    return std::chrono::duration_cast<FrameScheduler::Clock::duration>(
        std::chrono::microseconds(1'000'000 / std::max(fps, 1U)));
}

}  // namespace

FrameScheduler::FrameScheduler(Config config) : config_(config), interval_(frame_interval(config.max_fps))
{
    config_.max_frames_in_flight = std::max<std::size_t>(config_.max_frames_in_flight, 1);
}

void FrameScheduler::set_max_fps(unsigned fps) noexcept
{
    config_.max_fps = fps;
    interval_ = frame_interval(fps);
}

void FrameScheduler::frame_sent(std::uint32_t frame_id, Clock::time_point now)
{
    pending_ = false;
    last_sent_ = now;
    if (gated_by_acks()) {
        in_flight_.push_back({frame_id, now});
    }
}

void FrameScheduler::frame_acknowledged(std::uint32_t frame_id, Clock::time_point now)
{
    const auto found = std::ranges::find(in_flight_, frame_id, &InFlight::frame_id);
    if (found == in_flight_.end()) {
        return;
    }
    const auto sample = now - found->sent;
    // Exponentially weighted like TCP's SRTT (RFC 6298): 7/8 old, 1/8 new.
    round_trip_ = round_trip_ ? (*round_trip_ * 7 + sample) / 8 : sample;
    // Acknowledgements arrive in order; anything older was lost or skipped.
    in_flight_.erase(in_flight_.begin(), std::next(found));
}

void FrameScheduler::set_acknowledgements_suspended(bool suspended) noexcept
{
    suspended_ = suspended;
    if (suspended) {
        in_flight_.clear();
    }
}

FrameScheduler::Clock::duration FrameScheduler::ack_timeout() const noexcept
{
    if (!round_trip_) {
        return config_.ack_timeout;
    }
    const auto scaled = *round_trip_ * config_.ack_timeout_rtt_factor;
    // std::clamp is undefined where the floor exceeds the ceiling, which a
    // configured ack_timeout above max_ack_timeout would do.
    const auto floor = std::min(config_.ack_timeout, config_.max_ack_timeout);
    return std::clamp(scaled, floor, config_.max_ack_timeout);
}

void FrameScheduler::expire(Clock::time_point now)
{
    const auto timeout = ack_timeout();
    while (!in_flight_.empty() && now - in_flight_.front().sent >= timeout) {
        in_flight_.pop_front();
    }
}

bool FrameScheduler::due(Clock::time_point now)
{
    if (!pending_) {
        return false;
    }
    expire(now);
    if (gated_by_acks() && in_flight_.size() >= config_.max_frames_in_flight) {
        return false;
    }
    return !last_sent_ || now - *last_sent_ >= interval_;
}

std::optional<FrameScheduler::Clock::duration> FrameScheduler::wait(Clock::time_point now)
{
    if (!pending_) {
        return std::nullopt;
    }
    expire(now);
    Clock::duration wait = Clock::duration::zero();
    if (last_sent_) {
        wait = std::max(wait, *last_sent_ + interval_ - now);
    }
    if (gated_by_acks() && in_flight_.size() >= config_.max_frames_in_flight) {
        // Only an acknowledgement or the oldest frame's timeout opens the window.
        wait = std::max(wait, in_flight_.front().sent + ack_timeout() - now);
    }
    return wait;
}

}  // namespace farland::server
