// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/server/display_control.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <variant>

namespace farland::server {

namespace {

namespace disp = channels::disp;
namespace dvc = channels::dvc_event;
constexpr std::string_view log_component = "server.display";

std::string describe(const DisplayLayout& layout)
{
    std::string text = std::format("{}x{}", layout.width(), layout.height());
    if (layout.monitors().size() > 1) {
        text += std::format(" in {} monitors:", layout.monitors().size());
        for (const auto& m : layout.monitors()) {
            text += std::format(" {}x{}+{}+{}{}", m.rect.width, m.rect.height, m.rect.x, m.rect.y,
                                m.primary ? " (primary)" : "");
        }
    }
    const auto& first = layout.monitors().front();
    if (first.desktop_scale_factor != 0) {
        text += std::format(", scale {}%", first.desktop_scale_factor);
    }
    return text;
}

}  // namespace

DisplayControl::DisplayControl(DynamicChannels& channels, DisplayLayout current, Config config)
    : channels_(&channels), config_(config), current_(std::move(current))
{
    channel_id_ = channels_->open(std::string(channel_name));
}

bool DisplayControl::handle(const channels::DvcEvent& event, Clock::time_point now)
{
    return std::visit(
        [this, now](const auto& e) -> bool {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, dvc::CapabilitiesReady>) {
                return false;
            } else {
                if (e.id != channel_id_ || closed_) {
                    return false;
                }
                if constexpr (std::is_same_v<T, dvc::ChannelOpened>) {
                    // [MS-RDPEDISP] 3.2.5.1: the server sends its capabilities
                    // first; the client answers with layouts.
                    open_ = true;
                    channels_->send(channel_id_, disp::encode(config_.limits.caps()));
                } else if constexpr (std::is_same_v<T, dvc::ChannelOpenFailed>) {
                    log::info(log_component, "the client has no display control; the desktop keeps its size");
                    closed_ = true;
                } else if constexpr (std::is_same_v<T, dvc::ChannelData>) {
                    on_message(e.data, now);
                } else if constexpr (std::is_same_v<T, dvc::ChannelClosed>) {
                    open_ = false;
                    closed_ = true;
                    pending_.reset();
                }
                return true;
            }
        },
        event);
}

void DisplayControl::on_message(std::span<const std::byte> message, Clock::time_point now)
{
    const auto pdu = disp::decode(message, config_.limits.max_monitors);
    if (!pdu) {
        // A broken channel costs the client resizing, not the connection.
        log::warn(log_component, "display control: {}; closing the channel", pdu.error().message());
        channels_->close(channel_id_);
        open_ = false;
        closed_ = true;
        pending_.reset();
        return;
    }
    const auto* request = std::get_if<disp::MonitorLayoutPdu>(&*pdu);
    if (request == nullptr) {
        log::debug(log_component, "display control: ignoring a capabilities PDU from the client");
        return;
    }
    auto layout = DisplayLayout::from_disp(*request, config_.limits);
    if (!layout) {
        // [MS-RDPEDISP] 3.2.5.2: an invalid layout is ignored.
        log::warn(log_component, "ignoring the client's monitor layout: {}", layout.error().what);
        return;
    }
    log::debug(log_component, "the client asks for {}", describe(*layout));
    if (pending_ ? *layout == *pending_ : *layout == current_) {
        return;
    }
    if (!pending_) {
        first_request_ = now;
    }
    pending_ = std::move(*layout);
    last_request_ = now;
    if (*pending_ == current_) {
        pending_.reset();  // back to where it was before the burst
    }
}

std::optional<DisplayControl::Clock::time_point> DisplayControl::deadline() const noexcept
{
    if (!pending_) {
        return std::nullopt;
    }
    return std::min(last_request_ + config_.settle, first_request_ + config_.max_delay);
}

std::optional<DisplayLayout> DisplayControl::poll_layout(Clock::time_point now)
{
    const auto due = deadline();
    if (!pending_ || !due || now < *due) {
        return std::nullopt;
    }
    current_ = std::move(*pending_);
    pending_.reset();
    log::info(log_component, "monitor layout {}", describe(current_));
    return current_;
}

}  // namespace farland::server
