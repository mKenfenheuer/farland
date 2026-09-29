// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "clipboard_relay.hpp"

#include <poll.h>

namespace farland::app {

ClipboardRelay::ClipboardRelay(platform::Clipboard& backend)
    : backend_(backend), proxy_(*this, backend.mime_types())
{
}

std::vector<platform::PollFd> ClipboardRelay::desktop_fds() const
{
    auto fds = backend_.poll_fds();
    fds.push_back(platform::PollFd{to_desktop_.fd(), POLLIN});
    return fds;
}

void ClipboardRelay::service_desktop()
{
    to_desktop_.drain();
    std::deque<Command> commands;
    {
        const std::scoped_lock lock(mutex_);
        commands.swap(commands_);
    }
    for (auto& command : commands) {
        std::visit(
            [this](auto& c) {
                using T = std::decay_t<decltype(c)>;
                if constexpr (std::is_same_v<T, SetSelection>) {
                    backend_.set_selection(c.mime_types);
                } else if constexpr (std::is_same_v<T, Write>) {
                    backend_.write(c.serial, std::move(c.data));
                } else if constexpr (std::is_same_v<T, Read>) {
                    reads_[backend_.read(c.mime_type)] = c.id;
                }
            },
            command);
    }
    backend_.dispatch();
    bool any = false;
    while (auto event = backend_.poll_event()) {
        if (auto* finished = std::get_if<platform::clipboard_event::ReadFinished>(&*event)) {
            const auto found = reads_.find(finished->id);
            if (found == reads_.end()) {
                continue;  // not one the proxy asked for
            }
            finished->id = found->second;
            reads_.erase(found);
        }
        const std::scoped_lock lock(mutex_);
        events_.push_back(std::move(*event));
        any = true;
    }
    if (any) {
        to_connection_.notify();
    }
}

void ClipboardRelay::command(Command command)
{
    {
        const std::scoped_lock lock(mutex_);
        commands_.push_back(std::move(command));
    }
    to_desktop_.notify();
}

void ClipboardRelay::Proxy::set_selection(const std::vector<std::string>& mime_types)
{
    mime_types_.reset();  // the session owns the clipboard now
    relay_.command(SetSelection{mime_types});
}

void ClipboardRelay::Proxy::write(std::uint32_t serial, std::optional<std::vector<std::byte>> data)
{
    relay_.command(Write{serial, std::move(data)});
}

std::uint64_t ClipboardRelay::Proxy::read(const std::string& mime_type)
{
    const std::uint64_t id = next_read_++;
    relay_.command(Read{id, mime_type});
    return id;
}

std::optional<platform::ClipboardEvent> ClipboardRelay::Proxy::poll_event()
{
    const std::scoped_lock lock(relay_.mutex_);
    if (relay_.events_.empty()) {
        return std::nullopt;
    }
    auto event = std::move(relay_.events_.front());
    relay_.events_.pop_front();
    if (const auto* owner = std::get_if<platform::clipboard_event::OwnerChanged>(&event)) {
        mime_types_ = owner->mime_types;
    }
    return event;
}

std::vector<platform::PollFd> ClipboardRelay::Proxy::poll_fds() const
{
    return {platform::PollFd{relay_.to_connection_.fd(), POLLIN}};
}

void ClipboardRelay::Proxy::dispatch()
{
    relay_.to_connection_.drain();
}

}  // namespace farland::app
