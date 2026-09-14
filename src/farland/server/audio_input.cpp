// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/reader.hpp>
#include <farland/server/audio_input.hpp>

#include <format>

namespace farland::server {

namespace {

namespace dvc = channels::dvc_event;

channels::AudinServerConfig audin_config(const std::vector<audio::PcmFormat>& formats)
{
    channels::AudinServerConfig config;
    for (const auto& format : formats) {
        config.formats.push_back(channels::rdpsnd::pcm_format(format.rate, format.channels));
    }
    return config;
}

}  // namespace

std::vector<audio::PcmFormat> AudioInput::default_formats()
{
    return {{48000, 1}, {44100, 1}, {48000, 2}, {44100, 2}, {22050, 1}, {16000, 1}};
}

AudioInput::AudioInput(DynamicChannels& channels, const std::vector<audio::PcmFormat>& formats)
    : channels_(&channels), audin_(audin_config(formats)),
      channel_id_(channels_->open(std::string(channels::audin::channel_name)))
{
}

bool AudioInput::handle(const channels::DvcEvent& event)
{
    return std::visit(
        [this](const auto& e) -> bool {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, dvc::CapabilitiesReady>) {
                return false;
            } else {
                if (e.id != channel_id_ || closed_) {
                    return false;
                }
                if constexpr (std::is_same_v<T, dvc::ChannelOpened>) {
                    audin_.start();  // [MS-RDPEAI] 3.1.5.1: the server speaks first
                    flush();
                } else if constexpr (std::is_same_v<T, dvc::ChannelOpenFailed>) {
                    closed_ = true;
                    events_.emplace_back(input_event::Closed{"the client has no microphone channel"});
                } else if constexpr (std::is_same_v<T, dvc::ChannelData>) {
                    if (auto received = audin_.receive(e.data); !received) {
                        close(std::format("protocol error: {}", received.error().message()));
                        return true;
                    }
                    while (auto audin = audin_.poll_event()) {
                        if (const auto* opened = std::get_if<channels::audin_event::Opened>(&*audin)) {
                            events_.emplace_back(input_event::Opened{
                                audio::PcmFormat{opened->format.samples_per_sec, opened->format.channels}});
                        } else if (const auto* data = std::get_if<channels::audin_event::Data>(&*audin)) {
                            input_event::Samples samples;
                            samples.samples.reserve(data->data.size() / 2);
                            Reader r(data->data);
                            while (auto sample = r.u16le()) {
                                samples.samples.push_back(static_cast<std::int16_t>(*sample));
                            }
                            events_.emplace_back(std::move(samples));
                        } else if (const auto* failed = std::get_if<channels::audin_event::Failed>(&*audin)) {
                            close(failed->result != 0 ? std::format("{} ({:#010x})", failed->reason, failed->result)
                                                      : std::string(failed->reason));
                        }
                    }
                    flush();
                } else if constexpr (std::is_same_v<T, dvc::ChannelClosed>) {
                    closed_ = true;
                    events_.emplace_back(input_event::Closed{"the client closed the microphone channel"});
                }
                return true;
            }
        },
        event);
}

std::optional<InputEvent> AudioInput::poll_event()
{
    if (events_.empty()) {
        return std::nullopt;
    }
    InputEvent event = std::move(events_.front());
    events_.pop_front();
    return event;
}

void AudioInput::flush()
{
    for (const auto& message : audin_.take_output()) {
        static_cast<void>(channels_->send(channel_id_, message));
    }
}

void AudioInput::close(std::string reason)
{
    if (closed_) {
        return;
    }
    closed_ = true;
    channels_->close(channel_id_);
    events_.emplace_back(input_event::Closed{std::move(reason)});
}

}  // namespace farland::server
