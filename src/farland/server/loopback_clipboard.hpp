// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/platform/clipboard.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace farland::server {

/// The test backend's clipboard: whatever the client copies is pasted into
/// memory at once, every type of it, and then offered back as the desktop's
/// clipboard. A client that pastes afterwards gets its data after a round
/// trip through the server, both directions and every format conversion
/// included.
class LoopbackClipboard final : public platform::Clipboard {
public:
    [[nodiscard]] std::optional<std::vector<std::string>> mime_types() const override;
    void set_selection(const std::vector<std::string>& mime_types) override;
    void write(std::uint32_t serial, std::optional<std::vector<std::byte>> data) override;
    [[nodiscard]] std::uint64_t read(const std::string& mime_type) override;
    [[nodiscard]] std::optional<platform::ClipboardEvent> poll_event() override;
    [[nodiscard]] std::vector<platform::PollFd> poll_fds() const override { return {}; }
    void dispatch() override {}

private:
    [[nodiscard]] std::vector<std::string> types() const;

    std::map<std::string, std::vector<std::byte>> contents_;
    std::map<std::uint32_t, std::string> pending_;  ///< serial -> MIME type being pasted
    std::uint32_t next_serial_ = 1;
    std::uint64_t next_read_ = 1;
    bool offered_ = false;
    std::deque<platform::ClipboardEvent> events_;
};

}  // namespace farland::server
