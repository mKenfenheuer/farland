// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "consent.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace farland::daemon {

namespace {

namespace broker = server::broker;

/// Longest string the broker protocol takes for a peer or a client name.
constexpr std::size_t max_detail = 255;

std::string shorten(const std::string& text)
{
    return text.substr(0, max_detail);
}

}  // namespace

ConsentBroker::Admission ConsentBroker::admit(bool held) const noexcept
{
    if (!held) {
        return Admission::hand_over;
    }
    switch (policy_.takeover) {
    case TakeoverPolicy::ask:
        return Admission::ask;
    case TakeoverPolicy::always:
        return Admission::hand_over;
    case TakeoverPolicy::never:
        return Admission::refuse;
    }
    return Admission::ask;
}

std::string ConsentBroker::refusal(std::string_view account) const
{
    return std::format("the session of {} is in use and takeover = \"{}\"", account, to_string(policy_.takeover));
}

broker::ConsentRequest ConsentBroker::ask(std::uint32_t session, std::uint64_t connection, const Party& who,
                                          Clock::time_point now)
{
    forget(connection);
    prompts_.push_back(Prompt{session, connection, now + policy_.takeover_timeout + answer_grace});
    broker::ConsentRequest request;
    request.connection_id = connection;
    request.user = shorten(who.user);
    request.peer = shorten(who.peer);
    request.client_name = shorten(who.client_name);
    request.timeout_seconds = static_cast<std::uint32_t>(policy_.takeover_timeout.count());
    request.allow_on_timeout = policy_.takeover_on_timeout == TakeoverDefault::allow;
    return request;
}

std::optional<ConsentBroker::Resolved> ConsentBroker::answered(std::uint64_t connection, broker::ConsentAnswer answer)
{
    const auto found = std::ranges::find(prompts_, connection, &Prompt::connection);
    if (found == prompts_.end()) {
        return std::nullopt;
    }
    const Prompt prompt = *found;
    prompts_.erase(found);
    switch (answer) {
    case broker::ConsentAnswer::allowed:
        return Resolved{prompt.session, prompt.connection, true, false, "the user handed the session over"};
    case broker::ConsentAnswer::denied:
        return Resolved{prompt.session, prompt.connection, false, false, "the user kept the session"};
    case broker::ConsentAnswer::timed_out:
        return on_default(prompt, "nobody answered", false);
    case broker::ConsentAnswer::unavailable:
        return on_default(prompt, "the session has no notification service to ask in", false);
    }
    return on_default(prompt, "the agent gave no answer", false);
}

std::vector<ConsentBroker::Resolved> ConsentBroker::released(std::uint32_t session)
{
    std::vector<Resolved> resolved;
    for (const auto& prompt : take(session)) {
        resolved.push_back(
            Resolved{prompt.session, prompt.connection, true, true, "nobody holds the session any more"});
    }
    return resolved;
}

std::vector<ConsentBroker::Resolved> ConsentBroker::ended(std::uint32_t session)
{
    std::vector<Resolved> resolved;
    for (const auto& prompt : take(session)) {
        resolved.push_back(
            Resolved{prompt.session, prompt.connection, false, false, "the session ended while its user was asked"});
    }
    return resolved;
}

std::vector<ConsentBroker::Resolved> ConsentBroker::due(Clock::time_point now)
{
    std::vector<Resolved> resolved;
    std::vector<Prompt> kept;
    for (const auto& prompt : prompts_) {
        if (now >= prompt.deadline) {
            resolved.push_back(on_default(prompt, "the agent did not answer", true));
        } else {
            kept.push_back(prompt);
        }
    }
    prompts_ = std::move(kept);
    return resolved;
}

void ConsentBroker::forget(std::uint64_t connection)
{
    std::erase_if(prompts_, [connection](const Prompt& p) { return p.connection == connection; });
}

bool ConsentBroker::waiting(std::uint64_t connection) const
{
    return std::ranges::find(prompts_, connection, &Prompt::connection) != prompts_.end();
}

ConsentBroker::Resolved ConsentBroker::on_default(const Prompt& prompt, std::string_view why, bool withdraw) const
{
    const bool allow = policy_.takeover_on_timeout == TakeoverDefault::allow;
    return Resolved{prompt.session, prompt.connection, allow, withdraw,
                    std::format("{} within {} s; takeover_on_timeout = \"{}\"", why, policy_.takeover_timeout.count(),
                                to_string(policy_.takeover_on_timeout))};
}

std::vector<ConsentBroker::Prompt> ConsentBroker::take(std::uint32_t session)
{
    std::vector<Prompt> taken;
    std::vector<Prompt> kept;
    for (const auto& prompt : prompts_) {
        if (prompt.session == session) {
            taken.push_back(prompt);
        } else {
            kept.push_back(prompt);
        }
    }
    prompts_ = std::move(kept);
    return taken;
}

}  // namespace farland::daemon
