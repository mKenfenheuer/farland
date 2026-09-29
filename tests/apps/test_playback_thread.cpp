// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "playback_thread.hpp"
#include "wake_pipe.hpp"

#include <farland/channels/rdpsnd.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <poll.h>
#include <thread>
#include <vector>

namespace rdpsnd = farland::channels::rdpsnd;
using farland::app::PlaybackThread;
using farland::app::WakePipe;
using Bytes = std::vector<std::byte>;

namespace {

/// The desktop's sound: the test puts samples in, the audio thread reads them.
class FakeCapture final : public farland::platform::AudioSource {
public:
    [[nodiscard]] int wake_fd() const override { return wake_.fd(); }
    void read(std::vector<std::int16_t>& out) override
    {
        const std::scoped_lock lock(mutex_);
        wake_.drain();
        out.insert(out.end(), samples_.begin(), samples_.end());
        samples_.clear();
        if (closed_) {
            wake_.notify();  // stays readable once closed
        }
    }
    [[nodiscard]] bool closed() const override { return closed_.load(); }
    [[nodiscard]] std::string error() const override { return closed_ ? "the sink went away" : ""; }

    void play(std::size_t samples)
    {
        {
            const std::scoped_lock lock(mutex_);
            for (std::size_t i = 0; i < samples; ++i) {
                samples_.push_back(static_cast<std::int16_t>(4000 * std::sin(static_cast<double>(i) / 10)));
            }
        }
        wake_.notify();
    }
    void close()
    {
        closed_ = true;
        wake_.notify();
    }

private:
    WakePipe wake_;
    std::mutex mutex_;
    std::vector<std::int16_t> samples_;
    std::atomic<bool> closed_{false};
};

/// What the audio thread owns and destroys when the capture ends: the test
/// keeps the capture itself, so that closing it cannot race with its end.
class CaptureHandle final : public farland::platform::AudioSource {
public:
    explicit CaptureHandle(std::shared_ptr<FakeCapture> capture) : capture_(std::move(capture)) {}
    [[nodiscard]] int wake_fd() const override { return capture_->wake_fd(); }
    void read(std::vector<std::int16_t>& out) override { capture_->read(out); }
    [[nodiscard]] bool closed() const override { return capture_->closed(); }
    [[nodiscard]] std::string error() const override { return capture_->error(); }

private:
    std::shared_ptr<FakeCapture> capture_;
};

/// Waits for the audio thread's next messages; the stop reason lands in `stopped`.
std::vector<rdpsnd::ServerPdu> take(PlaybackThread& thread, std::optional<std::string>& stopped)
{
    pollfd pfd{.fd = thread.wake_fd(), .events = POLLIN, .revents = 0};
    REQUIRE(::poll(&pfd, 1, 5000) == 1);
    auto output = thread.take_output();
    if (output.stopped) {
        stopped = output.stopped;
    }
    std::vector<rdpsnd::ServerPdu> pdus;
    for (const auto& message : output.messages) {
        pdus.push_back(rdpsnd::decode_server_pdu(message).value());
    }
    return pdus;
}

/// The initialization sequence of a client that takes 48 kHz PCM.
void negotiate(PlaybackThread& thread, std::optional<std::string>& stopped)
{
    auto pdus = take(thread, stopped);
    REQUIRE(pdus.size() == 1);
    const auto server = std::get<rdpsnd::ServerFormats>(pdus[0]);
    rdpsnd::ClientFormats formats{.flags = rdpsnd::caps::alive, .version = 8, .formats = {}};
    for (const auto& format : server.formats) {
        if (format.tag == rdpsnd::format_tag::pcm && format.samples_per_sec == 48000) {
            formats.formats.push_back(format);
        }
    }
    thread.receive(rdpsnd::encode_client_pdu(formats));
    thread.receive(rdpsnd::encode_client_pdu(rdpsnd::QualityMode{rdpsnd::quality::high}));
    pdus = take(thread, stopped);
    const auto training = std::get<rdpsnd::Training>(pdus.at(0));
    thread.receive(rdpsnd::encode_client_pdu(rdpsnd::TrainingConfirm{training.timestamp, 0}));
}

}  // namespace

TEST_CASE("The playback thread captures, encodes and hands out audio on its own")
{
    auto sound = std::make_shared<FakeCapture>();
    std::atomic<std::thread::id> opened_on{};
    PlaybackThread thread("test", "the rdpsnd channel", {},
                          [&](farland::audio::PcmFormat format) -> farland::Result<std::unique_ptr<farland::platform::AudioSource>> {
                              CHECK(format.rate == 48000);
                              opened_on = std::this_thread::get_id();
                              return std::unique_ptr<farland::platform::AudioSource>(
                                  std::make_unique<CaptureHandle>(sound));
                          });
    std::optional<std::string> stopped;
    negotiate(thread, stopped);

    // 20 ms of stereo sound at 48 kHz: one packet, sent without the caller
    // doing anything but carrying it.
    while (opened_on.load() == std::thread::id{}) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(opened_on.load() != std::this_thread::get_id());
    sound->play(48 * 20 * 2);
    const auto pdus = take(thread, stopped);
    REQUIRE(pdus.size() == 1);
    const auto& wave = std::get<rdpsnd::Wave2>(pdus[0]);
    CHECK(wave.data.size() == std::size_t{48 * 20 * 2 * 2});
    CHECK_FALSE(stopped);

    // The capture ending stops audio output, with the reason.
    sound->close();
    std::vector<rdpsnd::ServerPdu> rest;
    while (!stopped) {
        const auto more = take(thread, stopped);
        rest.insert(rest.end(), more.begin(), more.end());
    }
    CHECK(stopped->find("the sink went away") != std::string::npos);
}

TEST_CASE("The playback thread stops audio when the desktop's sound cannot be captured")
{
    PlaybackThread thread("test", "the rdpsnd channel", {},
                          [](farland::audio::PcmFormat) -> farland::Result<std::unique_ptr<farland::platform::AudioSource>> {
                              return farland::fail(farland::Errc::unsupported, "no PipeWire");
                          });
    std::optional<std::string> stopped;
    negotiate(thread, stopped);
    while (!stopped) {
        static_cast<void>(take(thread, stopped));
    }
    CHECK(stopped->find("no PipeWire") != std::string::npos);
}
