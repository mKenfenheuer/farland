// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/channels/disp.hpp>
#include <farland/channels/dvc_server.hpp>
#include <farland/server/display_layout.hpp>
#include <farland/server/dynamic_channels.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

/// The server end of the Display Control channel ([MS-RDPEDISP] 3.2): opens
/// "Microsoft::Windows::RDS::DisplayControl", sends the capabilities, and
/// turns the client's monitor layouts into validated DisplayLayouts.
///
/// While the user drags a window edge, a client sends a layout every few
/// milliseconds. Each one would reset the graphics and, with a virtual
/// monitor, resize a monitor on the shared desktop, so layouts are debounced:
/// one is handed out once no newer one came for `settle`, or `max_delay`
/// after the first of a burst.
namespace farland::server {

class DisplayControl {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::string_view channel_name = channels::disp::channel_name;

    struct Config {
        DisplayLimits limits;
        Clock::duration settle = std::chrono::milliseconds(300);
        Clock::duration max_delay = std::chrono::seconds(1);
    };

    /// Opens the channel on `channels` (whose capabilities must be ready).
    /// `current` is the layout in effect; a request for it again is dropped.
    /// `channels` must outlive this.
    DisplayControl(DynamicChannels& channels, DisplayLayout current, Config config);
    DisplayControl(DynamicChannels& channels, DisplayLayout current) : DisplayControl(channels, std::move(current), {})
    {
    }

    /// Handles `event` if it concerns the channel (received at `now`). False otherwise.
    bool handle(const channels::DvcEvent& event, Clock::time_point now);

    /// The layout the client asked for, once it settled; nullopt while none
    /// is due. It becomes the current layout.
    [[nodiscard]] std::optional<DisplayLayout> poll_layout(Clock::time_point now);
    /// When poll_layout() has something next; nullopt while nothing is pending.
    [[nodiscard]] std::optional<Clock::time_point> deadline() const noexcept;

    /// The layout changed another way (the connection was reactivated).
    void set_current(DisplayLayout layout) { current_ = std::move(layout); }
    [[nodiscard]] bool open() const noexcept { return open_; }

private:
    void on_message(std::span<const std::byte> message, Clock::time_point now);

    DynamicChannels* channels_;
    Config config_;
    std::uint32_t channel_id_ = 0;
    bool open_ = false;
    bool closed_ = false;
    DisplayLayout current_;
    std::optional<DisplayLayout> pending_;
    Clock::time_point first_request_;
    Clock::time_point last_request_;
};

}  // namespace farland::server
