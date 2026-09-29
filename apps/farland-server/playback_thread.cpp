// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "playback_thread.hpp"

#include <farland/base/log.hpp>

#include <format>
#include <poll.h>

namespace farland::app {

namespace {

constexpr std::string_view log_component = "app.audio";
constexpr auto statistics_period = std::chrono::seconds(10);

std::string optional_ms(std::optional<std::chrono::milliseconds> d)
{
    return d ? std::format("{} ms", d->count()) : std::string("-");
}

}  // namespace

PlaybackThread::PlaybackThread(std::string peer, std::string transport, server::AudioPlaybackOptions options,
                               OpenCapture open_capture)
    : peer_(std::move(peer)), transport_(std::move(transport)), open_capture_(std::move(open_capture)),
      playback_(std::in_place,
                [this](std::span<const std::byte> message) {
                    {
                        const std::scoped_lock lock(mutex_);
                        outbox_.messages.emplace_back(message.begin(), message.end());
                    }
                    outbox_wake_.notify();
                },
                std::move(options)),
      thread_([this] { run(); })
{
}

PlaybackThread::~PlaybackThread()
{
    {
        const std::scoped_lock lock(mutex_);
        quit_ = true;
    }
    inbox_wake_.notify();
    thread_.join();
}

void PlaybackThread::receive(std::vector<std::byte> message)
{
    {
        const std::scoped_lock lock(mutex_);
        inbox_.push_back(std::move(message));
    }
    inbox_wake_.notify();
}

void PlaybackThread::set_bandwidth(std::optional<std::uint32_t> kbps)
{
    bool changed = false;
    {
        const std::scoped_lock lock(mutex_);
        changed = kbps != bandwidth_kbps_;
        bandwidth_kbps_ = kbps;
    }
    if (changed) {
        inbox_wake_.notify();
    }
}

PlaybackThread::Output PlaybackThread::take_output()
{
    const std::scoped_lock lock(mutex_);
    outbox_wake_.drain();
    return std::exchange(outbox_, Output{});
}

void PlaybackThread::run()
{
    playback_->start(Clock::now());
    statistics_since_ = Clock::now();
    for (;;) {
        std::vector<pollfd> fds{pollfd{inbox_wake_.fd(), POLLIN, 0}};
        if (capture_) {
            fds.push_back(pollfd{capture_->wake_fd(), POLLIN, 0});
        }
        ::poll(fds.data(), static_cast<nfds_t>(fds.size()), -1);
        inbox_wake_.drain();
        std::deque<std::vector<std::byte>> messages;
        std::optional<std::uint32_t> bandwidth;
        {
            const std::scoped_lock lock(mutex_);
            if (quit_) {
                break;
            }
            messages.swap(inbox_);
            bandwidth = bandwidth_kbps_;
        }
        if (stopped_) {
            continue;  // nothing more goes out; wait to be told to quit
        }
        playback_->set_bandwidth(bandwidth);
        for (const auto& message : messages) {
            if (auto received = playback_->receive(message, Clock::now()); !received) {
                stop(std::format("audio output protocol error: {}", received.error().message()));
                break;
            }
            poll_playback();
        }
        if (!stopped_ && capture_) {
            const auto now = Clock::now();
            samples_.clear();
            capture_->read(samples_);
            if (capture_->closed()) {
                stop(std::format("the audio capture ended: {}", capture_->error()));
                continue;
            }
            playback_->push(samples_, now);
            log_statistics(now);
        }
    }
    capture_.reset();
}

void PlaybackThread::poll_playback()
{
    while (!stopped_) {
        auto event = playback_->poll_event();
        if (!event) {
            break;
        }
        if (const auto* ready = std::get_if<server::playback_event::Ready>(&*event)) {
            auto capture = open_capture_(ready->capture);
            if (!capture) {
                stop(std::format("cannot capture the desktop's audio: {}", capture.error().message()));
                return;
            }
            capture_ = std::move(*capture);
            log::info(log_component, "{}: audio output over {}: {}", peer_, transport_, ready->codec);
        } else if (const auto* unavailable = std::get_if<server::playback_event::Unavailable>(&*event)) {
            stop(unavailable->reason);
            return;
        }
    }
}

void PlaybackThread::stop(std::string why)
{
    stopped_ = true;
    capture_.reset();
    {
        const std::scoped_lock lock(mutex_);
        outbox_.stopped = std::move(why);
    }
    outbox_wake_.notify();
}

/// Every few seconds while sound plays: packets, drops and the delays the
/// client's confirmations show.
void PlaybackThread::log_statistics(Clock::time_point now)
{
    if (now - statistics_since_ < statistics_period) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - statistics_since_).count();
    statistics_since_ = now;
    const auto stats = playback_->take_stats(now);
    if (stats.packets_sent == 0 && stats.packets_dropped == 0) {
        return;
    }
    log::info(log_component,
              "{}: audio {} packets ({:.0f} kbit/s), {} dropped, {} silent; confirmation within {}, client delay "
              "up to {}, {} ms unconfirmed",
              peer_, stats.packets_sent, static_cast<double>(stats.bytes_sent) * 8 / 1000 / seconds,
              stats.packets_dropped, stats.packets_silent, optional_ms(stats.max_round_trip),
              optional_ms(stats.max_client_delay), stats.unconfirmed.count());
}

}  // namespace farland::app
