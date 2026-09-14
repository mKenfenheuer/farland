// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/server/loopback_clipboard.hpp>

#include <utility>

namespace farland::server {

namespace {
constexpr std::string_view log_component = "server.clipboard";
}  // namespace

std::vector<std::string> LoopbackClipboard::types() const
{
    std::vector<std::string> mimes;
    mimes.reserve(contents_.size());
    for (const auto& [mime, data] : contents_) {
        mimes.push_back(mime);
    }
    return mimes;
}

std::optional<std::vector<std::string>> LoopbackClipboard::mime_types() const
{
    if (!offered_) {
        return std::nullopt;
    }
    return types();
}

void LoopbackClipboard::set_selection(const std::vector<std::string>& mime_types)
{
    contents_.clear();
    pending_.clear();
    offered_ = false;
    for (const auto& mime : mime_types) {
        const auto serial = next_serial_++;
        pending_[serial] = mime;
        events_.emplace_back(platform::clipboard_event::TransferRequested{serial, mime});
    }
}

void LoopbackClipboard::write(std::uint32_t serial, std::optional<std::vector<std::byte>> data)
{
    const auto found = pending_.find(serial);
    if (found == pending_.end()) {
        return;
    }
    if (data) {
        log::info(log_component, "loopback: took {} bytes of {}", data->size(), found->second);
        contents_[found->second] = std::move(*data);
    }
    pending_.erase(found);
    if (pending_.empty() && !contents_.empty()) {
        // Everything is in: offer it back.
        offered_ = true;
        events_.emplace_back(platform::clipboard_event::OwnerChanged{types()});
    }
}

std::uint64_t LoopbackClipboard::read(const std::string& mime_type)
{
    const auto id = next_read_++;
    const auto found = contents_.find(mime_type);
    std::optional<std::vector<std::byte>> data;
    if (offered_ && found != contents_.end()) {
        data = found->second;
    }
    events_.emplace_back(platform::clipboard_event::ReadFinished{id, std::move(data)});
    return id;
}

std::optional<platform::ClipboardEvent> LoopbackClipboard::poll_event()
{
    if (events_.empty()) {
        return std::nullopt;
    }
    auto event = std::move(events_.front());
    events_.pop_front();
    return event;
}

}  // namespace farland::server
