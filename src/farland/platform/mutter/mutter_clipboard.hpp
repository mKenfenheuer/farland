// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/platform/clipboard.hpp>
#include <farland/platform/mutter/mutter_session.hpp>
#include <farland/platform/portal/clipboard_pipes.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace farland::platform::mutter {

/// The desktop clipboard through Mutter's RemoteDesktop session, as
/// gnome-remote-desktop uses it: EnableClipboard, then SetSelection offers the
/// client's types, SelectionTransfer asks for them and SelectionWrite /
/// SelectionWriteDone hand them over; SelectionOwnerChanged announces the
/// desktop's types and SelectionRead reads them, one read at a time (Mutter
/// refuses parallel ones, so further reads wait their turn). Transfers go
/// through pipes that dispatch() serves without blocking.
///
/// Threading: as MutterSession. dispatch() also dispatches the session's bus.
/// The process must ignore SIGPIPE (a paste can be abandoned mid-write).
class MutterClipboard final : public Clipboard {
public:
    /// Subscribes to the clipboard signals and enables the clipboard.
    [[nodiscard]] static MutterResult<std::unique_ptr<MutterClipboard>>
    create(MutterSession& session, portal::ClipboardTransferOptions options = {});
    MutterClipboard(const MutterClipboard&) = delete;
    MutterClipboard& operator=(const MutterClipboard&) = delete;
    MutterClipboard(MutterClipboard&&) = delete;
    MutterClipboard& operator=(MutterClipboard&&) = delete;
    /// Refuses the pastes still being written and disables the clipboard.
    ~MutterClipboard() override;

    [[nodiscard]] std::optional<std::vector<std::string>> mime_types() const override { return mime_types_; }
    void set_selection(const std::vector<std::string>& mime_types) override;
    void write(std::uint32_t serial, std::optional<std::vector<std::byte>> data) override;
    [[nodiscard]] std::uint64_t read(const std::string& mime_type) override;
    [[nodiscard]] std::optional<ClipboardEvent> poll_event() override;
    [[nodiscard]] std::vector<PollFd> poll_fds() const override { return pipes_.poll_fds(); }
    void dispatch() override;

private:
    MutterClipboard(MutterSession& session, portal::ClipboardTransferOptions options);
    static int on_owner_changed(sd_bus_message* message, void* userdata, sd_bus_error* error);
    static int on_transfer(sd_bus_message* message, void* userdata, sd_bus_error* error);
    void owner_changed(sd_bus_message* message);
    void write_done(std::uint32_t serial, bool success);
    /// Starts the next queued read while none is in flight.
    void start_reads();

    MutterSession& session_;
    std::vector<portal::detail::SlotPtr> watches_;
    std::optional<std::vector<std::string>> mime_types_;
    std::deque<ClipboardEvent> events_;
    portal::ClipboardPipes pipes_;
    /// Reads waiting for the one in flight: their IDs and MIME types.
    std::deque<std::pair<std::uint64_t, std::string>> queued_reads_;
    std::uint64_t next_read_ = 1;
    bool enabled_ = false;
};

}  // namespace farland::platform::mutter
