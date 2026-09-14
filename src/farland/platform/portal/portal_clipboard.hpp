// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/platform/clipboard.hpp>
#include <farland/platform/portal/portal_session.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace farland::platform::portal {

struct PortalClipboardOptions {
    /// Local data larger than this fails a read.
    std::size_t max_read_size = std::size_t{64} * 1024 * 1024;
    /// How long the desktop application may take to hand over its data,
    /// and to take ours.
    std::chrono::milliseconds read_timeout{15'000};
    std::chrono::milliseconds write_timeout{15'000};
};

/// The desktop clipboard through org.freedesktop.portal.Clipboard (version
/// 1) of a started PortalSession that was granted clipboard access
/// (PortalOptions::clipboard, PortalSession::clipboard_enabled()):
/// SetSelection offers the client's types, SelectionTransfer asks for them
/// and SelectionWrite/SelectionWriteDone hand them over; SelectionOwnerChanged
/// announces the desktop's types and SelectionRead reads them. Transfers go
/// through pipes that dispatch() serves without blocking.
///
/// Threading: as PortalSession. dispatch() also dispatches the session's bus.
/// The process must ignore SIGPIPE (a paste can be abandoned mid-write).
class PortalClipboard final : public Clipboard {
public:
    /// Subscribes to the Clipboard signals, taking over one that arrived
    /// while the session started.
    [[nodiscard]] static PortalResult<std::unique_ptr<PortalClipboard>> create(PortalSession& session,
                                                                               PortalClipboardOptions options = {});
    PortalClipboard(const PortalClipboard&) = delete;
    PortalClipboard& operator=(const PortalClipboard&) = delete;
    PortalClipboard(PortalClipboard&&) = delete;
    PortalClipboard& operator=(PortalClipboard&&) = delete;
    /// Refuses the pastes still being written.
    ~PortalClipboard() override;

    [[nodiscard]] std::optional<std::vector<std::string>> mime_types() const override { return mime_types_; }
    void set_selection(const std::vector<std::string>& mime_types) override;
    void write(std::uint32_t serial, std::optional<std::vector<std::byte>> data) override;
    [[nodiscard]] std::uint64_t read(const std::string& mime_type) override;
    [[nodiscard]] std::optional<ClipboardEvent> poll_event() override;
    [[nodiscard]] std::vector<PollFd> poll_fds() const override;
    void dispatch() override;

private:
    using Clock = std::chrono::steady_clock;

    struct Read {
        std::uint64_t id = 0;
        UniqueFd fd;
        std::vector<std::byte> data;
        Clock::time_point deadline;
    };
    struct Write {
        std::uint32_t serial = 0;
        UniqueFd fd;
        std::vector<std::byte> data;
        std::size_t offset = 0;
        Clock::time_point deadline;
    };

    PortalClipboard(PortalSession& session, PortalClipboardOptions options);
    static int on_owner_changed(sd_bus_message* message, void* userdata, sd_bus_error* error);
    static int on_transfer(sd_bus_message* message, void* userdata, sd_bus_error* error);
    void owner_changed(sd_bus_message* message);
    void write_done(std::uint32_t serial, bool success);
    /// Reads or writes what the pipe takes; true when the transfer is over.
    [[nodiscard]] bool pump(Read& read);
    [[nodiscard]] bool pump(Write& write);

    PortalSession& session_;
    PortalClipboardOptions options_;
    std::vector<detail::SlotPtr> watches_;
    std::optional<std::vector<std::string>> mime_types_;
    std::deque<ClipboardEvent> events_;
    std::vector<Read> reads_;
    std::vector<Write> writes_;
    std::uint64_t next_read_ = 1;
};

}  // namespace farland::platform::portal
