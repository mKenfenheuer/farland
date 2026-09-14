// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "session_audio.hpp"

#include <farland/audio/opus.hpp>
#include <farland/base/log.hpp>
#include <farland/proto/client_info.hpp>
#ifdef FARLAND_HAVE_PIPEWIRE_AUDIO
#include <farland/platform/portal/pipewire_audio.hpp>
#endif

#include <format>

namespace farland::app {

namespace {

using Clock = std::chrono::steady_clock;
namespace dvc = channels::dvc_event;
constexpr std::string_view log_component = "app.audio";
constexpr std::string_view playback_channel = "AUDIO_PLAYBACK_DVC";
constexpr auto statistics_period = std::chrono::seconds(10);
/// Largest rdpsnd message a client sends: a Client Formats PDU with every format.
constexpr std::size_t max_rdpsnd_message = std::size_t{64} * 1024;

#ifdef FARLAND_HAVE_PIPEWIRE_AUDIO
constexpr bool have_backend = true;
#else
constexpr bool have_backend = false;
#endif

Result<std::unique_ptr<platform::AudioSource>> open_capture([[maybe_unused]] audio::PcmFormat format)
{
#ifdef FARLAND_HAVE_PIPEWIRE_AUDIO
    return platform::portal::capture_default_sink(format);
#else
    return fail(Errc::unsupported, "this build has no audio backend");
#endif
}

Result<std::unique_ptr<platform::AudioSink>> open_microphone([[maybe_unused]] audio::PcmFormat format)
{
#ifdef FARLAND_HAVE_PIPEWIRE_AUDIO
    return platform::portal::create_virtual_source(format);
#else
    return fail(Errc::unsupported, "this build has no audio backend");
#endif
}

server::AudioPlaybackOptions playback_options()
{
    server::AudioPlaybackOptions options;
    options.make_opus = [](audio::PcmFormat format, std::chrono::milliseconds duration, std::uint32_t bitrate) {
        return audio::opus::create_encoder(
            audio::opus::EncoderConfig{.format = format, .packet_duration = duration, .bitrate = bitrate});
    };
    return options;
}

std::string optional_ms(std::optional<std::chrono::milliseconds> d)
{
    return d ? std::format("{} ms", d->count()) : std::string("-");
}

}  // namespace

SessionAudio::SessionAudio(std::string peer, AudioOptions options)
    : peer_(std::move(peer)), options_(options), rdpsnd_reassembler_(max_rdpsnd_message)
{
}

void SessionAudio::start(const server::Session& session, SendStatic send_static)
{
    send_static_ = std::move(send_static);
    rdpsnd_channel_ = session.static_channel_id("rdpsnd");
    const std::uint32_t flags = session.info_flags;
    // [MS-RDPBCGR] 2.2.1.11.1.1: INFO_NOAUDIOPLAYBACK and INFO_REMOTECONSOLEAUDIO
    // ask for no audio on the client; INFO_AUDIOCAPTURE asks for the microphone.
    const bool client_plays =
        (flags & (proto::info_flags::no_audio_playback | proto::info_flags::remote_console_audio)) == 0;
    const bool client_records = (flags & proto::info_flags::audio_capture) != 0;
    want_playback_ = options_.playback && client_plays;
    want_microphone_ = options_.microphone && client_records;
    if (!have_backend && (want_playback_ || want_microphone_)) {
        log::info(log_component, "{}: no audio: this build has no PipeWire", peer_);
        want_playback_ = want_microphone_ = false;
        return;
    }
    log::debug(log_component, "{}: client audio: playback {}, microphone {}, rdpsnd channel {}", peer_,
               client_plays ? "yes" : "no", client_records ? "yes" : "no", rdpsnd_channel_.has_value() ? "yes" : "no");
    if (want_playback_ && !session.static_channel_id("drdynvc") && rdpsnd_channel_) {
        start_playback(Transport::static_channel);
    }
}

void SessionAudio::dynamic_channels_ready(server::DynamicChannels& channels)
{
    if (dvc_ != nullptr) {
        return;
    }
    dvc_ = &channels;
    if (want_playback_ && transport_ == Transport::none) {
        playback_dvc_id_ = channels.open(std::string(playback_channel));
    }
    if (want_microphone_) {
        input_.emplace(channels);
    }
}

bool SessionAudio::handle(const channels::DvcEvent& event)
{
    if (input_ && input_->handle(event)) {
        poll_input();
        return true;
    }
    return std::visit(
        [this](const auto& e) -> bool {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, dvc::CapabilitiesReady>) {
                return false;
            } else {
                if (!playback_dvc_id_ || e.id != *playback_dvc_id_) {
                    return false;
                }
                if constexpr (std::is_same_v<T, dvc::ChannelOpened>) {
                    start_playback(Transport::dynamic);
                } else if constexpr (std::is_same_v<T, dvc::ChannelOpenFailed>) {
                    playback_dvc_id_.reset();
                    if (rdpsnd_channel_) {
                        log::debug(log_component, "{}: no {}, audio goes over the rdpsnd channel", peer_,
                                   playback_channel);
                        start_playback(Transport::static_channel);
                    } else {
                        log::info(log_component, "{}: the client has no audio output channel", peer_);
                    }
                } else if constexpr (std::is_same_v<T, dvc::ChannelData>) {
                    on_playback_message(e.data);
                } else if constexpr (std::is_same_v<T, dvc::ChannelClosed>) {
                    playback_dvc_id_.reset();
                    stop_playback("the client closed the audio channel");
                }
                return true;
            }
        },
        event);
}

bool SessionAudio::receive_static(std::uint16_t channel_id, std::span<const std::byte> pdu)
{
    if (!rdpsnd_channel_ || channel_id != *rdpsnd_channel_) {
        return false;
    }
    if (transport_ != Transport::static_channel) {
        return true;  // the client speaks only after the server, so nothing to do
    }
    auto message = rdpsnd_reassembler_.add(pdu);
    if (!message) {
        stop_playback(std::format("rdpsnd: {}", message.error().message()));
    } else if (*message) {
        on_playback_message(**message);
    }
    return true;
}

void SessionAudio::start_playback(Transport transport)
{
    if (playback_) {
        return;
    }
    transport_ = transport;
    playback_.emplace([this](std::span<const std::byte> message) { send_playback(message); }, playback_options());
    playback_->start(Clock::now());
    statistics_since_ = Clock::now();
}

void SessionAudio::send_playback(std::span<const std::byte> message)
{
    if (transport_ == Transport::dynamic && dvc_ != nullptr && playback_dvc_id_) {
        static_cast<void>(dvc_->send(*playback_dvc_id_, message));
    } else if (transport_ == Transport::static_channel && rdpsnd_channel_) {
        for (const auto& chunk : channels::svc::encode_chunks(message)) {
            send_static_(*rdpsnd_channel_, chunk);
        }
    }
}

void SessionAudio::on_playback_message(std::span<const std::byte> message)
{
    if (!playback_) {
        return;
    }
    if (auto received = playback_->receive(message, Clock::now()); !received) {
        stop_playback(std::format("audio output protocol error: {}", received.error().message()));
        return;
    }
    poll_playback();
}

void SessionAudio::poll_playback()
{
    while (playback_) {
        auto event = playback_->poll_event();
        if (!event) {
            break;
        }
        if (const auto* ready = std::get_if<server::playback_event::Ready>(&*event)) {
            auto capture = open_capture(ready->capture);
            if (!capture) {
                stop_playback(std::format("cannot capture the desktop's audio: {}", capture.error().message()));
                return;
            }
            capture_ = std::move(*capture);
            log::info(log_component, "{}: audio output over {}: {}", peer_,
                      transport_ == Transport::dynamic ? playback_channel : "the rdpsnd channel", ready->codec);
        } else if (const auto* unavailable = std::get_if<server::playback_event::Unavailable>(&*event)) {
            stop_playback(unavailable->reason);
            return;
        }
    }
}

void SessionAudio::stop_playback(const std::string& why)
{
    if (!playback_) {
        return;
    }
    log::info(log_component, "{}: audio output stopped: {}", peer_, why);
    capture_.reset();
    playback_.reset();
    if (transport_ == Transport::dynamic && dvc_ != nullptr && playback_dvc_id_) {
        dvc_->close(*playback_dvc_id_);
        playback_dvc_id_.reset();
    }
}

void SessionAudio::poll_input()
{
    while (input_) {
        auto event = input_->poll_event();
        if (!event) {
            break;
        }
        if (const auto* opened = std::get_if<server::input_event::Opened>(&*event)) {
            auto microphone = open_microphone(opened->format);
            if (!microphone) {
                log::warn(log_component, "{}: cannot create the microphone source: {}", peer_,
                          microphone.error().message());
                continue;
            }
            microphone_ = std::move(*microphone);
            log::info(log_component, "{}: microphone: PCM {} Hz {}, as the audio source farland-microphone", peer_,
                      opened->format.rate, opened->format.channels == 1 ? "mono" : "stereo");
        } else if (const auto* samples = std::get_if<server::input_event::Samples>(&*event)) {
            if (microphone_) {
                microphone_->write(samples->samples);
                microphone_samples_ += samples->samples.size();
            }
        } else if (const auto* closed = std::get_if<server::input_event::Closed>(&*event)) {
            log::info(log_component, "{}: microphone off: {}", peer_, closed->reason);
            microphone_.reset();
        }
    }
    if (microphone_ && microphone_->closed()) {
        log::warn(log_component, "{}: the microphone source failed: {}", peer_, microphone_->error());
        microphone_.reset();
    }
}

void SessionAudio::add_fds(std::vector<pollfd>& fds) const
{
    if (capture_) {
        fds.push_back(pollfd{capture_->wake_fd(), POLLIN, 0});
    }
}

void SessionAudio::service(std::optional<std::uint32_t> bandwidth_kbps)
{
    if (!playback_ || !capture_) {
        return;
    }
    const auto now = Clock::now();
    samples_.clear();
    capture_->read(samples_);
    if (capture_->closed()) {
        stop_playback(std::format("the audio capture ended: {}", capture_->error()));
        return;
    }
    playback_->set_bandwidth(bandwidth_kbps);
    playback_->push(samples_, now);
    log_statistics(now);
}

/// Every few seconds while sound plays: packets, drops and the delays the
/// client's confirmations show.
void SessionAudio::log_statistics(Clock::time_point now)
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
              "up to {}, {} ms unconfirmed{}",
              peer_, stats.packets_sent, static_cast<double>(stats.bytes_sent) * 8 / 1000 / seconds,
              stats.packets_dropped, stats.packets_silent, optional_ms(stats.max_round_trip),
              optional_ms(stats.max_client_delay), stats.unconfirmed.count(),
              microphone_ ? std::format("; microphone {} samples", std::exchange(microphone_samples_, 0)) : "");
}

}  // namespace farland::app
