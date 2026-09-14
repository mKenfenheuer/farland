// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/base/log.hpp>
#include <farland/server/audio_playback.hpp>

#include <algorithm>
#include <format>

namespace farland::server {

namespace {

namespace rdpsnd = channels::rdpsnd;
using std::chrono::milliseconds;
constexpr std::string_view log_component = "server.audio";
/// How long a lowest-backlog observation counts for flow control.
constexpr auto floor_window = std::chrono::seconds(5);
/// At most this much PCM waits for a whole packet; more means the caller
/// pushes faster than real time, and the oldest goes.
constexpr milliseconds max_pending{200};

const rdpsnd::AudioFormat pcm48 = rdpsnd::pcm_format(48000, 2);
const rdpsnd::AudioFormat pcm44 = rdpsnd::pcm_format(44100, 2);
const rdpsnd::AudioFormat pcm22 = rdpsnd::pcm_format(22050, 2);

std::string describe(const rdpsnd::AudioFormat& format, bool opus, std::uint32_t bitrate)
{
    const char* layout = format.channels == 1 ? "mono" : "stereo";
    if (opus) {
        return std::format("Opus {} Hz {} at {} kbit/s", format.samples_per_sec, layout, bitrate / 1000);
    }
    return std::format("PCM {} Hz {} 16-bit ({} kbit/s)", format.samples_per_sec, layout,
                       format.avg_bytes_per_sec * 8 / 1000);
}

}  // namespace

rdpsnd::AudioFormat AudioPlayback::opus_format()
{
    // nAvgBytesPerSec and nBlockAlign are nominal: Opus packets vary in size.
    return rdpsnd::AudioFormat{
        .tag = rdpsnd::format_tag::opus,
        .channels = 2,
        .samples_per_sec = 48000,
        .avg_bytes_per_sec = 12000,
        .block_align = 4,
        .bits_per_sample = 16,
        .extra = {},
    };
}

namespace {

std::vector<rdpsnd::AudioFormat> formats_to_offer(bool opus)
{
    std::vector<rdpsnd::AudioFormat> formats;
    if (opus) {
        formats.push_back(AudioPlayback::opus_format());
    }
    formats.insert(formats.end(), {pcm48, pcm44, pcm22});
    return formats;
}

std::unique_ptr<audio::Encoder> probe_opus(const AudioPlaybackOptions& options)
{
    if (!options.make_opus) {
        return nullptr;
    }
    auto encoder = options.make_opus(audio::PcmFormat{48000, 2}, options.packet_duration, 96000);
    if (!encoder) {
        log::info(log_component, "no Opus for audio output ({}), PCM only", encoder.error().message());
        return nullptr;
    }
    return std::move(*encoder);
}

}  // namespace

AudioPlayback::AudioPlayback(SendMessage send, AudioPlaybackOptions options)
    : send_(std::move(send)), options_(std::move(options)), opus_(probe_opus(options_)),
      offered_(formats_to_offer(opus_ != nullptr)),
      rdpsnd_(channels::RdpsndServerConfig{.formats = offered_, .version = rdpsnd::version::windows_8})
{
}

void AudioPlayback::start(Clock::time_point now)
{
    epoch_ = now;
    floor_window_start_ = now;
    rdpsnd_.start();
    flush();
}

std::uint32_t AudioPlayback::millis(Clock::time_point t) const
{
    return static_cast<std::uint32_t>(std::chrono::duration_cast<milliseconds>(t - epoch_).count());
}

Result<void> AudioPlayback::receive(std::span<const std::byte> message, Clock::time_point now)
{
    FARLAND_TRY_VOID(rdpsnd_.receive(message, millis(now)));
    while (auto event = rdpsnd_.poll_event()) {
        std::visit(
            [this, now](const auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, channels::rdpsnd_event::Ready>) {
                    choose_format(e);
                } else if constexpr (std::is_same_v<T, channels::rdpsnd_event::Unavailable>) {
                    events_.emplace_back(playback_event::Unavailable{std::string(e.reason)});
                } else if constexpr (std::is_same_v<T, channels::rdpsnd_event::WaveConfirmed>) {
                    on_confirm(e, now);
                }
            },
            *event);
    }
    flush();
    return {};
}

std::optional<PlaybackEvent> AudioPlayback::poll_event()
{
    if (events_.empty()) {
        return std::nullopt;
    }
    PlaybackEvent event = std::move(events_.front());
    events_.pop_front();
    return event;
}

/// [MS-RDPEA] 3.1.1.4: HIGH_QUALITY takes PCM (unless the measured
/// bandwidth cannot carry it); DYNAMIC and MEDIUM take Opus where both sides
/// have it, sized by the quality mode and the bandwidth.
void AudioPlayback::choose_format(const channels::rdpsnd_event::Ready& ready)
{
    quality_mode_ = ready.quality_mode.value_or(rdpsnd::quality::dynamic);
    const auto pcm_kbps = pcm48.avg_bytes_per_sec * 8 / 1000;
    const bool pcm_fits = !bandwidth_kbps_ || *bandwidth_kbps_ >= 4 * pcm_kbps;
    const bool prefer_pcm = quality_mode_ == rdpsnd::quality::high && pcm_fits;
    std::vector<rdpsnd::AudioFormat> order;
    if (prefer_pcm) {
        order = {pcm48, pcm44, opus_format(), pcm22};
    } else {
        order = {opus_format(), pcm48, pcm44, pcm22};
    }
    for (const auto& wanted : order) {
        if (wanted.tag == rdpsnd::format_tag::opus && !opus_) {
            continue;
        }
        const auto found = std::ranges::find(ready.formats, wanted);
        if (found == ready.formats.end()) {
            continue;
        }
        format_no_ = static_cast<std::uint16_t>(found - ready.formats.begin());
        use_opus_ = wanted.tag == rdpsnd::format_tag::opus;
        capture_ = audio::PcmFormat{wanted.samples_per_sec, wanted.channels};
        if (use_opus_) {
            opus_->set_bitrate(opus_bitrate());
        } else {
            opus_.reset();
        }
        events_.emplace_back(playback_event::Ready{capture_, describe(wanted, use_opus_, opus_bitrate())});
        return;
    }
    // Every format of the client's list is one farland offered; reaching here
    // would take a client that agreed to none of the usable ones.
    events_.emplace_back(playback_event::Unavailable{"no usable audio format in common with the client"});
}

std::uint32_t AudioPlayback::opus_bitrate() const
{
    if (quality_mode_ == rdpsnd::quality::high) {
        return 128000;
    }
    if (quality_mode_ == rdpsnd::quality::medium) {
        return 64000;
    }
    if (!bandwidth_kbps_) {
        return 96000;
    }
    // About a twentieth of the link, between speech and transparent music.
    return std::clamp<std::uint32_t>(*bandwidth_kbps_ * 1000 / 20, 32000, 128000);
}

void AudioPlayback::set_bandwidth(std::optional<std::uint32_t> kbps)
{
    if (kbps == bandwidth_kbps_) {
        return;
    }
    bandwidth_kbps_ = kbps;
    if (use_opus_ && opus_) {
        opus_->set_bitrate(opus_bitrate());
    }
}

std::optional<audio::PcmFormat> AudioPlayback::capture_format() const
{
    if (!ready()) {
        return std::nullopt;
    }
    return capture_;
}

void AudioPlayback::push(std::span<const std::int16_t> samples, Clock::time_point now)
{
    if (!ready()) {
        return;
    }
    const std::size_t packet = capture_.samples(options_.packet_duration);
    FARLAND_ASSERT(packet > 0);
    pending_.insert(pending_.end(), samples.begin(), samples.end());
    const std::size_t limit = capture_.samples(max_pending);
    if (pending_.size() > limit) {
        const std::size_t excess = (pending_.size() - limit) / capture_.channels * capture_.channels;
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(excess));
        stats_.packets_dropped += excess / packet;
    }
    std::size_t offset = 0;
    while (pending_.size() - offset >= packet) {
        send_packet(std::span(pending_).subspan(offset, packet), now);
        offset += packet;
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(offset));
    flush();
}

milliseconds AudioPlayback::unconfirmed() const
{
    return options_.packet_duration * static_cast<std::int64_t>(in_flight_.size());
}

/// Flow control: a client or network that falls behind gets fewer packets
/// rather than a growing queue, which would put the audio ever later.
bool AudioPlayback::should_drop(Clock::time_point now)
{
    while (!in_flight_.empty() && now - in_flight_.front().sent > options_.confirm_timeout) {
        in_flight_.pop_front();
    }
    const auto backlog = unconfirmed() + options_.packet_duration;
    std::optional<milliseconds> floor = floor_current_;
    if (floor_previous_ && (!floor || *floor_previous_ < *floor)) {
        floor = floor_previous_;
    }
    const milliseconds limit =
        floor ? std::min(*floor + options_.max_backlog, options_.hard_limit) : options_.startup_limit;
    return backlog > std::max(limit, options_.packet_duration);
}

void AudioPlayback::send_packet(std::span<const std::int16_t> packet, Clock::time_point now)
{
    const std::optional<std::uint16_t> format_no = format_no_;
    if (!format_no) {
        return;  // push() calls this only once ready
    }
    const bool silent = std::ranges::all_of(packet, [](std::int16_t s) { return s == 0; });
    if (silent) {
        silent_ += options_.packet_duration;
        if (silent_ >= options_.silence_timeout) {
            if (!idle_) {
                // [MS-RDPEA] 3.3.5.2.1.7: the audio stopped.
                rdpsnd_.send_close();
                idle_ = true;
            }
            ++stats_.packets_silent;
            return;
        }
    } else {
        silent_ = milliseconds{0};
        idle_ = false;
    }
    if (should_drop(now)) {
        ++stats_.packets_dropped;
        return;
    }
    std::vector<std::byte> data;
    if (use_opus_) {
        data = opus_->encode(packet);
        if (data.empty()) {
            ++stats_.packets_dropped;
            return;
        }
    } else {
        Writer w(packet.size() * 2);
        for (const std::int16_t sample : packet) {
            w.u16le(static_cast<std::uint16_t>(sample));
        }
        data = std::move(w).take();
    }
    const auto audio_ms = static_cast<std::uint32_t>(
        std::chrono::duration_cast<milliseconds>(now.time_since_epoch()).count() & 0xFFFFFFFFU);
    const std::uint8_t block = rdpsnd_.send_wave(*format_no, data, millis(now), audio_ms);
    in_flight_.push_back(InFlight{block, now});
    ++stats_.packets_sent;
    stats_.bytes_sent += data.size();
}

void AudioPlayback::on_confirm(const channels::rdpsnd_event::WaveConfirmed& confirm, Clock::time_point now)
{
    if (confirm.sent_timestamp) {
        const auto delay = milliseconds(static_cast<std::uint16_t>(confirm.timestamp - *confirm.sent_timestamp));
        if (delay.count() < 0x8000 && (!stats_.max_client_delay || delay > *stats_.max_client_delay)) {
            stats_.max_client_delay = delay;
        }
    }
    // Clients confirm in order: this block, and any older one still listed.
    const auto found = std::ranges::find(in_flight_, confirm.block_no, &InFlight::block);
    if (found == in_flight_.end()) {
        return;  // a second confirmation (FreeRDP sends two), or one for a sample given up on
    }
    const auto round_trip = std::chrono::duration_cast<milliseconds>(now - found->sent);
    if (!stats_.max_round_trip || round_trip > *stats_.max_round_trip) {
        stats_.max_round_trip = round_trip;
    }
    in_flight_.erase(in_flight_.begin(), std::next(found));

    if (now - floor_window_start_ > floor_window) {
        floor_previous_ = floor_current_;
        floor_current_.reset();
        floor_window_start_ = now;
    }
    const auto backlog = unconfirmed();
    if (!floor_current_ || backlog < *floor_current_) {
        floor_current_ = backlog;
    }
}

PlaybackStats AudioPlayback::take_stats(Clock::time_point /*now*/)
{
    stats_.unconfirmed = unconfirmed();
    return std::exchange(stats_, PlaybackStats{});
}

void AudioPlayback::flush()
{
    for (const auto& message : rdpsnd_.take_output()) {
        send_(message);
    }
}

}  // namespace farland::server
