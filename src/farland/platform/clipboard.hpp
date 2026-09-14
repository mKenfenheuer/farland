// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

/// The desktop's clipboard as a platform backend offers it (docs/PLAN.md
/// §3.3): contents are MIME types, data is delay-rendered both ways. The
/// portal backend implements it with org.freedesktop.portal.Clipboard.
///
/// Threading: like the other backend interfaces, all calls come from the
/// session thread; the session polls poll_fds() and calls dispatch().
namespace farland::platform {

namespace clipboard_event {
/// Someone on the desktop copied: the clipboard holds these types now. Not
/// sent for the session's own set_selection().
struct OwnerChanged {
    std::vector<std::string> mime_types;
};
/// A desktop application pastes what set_selection() offered. Answer with
/// write(serial, ...).
struct TransferRequested {
    std::uint32_t serial = 0;
    std::string mime_type;
};
/// A read() finished. nullopt when the owner failed it, it took too long or
/// it grew beyond the size limit.
struct ReadFinished {
    std::uint64_t id = 0;
    std::optional<std::vector<std::byte>> data;
};
}  // namespace clipboard_event

using ClipboardEvent =
    std::variant<clipboard_event::OwnerChanged, clipboard_event::TransferRequested, clipboard_event::ReadFinished>;

/// A descriptor to poll: `events` is POLLIN or POLLOUT.
struct PollFd {
    int fd = -1;
    short events = 0;
};

class Clipboard {
public:
    Clipboard() = default;
    Clipboard(const Clipboard&) = delete;
    Clipboard& operator=(const Clipboard&) = delete;
    Clipboard(Clipboard&&) = delete;
    Clipboard& operator=(Clipboard&&) = delete;
    virtual ~Clipboard() = default;

    /// What someone else on the desktop has on the clipboard, as of the last
    /// OwnerChanged; nullopt when unknown or when the session owns it.
    [[nodiscard]] virtual std::optional<std::vector<std::string>> mime_types() const = 0;
    /// Puts these types on the desktop's clipboard (the client copied).
    virtual void set_selection(const std::vector<std::string>& mime_types) = 0;
    /// Answers TransferRequested; nullopt refuses. Large data goes out in
    /// the background of dispatch().
    virtual void write(std::uint32_t serial, std::optional<std::vector<std::byte>> data) = 0;
    /// Starts reading the desktop's clipboard as `mime_type`; ReadFinished
    /// with the returned ID carries the result.
    [[nodiscard]] virtual std::uint64_t read(const std::string& mime_type) = 0;
    [[nodiscard]] virtual std::optional<ClipboardEvent> poll_event() = 0;

    /// Descriptors with transfers in progress; dispatch() when one is ready,
    /// and at least every second (for time-outs).
    [[nodiscard]] virtual std::vector<PollFd> poll_fds() const = 0;
    virtual void dispatch() = 0;
};

}  // namespace farland::platform
