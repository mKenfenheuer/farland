// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "session_view.hpp"

#include <algorithm>
#include <utility>

namespace farland::daemon {

void SessionView::publish(std::vector<Entry> sessions)
{
    const std::scoped_lock lock(mutex_);
    sessions_ = std::move(sessions);
}

std::vector<SessionView::Entry> SessionView::list() const
{
    const std::scoped_lock lock(mutex_);
    return sessions_;
}

bool SessionView::request_terminate(std::uint32_t id)
{
    const std::scoped_lock lock(mutex_);
    const bool known = std::ranges::any_of(sessions_, [id](const Entry& e) { return e.id == id; });
    if (known && std::ranges::find(terminate_, id) == terminate_.end()) {
        terminate_.push_back(id);
    }
    return known;
}

std::size_t SessionView::request_terminate_account(const std::string& account)
{
    const std::scoped_lock lock(mutex_);
    std::size_t asked = 0;
    for (const Entry& entry : sessions_) {
        if (entry.account != account) {
            continue;
        }
        ++asked;
        if (std::ranges::find(terminate_, entry.id) == terminate_.end()) {
            terminate_.push_back(entry.id);
        }
    }
    return asked;
}

std::vector<std::uint32_t> SessionView::take_terminations()
{
    const std::scoped_lock lock(mutex_);
    return std::exchange(terminate_, {});
}

}  // namespace farland::daemon
