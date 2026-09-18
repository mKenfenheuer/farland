// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "logind.hpp"
#include "registry.hpp"

#include <catch2/catch_test_macros.hpp>

using farland::daemon::LocalSessionPolicy;
using farland::daemon::PolicySection;
using farland::daemon::SessionRegistry;
using Admission = SessionRegistry::Admission;
using Kind = SessionRegistry::Action::Kind;
using namespace std::chrono_literals;

namespace {

/// A fake clock: any time point, moved by hand.
struct FakeClock {
    SessionRegistry::Clock::time_point now{std::chrono::hours(1000)};
    void advance(SessionRegistry::Clock::duration d) { now += d; }
};

}  // namespace

TEST_CASE("Registry: one session per account, takeover replaces the connection")
{
    FakeClock clock;
    SessionRegistry registry(PolicySection{});
    auto decision = registry.admit("alice", false);
    CHECK(decision.admission == Admission::create);
    const auto alice = registry.create("alice", false, clock.now);
    CHECK(alice != 0);

    decision = registry.admit("alice", false);
    CHECK(decision.admission == Admission::existing);
    CHECK(decision.session == alice);
    CHECK(registry.admit("bob", false).admission == Admission::create);

    registry.set_running(alice);
    CHECK(registry.connect(alice, 7) == 0);
    CHECK(registry.connect(alice, 8) == 7);  // the second connection takes over
    CHECK(registry.find(alice)->connection == 8);
    // The first one's end arrives late and changes nothing.
    registry.disconnected(alice, 7, clock.now);
    CHECK(registry.find(alice)->connection == 8);
    registry.disconnected(alice, 8, clock.now);
    CHECK(registry.find(alice)->connection == 0);
    CHECK(registry.find_account("alice") != nullptr);
    registry.remove(alice);
    CHECK(registry.find_account("alice") == nullptr);
}

TEST_CASE("Registry: max_sessions and on_local_session")
{
    FakeClock clock;
    PolicySection policy;
    policy.max_sessions = 1;
    SessionRegistry registry(policy);
    registry.create("alice", false, clock.now);
    CHECK(registry.admit("alice", false).admission == Admission::existing);
    const auto refused = registry.admit("bob", false);
    CHECK(refused.admission == Admission::refuse_limit);
    CHECK(refused.error_info == 0x00000007);  // ERRINFO_SERVER_DENIED_CONNECTION
    CHECK_FALSE(refused.reason.empty());

    PolicySection refuse;
    refuse.on_local_session = LocalSessionPolicy::refuse;
    SessionRegistry refusing(refuse);
    CHECK(refusing.admit("carol", true).admission == Admission::refuse_local_session);

    PolicySection attach;
    attach.on_local_session = LocalSessionPolicy::attach;
    SessionRegistry attaching(attach);
    CHECK(attaching.admit("carol", true).admission == Admission::attach_local);
    // Once there is a session, the connection simply goes there.
    PolicySection replace;
    replace.on_local_session = LocalSessionPolicy::replace;
    const SessionRegistry replacing(replace);
    const auto decision = replacing.admit("carol", true);
    CHECK(decision.admission == Admission::replace_local);
    CHECK(decision.error_info == 0);  // not a refusal: the local session ends
    CHECK(decision.reason.contains("replace"));
    // Without a local session every policy starts a headless one.
    CHECK(replacing.admit("carol", false).admission == Admission::create);

    PolicySection separate;
    separate.on_local_session = LocalSessionPolicy::separate;
    const SessionRegistry separating(separate);
    const auto second = separating.admit("carol", true);
    CHECK(second.admission == Admission::separate_local);
    CHECK(second.error_info == 0);
    CHECK(second.reason.contains("separate"));

    const auto carol = attaching.create("carol", true, clock.now);
    CHECK(attaching.admit("carol", true).session == carol);
    CHECK(attaching.find(carol)->attached);
}

TEST_CASE("Registry: disconnected_timeout ends sessions nobody came back to")
{
    FakeClock clock;
    PolicySection policy;
    policy.disconnected_timeout = 60s;
    SessionRegistry registry(policy);
    const auto id = registry.create("alice", false, clock.now);
    clock.advance(120s);
    CHECK(registry.due(clock.now).empty());  // still starting: the start timeout applies instead
    registry.set_running(id);
    static_cast<void>(registry.connect(id, 1));
    clock.advance(120s);
    CHECK(registry.due(clock.now).empty());  // connected
    registry.disconnected(id, 1, clock.now);
    clock.advance(59s);
    CHECK(registry.due(clock.now).empty());
    // A reconnect in time resets the clock.
    static_cast<void>(registry.connect(id, 2));
    clock.advance(30s);
    registry.disconnected(id, 2, clock.now);
    clock.advance(59s);
    CHECK(registry.due(clock.now).empty());
    clock.advance(1s);
    const auto actions = registry.due(clock.now);
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].kind == Kind::terminate_disconnected);
    CHECK(actions[0].session == id);
    CHECK(registry.find(id)->state == SessionRegistry::State::ending);
    CHECK(registry.due(clock.now).empty());  // once
    // An ending session takes no connections; a new one starts instead.
    CHECK(registry.admit("alice", false).admission == Admission::create);
}

TEST_CASE("Registry: idle_timeout disconnects an idle connection once")
{
    FakeClock clock;
    PolicySection policy;
    policy.idle_timeout = 300s;
    SessionRegistry registry(policy);
    const auto id = registry.create("bob", false, clock.now);
    registry.set_running(id);
    static_cast<void>(registry.connect(id, 5));
    registry.update_idle(id, 5, 299);
    CHECK(registry.due(clock.now).empty());
    registry.update_idle(id, 4, 1000);  // stats of an older connection
    CHECK(registry.due(clock.now).empty());
    registry.update_idle(id, 5, 300);
    const auto actions = registry.due(clock.now);
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].kind == Kind::disconnect_idle);
    CHECK(actions[0].connection == 5);
    registry.update_idle(id, 5, 400);
    CHECK(registry.due(clock.now).empty());
    // Without disconnected_timeout the session itself stays.
    registry.disconnected(id, 5, clock.now);
    clock.advance(std::chrono::hours(24 * 30));
    CHECK(registry.due(clock.now).empty());
    // A new connection can be idle-disconnected again.
    static_cast<void>(registry.connect(id, 6));
    registry.update_idle(id, 6, 301);
    CHECK(registry.due(clock.now).size() == 1);
}

TEST_CASE("Local graphical sessions are those on a seat")
{
    farland::daemon::LoginSession session{"2", "seat0", "wayland", "user", "active", false};
    CHECK(farland::daemon::is_local_graphical(session));
    session.state = "online";
    CHECK(farland::daemon::is_local_graphical(session));
    auto headless = session;
    headless.seat.clear();  // GDM's remote displays and farland's own sessions
    CHECK_FALSE(farland::daemon::is_local_graphical(headless));
    auto greeter = session;
    greeter.session_class = "greeter";
    CHECK_FALSE(farland::daemon::is_local_graphical(greeter));
    auto tty = session;
    tty.type = "tty";
    CHECK_FALSE(farland::daemon::is_local_graphical(tty));
    auto closing = session;
    closing.state = "closing";
    CHECK_FALSE(farland::daemon::is_local_graphical(closing));
    auto remote = session;
    remote.remote = true;
    CHECK_FALSE(farland::daemon::is_local_graphical(remote));
}
