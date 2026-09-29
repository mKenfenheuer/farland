// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "wake_pipe.hpp"

#include <farland/platform/clipboard.hpp>

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace farland::app {

/// The desktop's clipboard across two threads. A backend's clipboard runs over
/// the desktop's own connection (the portal's or Mutter's D-Bus session, the
/// compositor's Wayland connection), so it stays on the desktop thread, which
/// services it (desktop_fds(), service_desktop()); the cliprdr protocol runs
/// on the connection thread against proxy(), a stand-in that queues every call
/// for the desktop thread and hands back the backend's events.
class ClipboardRelay {
public:
    /// `backend` must outlive the relay; construct on the desktop thread.
    explicit ClipboardRelay(platform::Clipboard& backend);
    ClipboardRelay(const ClipboardRelay&) = delete;
    ClipboardRelay& operator=(const ClipboardRelay&) = delete;
    ClipboardRelay(ClipboardRelay&&) = delete;
    ClipboardRelay& operator=(ClipboardRelay&&) = delete;
    ~ClipboardRelay() = default;

    /// The stand-in, for the connection thread only.
    [[nodiscard]] platform::Clipboard& proxy() noexcept { return proxy_; }

    /// Desktop thread: what to poll; service_desktop() when one is ready, and
    /// at least every second (the backend's time-outs).
    [[nodiscard]] std::vector<platform::PollFd> desktop_fds() const;
    /// Desktop thread: carries out what the connection thread asked, services
    /// the backend and sends its events over.
    void service_desktop();

private:
    struct SetSelection {
        std::vector<std::string> mime_types;
    };
    struct Write {
        std::uint32_t serial = 0;
        std::optional<std::vector<std::byte>> data;
    };
    struct Read {
        std::uint64_t id = 0;  ///< the proxy's
        std::string mime_type;
    };
    using Command = std::variant<SetSelection, Write, Read>;

    class Proxy final : public platform::Clipboard {
    public:
        explicit Proxy(ClipboardRelay& relay, std::optional<std::vector<std::string>> mime_types)
            : relay_(relay), mime_types_(std::move(mime_types))
        {
        }
        [[nodiscard]] std::optional<std::vector<std::string>> mime_types() const override { return mime_types_; }
        void set_selection(const std::vector<std::string>& mime_types) override;
        void write(std::uint32_t serial, std::optional<std::vector<std::byte>> data) override;
        [[nodiscard]] std::uint64_t read(const std::string& mime_type) override;
        [[nodiscard]] std::optional<platform::ClipboardEvent> poll_event() override;
        [[nodiscard]] std::vector<platform::PollFd> poll_fds() const override;
        void dispatch() override;

    private:
        ClipboardRelay& relay_;
        /// As of the last OwnerChanged; nullopt once the session owns it.
        std::optional<std::vector<std::string>> mime_types_;
        std::uint64_t next_read_ = 1;
    };

    void command(Command command);

    platform::Clipboard& backend_;
    /// The desktop thread's: the backend's read IDs to the proxy's.
    std::map<std::uint64_t, std::uint64_t> reads_;

    mutable std::mutex mutex_;
    std::deque<Command> commands_;              ///< to the desktop thread
    std::deque<platform::ClipboardEvent> events_;  ///< to the connection thread
    WakePipe to_desktop_;
    WakePipe to_connection_;
    Proxy proxy_;  ///< last: it reads the backend's state through the rest
};

}  // namespace farland::app
