// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/server/touch_input.hpp>

#include <type_traits>
#include <utility>

namespace farland::server {

namespace {

namespace dvc = channels::dvc_event;
namespace rdpei = channels::rdpei;

constexpr std::string_view component = "server.rdpei";
/// Malformed messages logged as warnings; later ones go to debug.
constexpr std::uint64_t max_logged_malformed = 3;

}  // namespace

TouchInput::TouchInput(DynamicChannels& channels, rdpei::RdpeiServerConfig config)
    : channels_(&channels), rdpei_(config)
{
    channel_id_ = channels_->open(std::string(channel_name),
                                  channels::DvcChannelOptions{.priority = 0, .max_message_size = rdpei::max_pdu_size});
}

bool TouchInput::handle(const channels::DvcEvent& event)
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
                    rdpei_.start();
                } else if constexpr (std::is_same_v<T, dvc::ChannelOpenFailed>) {
                    close("the client has no touch or pen input");
                } else if constexpr (std::is_same_v<T, dvc::ChannelData>) {
                    if (auto received = rdpei_.receive(e.data); !received) {
                        // [MS-RDPEI] 3.1.5.1: a malformed message is ignored.
                        const auto level = ++malformed_ <= max_logged_malformed ? log::Level::warn : log::Level::debug;
                        log::write(level, component, received.error().message());
                    }
                } else if constexpr (std::is_same_v<T, dvc::ChannelClosed>) {
                    close("the client closed the input channel");
                }
                drain();
                return true;
            }
        },
        event);
}

std::optional<TouchEvent> TouchInput::poll_event()
{
    if (events_.empty()) {
        return std::nullopt;
    }
    TouchEvent event = std::move(events_.front());
    events_.pop_front();
    return event;
}

void TouchInput::suspend()
{
    if (!closed_) {
        rdpei_.suspend();
        drain();
    }
}

void TouchInput::resume()
{
    if (!closed_) {
        rdpei_.resume();
        drain();
    }
}

void TouchInput::release_all()
{
    rdpei_.release_all();
    drain();
}

void TouchInput::drain()
{
    if (!closed_) {
        for (const auto& message : rdpei_.take_output()) {
            channels_->send(channel_id_, message);
        }
    }
    while (auto event = rdpei_.poll_event()) {
        std::visit([this](auto& e) { events_.emplace_back(std::move(e)); }, *event);
    }
    if (rdpei_.violations() != violations_) {
        log::debug(component, "{} contacts broke the contact state machine and were canceled or ignored",
                   rdpei_.violations() - violations_);
        violations_ = rdpei_.violations();
    }
}

void TouchInput::close(std::string reason)
{
    closed_ = true;
    rdpei_.release_all();
    drain();
    events_.emplace_back(touch_event::Closed{std::move(reason)});
}

}  // namespace farland::server
