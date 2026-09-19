// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The bit between farlandd's D-Bus thread and its loop: what
// `farlandctl sessions` sees, and what `farlandctl terminate` asks for.

#include "session_view.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using farland::daemon::SessionView;

namespace {

SessionView::Entry entry(std::uint32_t id, std::string account)
{
    SessionView::Entry e;
    e.id = id;
    e.account = std::move(account);
    e.state = "running";
    e.desktop = "plasma";
    return e;
}

}  // namespace

TEST_CASE("Session view: nothing is published before the loop runs", "[daemon][sessions]")
{
    SessionView view;
    CHECK(view.list().empty());
    // A session the snapshot does not know cannot be ended.
    CHECK_FALSE(view.request_terminate(1));
    CHECK(view.request_terminate_account("alice") == 0);
    CHECK(view.take_terminations().empty());
}

TEST_CASE("Session view: the loop publishes, the bus reads", "[daemon][sessions]")
{
    SessionView view;
    view.publish({entry(1, "alice"), entry(2, "bob")});
    const auto listed = view.list();
    REQUIRE(listed.size() == 2);
    CHECK(listed[0].id == 1);
    CHECK(listed[0].account == "alice");
    CHECK(listed[1].account == "bob");

    // A later snapshot replaces the last one entirely.
    view.publish({entry(2, "bob")});
    REQUIRE(view.list().size() == 1);
    CHECK(view.list()[0].id == 2);
}

TEST_CASE("Session view: ending a session is queued for the loop", "[daemon][sessions]")
{
    SessionView view;
    view.publish({entry(1, "alice"), entry(2, "bob"), entry(3, "alice")});

    CHECK(view.request_terminate(2));
    CHECK_FALSE(view.request_terminate(9));  // no such session
    auto asked = view.take_terminations();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0] == 2);
    // Each request is handed over once.
    CHECK(view.take_terminations().empty());

    // By account: every session of it.
    CHECK(view.request_terminate_account("alice") == 2);
    CHECK(view.request_terminate_account("nobody") == 0);
    asked = view.take_terminations();
    REQUIRE(asked.size() == 2);
    CHECK(asked[0] == 1);
    CHECK(asked[1] == 3);
}

TEST_CASE("Session view: asking twice before the loop looks does not queue twice", "[daemon][sessions]")
{
    SessionView view;
    view.publish({entry(1, "alice")});
    CHECK(view.request_terminate(1));
    CHECK(view.request_terminate(1));
    CHECK(view.request_terminate_account("alice") == 1);
    const auto asked = view.take_terminations();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0] == 1);
}
