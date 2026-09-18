// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "seat_takeover.hpp"

#include <algorithm>
#include <utility>

namespace farland::daemon {

std::optional<std::uint64_t> SeatTakeoverGate::begin(const std::string& account)
{
    const std::lock_guard lock(mutex_);
    if (closed_ || std::ranges::find(held_, account) == held_.end()) {
        return std::nullopt;  // nobody holds it: the login is not held up
    }
    const auto cookie = next_cookie_++;
    pending_.emplace(cookie, Pending{account, false, false, true});
    return cookie;
}

bool SeatTakeoverGate::await(std::uint64_t cookie, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    const auto found = pending_.find(cookie);
    if (found == pending_.end()) {
        return true;
    }
    answered_.wait_for(lock, timeout, [&] {
        const auto it = pending_.find(cookie);
        return closed_ || it == pending_.end() || it->second.done;
    });
    const auto it = pending_.find(cookie);
    const bool allowed = it == pending_.end() || !it->second.done || it->second.allowed;
    if (it != pending_.end()) {
        pending_.erase(it);
    }
    return allowed;
}

void SeatTakeoverGate::set_held_accounts(std::vector<std::string> accounts)
{
    const std::lock_guard lock(mutex_);
    held_ = std::move(accounts);
}

std::vector<SeatTakeoverGate::Waiting> SeatTakeoverGate::take_new()
{
    const std::lock_guard lock(mutex_);
    std::vector<Waiting> fresh;
    for (auto& [cookie, entry] : pending_) {
        if (!entry.taken && !entry.done) {
            entry.taken = true;
            fresh.push_back(Waiting{cookie, entry.account});
        }
    }
    return fresh;
}

void SeatTakeoverGate::resolve(std::uint64_t cookie, bool allowed)
{
    {
        const std::lock_guard lock(mutex_);
        const auto found = pending_.find(cookie);
        if (found == pending_.end()) {
            return;
        }
        found->second.done = true;
        found->second.allowed = allowed;
    }
    answered_.notify_all();
}

void SeatTakeoverGate::release_all()
{
    {
        const std::lock_guard lock(mutex_);
        closed_ = true;
        for (auto& [cookie, entry] : pending_) {
            entry.done = true;
            entry.allowed = true;
        }
        held_.clear();
    }
    answered_.notify_all();
}

std::size_t SeatTakeoverGate::waiting() const
{
    const std::lock_guard lock(mutex_);
    return pending_.size();
}

}  // namespace farland::daemon
