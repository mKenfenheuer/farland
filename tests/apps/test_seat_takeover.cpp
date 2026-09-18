// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// [policy] seat_takeover: the gate between the PAM module asking on the
// D-Bus thread and the daemon's loop answering. Nobody may be kept waiting
// at a login screen, so every case without a clear answer lets the login
// through.

#include "seat_takeover.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

using farland::daemon::SeatTakeoverGate;
using namespace std::chrono_literals;

TEST_CASE("a login is held up only for an account whose session a client holds")
{
    SeatTakeoverGate gate;
    // Nothing is held yet, so nobody is asked about anything.
    CHECK(!gate.begin("max").has_value());
    CHECK(gate.take_new().empty());

    gate.set_held_accounts({"max", "alice"});
    CHECK(!gate.begin("bob").has_value());  // another account entirely
    const auto cookie = gate.begin("max");
    REQUIRE(cookie.has_value());
    CHECK(gate.waiting() == 1);

    const auto fresh = gate.take_new();
    REQUIRE(fresh.size() == 1);
    CHECK(fresh.front().cookie == *cookie);
    CHECK(fresh.front().account == "max");
    // The loop is told once; asking again does not ask twice.
    CHECK(gate.take_new().empty());
}

TEST_CASE("the answer reaches the login that is waiting for it")
{
    SeatTakeoverGate gate;
    gate.set_held_accounts({"max"});

    const auto allowed = gate.begin("max");
    REQUIRE(allowed.has_value());
    gate.resolve(*allowed, true);
    CHECK(gate.await(*allowed, 1s));
    CHECK(gate.waiting() == 0);  // the login took its answer with it

    const auto refused = gate.begin("max");
    REQUIRE(refused.has_value());
    gate.resolve(*refused, false);
    CHECK(!gate.await(*refused, 1s));
}

TEST_CASE("a login waits for the answer and is let through when it never comes")
{
    SeatTakeoverGate gate;
    gate.set_held_accounts({"max"});
    const auto cookie = gate.begin("max");
    REQUIRE(cookie.has_value());

    // The answer arrives while the login waits.
    std::thread answering([&] {
        std::this_thread::sleep_for(50ms);
        gate.resolve(*cookie, false);
    });
    const auto started = std::chrono::steady_clock::now();
    CHECK(!gate.await(*cookie, 5s));
    CHECK(std::chrono::steady_clock::now() - started < 5s);
    answering.join();

    // Nobody answers at all: the login goes ahead once its time is up.
    const auto lonely = gate.begin("max");
    REQUIRE(lonely.has_value());
    CHECK(gate.await(*lonely, 20ms));
}

TEST_CASE("nothing keeps a login waiting for farland")
{
    SeatTakeoverGate gate;
    // A cookie nobody knows (farlandd restarted under the login).
    CHECK(gate.await(1234, 1s));

    gate.set_held_accounts({"max"});
    const auto cookie = gate.begin("max");
    REQUIRE(cookie.has_value());
    // farlandd is going away with a login waiting on it.
    gate.release_all();
    CHECK(gate.await(*cookie, 1s));
    // And nothing new is held up afterwards.
    CHECK(!gate.begin("max").has_value());
}
