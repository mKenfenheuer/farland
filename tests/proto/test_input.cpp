// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/hexdump.hpp>
#include <farland/proto/input.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace proto = farland::proto;
using farland::Errc;
using farland::Reader;
using farland::to_hex;
using farland::Writer;
using farland::test::hex;

namespace {

std::vector<proto::InputEvent> sample_events()
{
    return {
        proto::KeyboardEvent{0, 0x1e},
        proto::KeyboardEvent{proto::kbd_flags::release | proto::kbd_flags::extended, 0x48},
        proto::UnicodeKeyboardEvent{0, 0x20ac},
        proto::UnicodeKeyboardEvent{proto::kbd_flags::release, 0x20ac},
        proto::MouseEvent{proto::ptr_flags::down | proto::ptr_flags::button1, 100, 200},
        proto::ExtendedMouseEvent{proto::ptrx_flags::down | proto::ptrx_flags::button2, 5, 6},
        proto::RelativeMouseEvent{proto::ptr_flags::move, -12, 34},
        proto::SyncEvent{proto::sync_flags::num_lock | proto::sync_flags::caps_lock},
    };
}

bool same(const proto::InputEvent& a, const proto::InputEvent& b)
{
    if (a.index() != b.index()) {
        return false;
    }
    return std::visit(
        [&b](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            const auto& y = std::get<T>(b);
            if constexpr (std::is_same_v<T, proto::SyncEvent>) {
                return x.toggle_flags == y.toggle_flags;
            } else if constexpr (std::is_same_v<T, proto::QoeTimestampEvent>) {
                return x.timestamp == y.timestamp;
            } else if constexpr (std::is_same_v<T, proto::RelativeMouseEvent>) {
                return x.flags == y.flags && x.dx == y.dx && x.dy == y.dy;
            } else if constexpr (std::is_same_v<T, proto::KeyboardEvent> ||
                                 std::is_same_v<T, proto::UnicodeKeyboardEvent>) {
                return x.flags == y.flags && x.code == y.code;
            } else {
                return x.flags == y.flags && x.x == y.x && x.y == y.y;
            }
        },
        a);
}

void check_same(const std::vector<proto::InputEvent>& expected, const std::vector<proto::InputEvent>& actual)
{
    REQUIRE(expected.size() == actual.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        INFO("event " << i);
        CHECK(same(expected[i], actual[i]));
    }
}

}  // namespace

TEST_CASE("Slow-path input round-trips ([MS-RDPBCGR] 2.2.8.1.1.3)")
{
    const auto events = sample_events();
    Writer w;
    proto::encode_slow_path_input(w, events);
    Reader r(w.view());
    check_same(events, proto::decode_slow_path_input(r).value());
    CHECK(r.empty());
}

TEST_CASE("Fast-path input round-trips, with the count in the header or in its own octet")
{
    auto events = sample_events();
    events.emplace_back(proto::QoeTimestampEvent{123456});
    Writer w;
    proto::encode_fastpath_input(w, events);
    CHECK(std::to_integer<unsigned>(w.view()[0]) == (events.size() << 2U));
    Reader r(w.view());
    check_same(events, proto::decode_fastpath_input(r).value());

    std::vector<proto::InputEvent> many(20, proto::KeyboardEvent{0, 0x10});
    Writer big;
    proto::encode_fastpath_input(big, many);
    CHECK(std::to_integer<unsigned>(big.view()[0]) == 0);  // numEvents in its own octet
    Reader rb(big.view());
    CHECK(proto::decode_fastpath_input(rb).value().size() == 20);
}

TEST_CASE("Fast-path keyboard flags map to the slow-path values")
{
    // eventHeader 0x06: scancode, EXTENDED | EXTENDED1... here RELEASE | EXTENDED = 0x03.
    const auto bytes = hex("04 04 03 48");
    Reader r(bytes);
    const auto events = proto::decode_fastpath_input(r).value();
    const auto key = std::get<proto::KeyboardEvent>(events.at(0));
    CHECK(key.flags == (proto::kbd_flags::release | proto::kbd_flags::extended));
    CHECK(key.code == 0x48);
}

TEST_CASE("Malformed fast-path input is rejected")
{
    const auto check = [](std::string_view text, Errc expected) {
        const auto bytes = hex(text);
        Reader r(bytes);
        const auto events = proto::decode_fastpath_input(r);
        REQUIRE_FALSE(events.has_value());
        CHECK(events.error().code == expected);
    };
    check("84 04 00 1e", Errc::invalid_value);        // FASTPATH_INPUT_ENCRYPTED
    check("04 05 00 1e", Errc::invalid_length);       // length disagrees with the PDU
    check("04 04 e0 00", Errc::unsupported);          // event code 7
    check("08 04 00 1e", Errc::truncated);            // two events announced, one present
    check("04 06 00 1e 00 1f", Errc::trailing_data);  // one event announced, two present
}

TEST_CASE("Malformed slow-path input is rejected")
{
    const auto check = [](std::string_view text, Errc expected) {
        const auto bytes = hex(text);
        Reader r(bytes);
        const auto events = proto::decode_slow_path_input(r);
        REQUIRE_FALSE(events.has_value());
        CHECK(events.error().code == expected);
    };
    check("01 00 00 00 00 00 00 00 03 00 00 00 00 00 00 00", Errc::unsupported);  // messageType 3
    check("01 00 00 00 00 00 00 00 04 00 00 00", Errc::truncated);
    check("01 02 00 00", Errc::limit_exceeded);  // 513 events
}

TEST_CASE("An empty fast-path input PDU carries its count in the numEvents octet")
{
    Writer w;
    proto::encode_fastpath_input(w, std::vector<proto::InputEvent>{});
    CHECK(to_hex(w.view()) == "00 80 04 00");
    Reader r(w.view());
    CHECK(proto::decode_fastpath_input(r).value().empty());
}
