// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/channels/rdpei_server.hpp>
#include <farland/server/dynamic_channels.hpp>

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

/// Touch and pen input of one session ([MS-RDPEI]) over a dynamic virtual
/// channel. It opens the channel, runs RdpeiServer on it, and hands out the
/// validated contact frames for the session to inject.
namespace farland::server {

namespace touch_event {
/// The channel is gone: the client refused it (it has no touch or pen
/// digitizer, [MS-RDPEI] 3.3.3) or closed it. Contacts it had active were
/// released in a Frame before this.
struct Closed {
    std::string reason;
};
}  // namespace touch_event

using TouchEvent = std::variant<channels::rdpei::event::Ready, channels::rdpei::event::Frame, touch_event::Closed>;

class TouchInput {
public:
    static constexpr std::string_view channel_name = channels::rdpei::channel_name;

    /// Opens the input channel on `channels` (whose capabilities must be
    /// ready). `channels` must outlive this.
    explicit TouchInput(DynamicChannels& channels, channels::rdpei::RdpeiServerConfig config = {});

    /// Handles `event` if it concerns the input channel. False otherwise.
    bool handle(const channels::DvcEvent& event);
    [[nodiscard]] std::optional<TouchEvent> poll_event();

    /// Asks the client to stop or restart sending input ([MS-RDPEI]
    /// 3.2.5.4); suspending releases every contact.
    void suspend();
    void resume();
    /// Releases every contact (a Frame follows), for the end of the session.
    void release_all();

    [[nodiscard]] bool ready() const noexcept { return rdpei_.ready() && !closed_; }
    [[nodiscard]] bool closed() const noexcept { return closed_; }

private:
    void drain();
    void close(std::string reason);

    DynamicChannels* channels_;  ///< never null
    std::uint32_t channel_id_ = 0;
    channels::rdpei::RdpeiServer rdpei_;
    std::deque<TouchEvent> events_;
    std::uint64_t violations_ = 0;  ///< as last logged
    std::uint64_t malformed_ = 0;
    bool closed_ = false;
};

}  // namespace farland::server
