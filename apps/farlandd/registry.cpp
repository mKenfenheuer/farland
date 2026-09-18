// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "registry.hpp"

#include <algorithm>
#include <format>

namespace farland::daemon {

namespace {

// [MS-RDPBCGR] 2.2.5.1.1.
constexpr std::uint32_t errinfo_server_denied_connection = 0x00000007;

}  // namespace

SessionRegistry::Decision SessionRegistry::admit(std::string_view account, bool local_session) const
{
    if (const auto* session = find_account(account); session != nullptr && session->state != State::ending) {
        return Decision{Admission::existing, session->id, 0, {}};
    }
    if (local_session && policy_.on_local_session == LocalSessionPolicy::refuse) {
        return Decision{Admission::refuse_local_session, 0, errinfo_server_denied_connection,
                        std::format("{} is logged in at a local seat and on_local_session = \"refuse\"", account)};
    }
    const auto live = std::ranges::count_if(sessions_, [](const Session& s) { return s.state != State::ending; });
    if (policy_.max_sessions != 0 && static_cast<std::size_t>(live) >= policy_.max_sessions) {
        return Decision{Admission::refuse_limit, 0, errinfo_server_denied_connection,
                        std::format("max_sessions = {} reached", policy_.max_sessions)};
    }
    if (local_session) {
        if (policy_.on_local_session == LocalSessionPolicy::separate) {
            return Decision{Admission::separate_local, 0, 0,
                            std::format("{} is logged in at a local seat and on_local_session = \"separate\": that "
                                        "session keeps running, and this connection gets a desktop of its own",
                                        account)};
        }
        if (policy_.on_local_session == LocalSessionPolicy::replace) {
            return Decision{Admission::replace_local, 0, 0,
                            std::format("{} is logged in at a local seat and on_local_session = \"replace\": that "
                                        "session ends, with the applications running in it",
                                        account)};
        }
        return Decision{Admission::attach_local, 0, 0, {}};
    }
    return Decision{Admission::create, 0, 0, {}};
}

std::uint32_t SessionRegistry::create(std::string account, bool attached, Clock::time_point now)
{
    Session session;
    session.id = next_id_++;
    session.account = std::move(account);
    session.attached = attached;
    session.disconnected_since = now;
    sessions_.push_back(std::move(session));
    return sessions_.back().id;
}

void SessionRegistry::set_running(std::uint32_t session)
{
    if (auto* s = get(session); s != nullptr && s->state == State::starting) {
        s->state = State::running;
    }
}

std::uint64_t SessionRegistry::connect(std::uint32_t session, std::uint64_t connection)
{
    auto* s = get(session);
    if (s == nullptr) {
        return 0;
    }
    const std::uint64_t replaced = s->connection;
    s->connection = connection;
    s->disconnected_since.reset();
    s->idle_seconds = 0;
    s->idle_disconnect_sent = false;
    return replaced;
}

void SessionRegistry::disconnected(std::uint32_t session, std::uint64_t connection, Clock::time_point now)
{
    auto* s = get(session);
    if (s == nullptr || s->connection != connection || connection == 0) {
        return;
    }
    s->connection = 0;
    s->disconnected_since = now;
    s->idle_seconds = 0;
}

void SessionRegistry::update_idle(std::uint32_t session, std::uint64_t connection, std::uint32_t idle_seconds)
{
    if (auto* s = get(session); s != nullptr && s->connection == connection && connection != 0) {
        s->idle_seconds = idle_seconds;
    }
}

void SessionRegistry::set_ending(std::uint32_t session)
{
    if (auto* s = get(session)) {
        s->state = State::ending;
    }
}

void SessionRegistry::remove(std::uint32_t session)
{
    std::erase_if(sessions_, [session](const Session& s) { return s.id == session; });
}

std::vector<SessionRegistry::Action> SessionRegistry::due(Clock::time_point now)
{
    std::vector<Action> actions;
    for (auto& s : sessions_) {
        if (s.state != State::running) {
            continue;
        }
        if (s.connection == 0 && policy_.disconnected_timeout.count() > 0 && s.disconnected_since &&
            now - *s.disconnected_since >= policy_.disconnected_timeout) {
            s.state = State::ending;
            actions.push_back(Action{Action::Kind::terminate_disconnected, s.id, 0});
            continue;
        }
        if (s.connection != 0 && policy_.idle_timeout.count() > 0 && !s.idle_disconnect_sent &&
            std::chrono::seconds(s.idle_seconds) >= policy_.idle_timeout) {
            s.idle_disconnect_sent = true;
            actions.push_back(Action{Action::Kind::disconnect_idle, s.id, s.connection});
        }
    }
    return actions;
}

const SessionRegistry::Session* SessionRegistry::find(std::uint32_t session) const
{
    const auto found = std::ranges::find(sessions_, session, &Session::id);
    return found != sessions_.end() ? &*found : nullptr;
}

const SessionRegistry::Session* SessionRegistry::find_account(std::string_view account) const
{
    const auto found = std::ranges::find(sessions_, account, &Session::account);
    return found != sessions_.end() ? &*found : nullptr;
}

SessionRegistry::Session* SessionRegistry::get(std::uint32_t session)
{
    const auto found = std::ranges::find(sessions_, session, &Session::id);
    return found != sessions_.end() ? &*found : nullptr;
}

}  // namespace farland::daemon
