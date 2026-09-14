// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/platform/input_translator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace platform = farland::platform;
namespace proto = farland::proto;
namespace kbd = proto::kbd_flags;
namespace ptr = proto::ptr_flags;
namespace ptrx = proto::ptrx_flags;

namespace {

using Calls = std::vector<std::string>;
using Events = std::vector<proto::InputEvent>;

class RecordingSink final : public platform::InputSink {
public:
    void key(std::uint32_t evdev_code, bool pressed) override
    {
        record(std::format("key {} {}", evdev_code, pressed ? "down" : "up"));
    }
    void pointer_motion_absolute(double x, double y) override { record(std::format("abs {} {}", x, y)); }
    void pointer_motion_relative(double dx, double dy) override { record(std::format("rel {} {}", dx, dy)); }
    void button(std::uint32_t evdev_button, bool pressed) override
    {
        record(std::format("button {} {}", evdev_button, pressed ? "down" : "up"));
    }
    void scroll_discrete(std::int32_t x_v120, std::int32_t y_v120) override
    {
        record(std::format("scroll {} {}", x_v120, y_v120));
    }
    void text(char32_t codepoint) override
    {
        record(std::format("text U+{:04X}", static_cast<std::uint32_t>(codepoint)));
    }
    void flush() override { record("flush"); }

    Calls take() { return std::exchange(calls_, {}); }

private:
    void record(std::string call) { calls_.push_back(std::move(call)); }

    Calls calls_;
};

struct Harness {
    RecordingSink sink;
    platform::InputTranslator translator{sink};

    Calls operator()(const Events& events)
    {
        translator.translate(events);
        return sink.take();
    }
    Calls release_all()
    {
        translator.release_all();
        return sink.take();
    }
};

proto::InputEvent down(std::uint16_t scancode, std::uint16_t flags = 0)
{
    return proto::KeyboardEvent{flags, scancode};
}

proto::InputEvent up(std::uint16_t scancode, std::uint16_t flags = 0)
{
    return proto::KeyboardEvent{static_cast<std::uint16_t>(flags | kbd::release), scancode};
}

proto::InputEvent unicode(std::uint16_t unit, bool release = false)
{
    return proto::UnicodeKeyboardEvent{release ? kbd::release : std::uint16_t{0}, unit};
}

}  // namespace

TEST_CASE("keys are pressed and released")
{
    Harness h;
    CHECK(h({down(0x1E)}) == Calls{"key 30 down", "flush"});
    CHECK(h({up(0x1E)}) == Calls{"key 30 up", "flush"});
    CHECK(h({down(0x1D), down(0x1E), up(0x1E), up(0x1D)}) ==
          Calls{"key 29 down", "key 30 down", "key 30 up", "key 29 up", "flush"});
}

TEST_CASE("repeated presses and stray releases are dropped")
{
    Harness h;
    CHECK(h({down(0x1E), down(0x1E), down(0x1E)}) == Calls{"key 30 down", "flush"});
    CHECK(h({down(0x1E, kbd::down)}).empty());  // an auto-repeat, with "was down" set
    CHECK(h({up(0x1E)}) == Calls{"key 30 up", "flush"});
    CHECK(h({up(0x1E)}).empty());
    CHECK(h({up(0x30)}).empty());
}

TEST_CASE("extended and unknown keys")
{
    Harness h;
    CHECK(h({down(0x48, kbd::extended), up(0x48, kbd::extended)}) == Calls{"key 103 down", "key 103 up", "flush"});
    CHECK(h({down(0x1D, kbd::extended), down(0x1D)}) == Calls{"key 97 down", "key 29 down", "flush"});
    CHECK(h({down(0x55), down(0x7F), down(0x00), down(0x1FF)}).empty());
}

TEST_CASE("Pause arrives as E1 1D, 45")
{
    Harness h;
    // [MS-RDPBCGR] 2.2.8.1.1.3.1.1.1
    CHECK(h({down(0x1D, kbd::extended1), down(0x45), up(0x1D, kbd::extended1), up(0x45)}) ==
          Calls{"key 119 down", "key 119 up", "flush"});
    // FreeRDP sends each event in a PDU of its own.
    CHECK(h({down(0x1D, kbd::extended1)}) == Calls{"key 119 down", "flush"});
    CHECK(h({down(0x45)}).empty());
    CHECK(h({up(0x1D, kbd::extended1)}) == Calls{"key 119 up", "flush"});
    CHECK(h({up(0x45)}).empty());
    // Num Lock on its own still works.
    CHECK(h({down(0x45), up(0x45)}) == Calls{"key 69 down", "key 69 up", "flush"});
    // Ctrl+Pause (Break).
    CHECK(h({down(0x1D), down(0x46, kbd::extended), up(0x46, kbd::extended), up(0x1D)}) ==
          Calls{"key 29 down", "key 119 down", "key 119 up", "key 29 up", "flush"});
    // A key other than 45 after E1 1D is not swallowed.
    CHECK(h({down(0x1D, kbd::extended1), down(0x1E)}) == Calls{"key 119 down", "key 30 down", "flush"});
}

TEST_CASE("fake shifts are ignored")
{
    Harness h;
    CHECK(h({down(0x2A, kbd::extended), down(0x47, kbd::extended), up(0x47, kbd::extended), up(0x2A, kbd::extended)}) ==
          Calls{"key 102 down", "key 102 up", "flush"});
    CHECK(h({up(0x36, kbd::extended), down(0x4F, kbd::extended), up(0x4F, kbd::extended), down(0x36, kbd::extended)}) ==
          Calls{"key 107 down", "key 107 up", "flush"});
}

TEST_CASE("Unicode events type text")
{
    Harness h;
    CHECK(h({unicode('a'), unicode('a', true)}) == Calls{"text U+0061", "flush"});
    CHECK(h({unicode('a', true)}).empty());
    CHECK(h({unicode(0x20AC)}) == Calls{"text U+20AC", "flush"});
    CHECK(h({unicode('a'), unicode('a')}) == Calls{"text U+0061", "text U+0061", "flush"});
    CHECK(h({unicode(0)}).empty());
}

TEST_CASE("UTF-16 surrogate pairs are combined")
{
    Harness h;
    CHECK(h({unicode(0xD83D), unicode(0xD83D, true), unicode(0xDE00), unicode(0xDE00, true)}) ==
          Calls{"text U+1F600", "flush"});
    CHECK(h({unicode(0xD83D)}).empty());
    CHECK(h({unicode(0xDE00)}) == Calls{"text U+1F600", "flush"});
    CHECK(h({unicode(0xDBFF), unicode(0xDFFF)}) == Calls{"text U+10FFFF", "flush"});
    CHECK(h({unicode(0xD800), unicode(0xDC00)}) == Calls{"text U+10000", "flush"});
    // Lone surrogates are dropped.
    CHECK(h({unicode(0xDC00)}).empty());
    CHECK(h({unicode(0xD83D), unicode('b'), unicode(0xDE00)}) == Calls{"text U+0062", "flush"});
    // A second high surrogate replaces the first.
    CHECK(h({unicode(0xD800), unicode(0xD83D), unicode(0xDE00)}) == Calls{"text U+1F600", "flush"});
}

TEST_CASE("absolute motion without geometry passes through")
{
    Harness h;
    CHECK(h({proto::MouseEvent{ptr::move, 100, 200}}) == Calls{"abs 100 200", "flush"});
    CHECK(h({proto::MouseEvent{ptr::move, 65535, 65535}}) == Calls{"abs 65535 65535", "flush"});
    // A move to the same position is still a move.
    CHECK(h({proto::MouseEvent{ptr::move, 65535, 65535}}) == Calls{"abs 65535 65535", "flush"});
}

TEST_CASE("absolute motion is scaled onto the desktop")
{
    Harness h;
    h.translator.set_geometry(1280, 720, 1920, 1080);
    CHECK(h({proto::MouseEvent{ptr::move, 100, 200}}) == Calls{"abs 150 300", "flush"});
    CHECK(h({proto::MouseEvent{ptr::move, 1279, 719}}) == Calls{"abs 1918.5 1078.5", "flush"});
    CHECK(h({proto::MouseEvent{ptr::move, 5000, 5000}}) == Calls{"abs 1919 1079", "flush"});

    h.translator.set_geometry(3840, 2160, 1920, 1080);
    CHECK(h({proto::MouseEvent{ptr::move, 1001, 3}}) == Calls{"abs 500.5 1.5", "flush"});
    // The last client pixel would be 1919.5; positions stop at the last desktop pixel.
    CHECK(h({proto::MouseEvent{ptr::move, 3839, 2159}}) == Calls{"abs 1919 1079", "flush"});

    h.translator.set_geometry(1920, 1080, 1920, 1080, 1920, -1080);
    CHECK(h({proto::MouseEvent{ptr::move, 10, 20}}) == Calls{"abs 1930 -1060", "flush"});

    h.translator.set_geometry(0, 0, 0, 0);
    CHECK(h({proto::MouseEvent{ptr::move, 5000, 5000}}) == Calls{"abs 5000 5000", "flush"});
}

TEST_CASE("mouse buttons")
{
    Harness h;
    CHECK(h({proto::MouseEvent{ptr::move, 10, 20}, proto::MouseEvent{ptr::down | ptr::button1, 10, 20},
             proto::MouseEvent{ptr::button1, 10, 20}}) ==
          Calls{"abs 10 20", "button 272 down", "button 272 up", "flush"});
    // A click somewhere else moves the pointer there first.
    CHECK(h({proto::MouseEvent{ptr::down | ptr::button2, 30, 40}}) == Calls{"abs 30 40", "button 273 down", "flush"});
    CHECK(h({proto::MouseEvent{ptr::button2, 30, 40}}) == Calls{"button 273 up", "flush"});
    CHECK(h({proto::MouseEvent{ptr::down | ptr::button3, 30, 40}, proto::MouseEvent{ptr::button3, 30, 40}}) ==
          Calls{"button 274 down", "button 274 up", "flush"});
    // Extended buttons X1 and X2.
    CHECK(h({proto::ExtendedMouseEvent{ptrx::down | ptrx::button1, 30, 40},
             proto::ExtendedMouseEvent{ptrx::button1, 30, 40},
             proto::ExtendedMouseEvent{ptrx::down | ptrx::button2, 30, 40},
             proto::ExtendedMouseEvent{ptrx::button2, 30, 40}}) ==
          Calls{"button 275 down", "button 275 up", "button 276 down", "button 276 up", "flush"});
    CHECK(h({proto::ExtendedMouseEvent{ptrx::down | ptrx::button1, 50, 60}}) ==
          Calls{"abs 50 60", "button 275 down", "flush"});
    // Repeated presses and stray releases are dropped.
    CHECK(h({proto::ExtendedMouseEvent{ptrx::down | ptrx::button1, 50, 60}}).empty());
    CHECK(h({proto::MouseEvent{ptr::button1, 50, 60}}).empty());
    // Nothing to do.
    CHECK(h({proto::ExtendedMouseEvent{ptrx::down, 1, 2}, proto::MouseEvent{0, 1, 2}}).empty());
}

TEST_CASE("wheel rotation becomes v120 scrolling")
{
    Harness h;
    // Positive vertical rotation is away from the user: scroll up.
    CHECK(h({proto::MouseEvent{ptr::wheel | 0x78, 500, 500}}) == Calls{"scroll 0 -120", "flush"});
    CHECK(h({proto::MouseEvent{ptr::wheel | ptr::wheel_negative | 0x88, 0, 0}}) == Calls{"scroll 0 120", "flush"});
    // Positive horizontal rotation scrolls right.
    CHECK(h({proto::MouseEvent{ptr::hwheel | 0x78, 0, 0}}) == Calls{"scroll 120 0", "flush"});
    CHECK(h({proto::MouseEvent{ptr::hwheel | ptr::wheel_negative | 0x88, 0, 0}}) == Calls{"scroll -120 0", "flush"});
    // Magnitudes: high-resolution wheels send fractions of a notch.
    CHECK(h({proto::MouseEvent{ptr::wheel | 0x0F, 0, 0}}) == Calls{"scroll 0 -15", "flush"});
    CHECK(h({proto::MouseEvent{ptr::wheel | 0xFF, 0, 0}}) == Calls{"scroll 0 -255", "flush"});
    CHECK(h({proto::MouseEvent{ptr::wheel | ptr::wheel_negative | 0x01, 0, 0}}) == Calls{"scroll 0 255", "flush"});
    CHECK(h({proto::MouseEvent{ptr::wheel | ptr::wheel_negative, 0, 0}}) == Calls{"scroll 0 256", "flush"});
    // The vertical wheel wins; position and buttons are ignored.
    CHECK(h({proto::MouseEvent{ptr::wheel | ptr::hwheel | 0x78, 0, 0}}) == Calls{"scroll 0 -120", "flush"});
    CHECK(h({proto::MouseEvent{ptr::wheel | ptr::move | ptr::down | ptr::button1 | 0x78, 9, 9}}) ==
          Calls{"scroll 0 -120", "flush"});
    CHECK(h({proto::MouseEvent{ptr::wheel, 0, 0}}).empty());
}

TEST_CASE("relative motion")
{
    Harness h;
    CHECK(h({proto::RelativeMouseEvent{ptr::move, -12, 34}}) == Calls{"rel -12 34", "flush"});
    CHECK(h({proto::RelativeMouseEvent{ptr::down | ptr::button1, 0, 0}}) == Calls{"button 272 down", "flush"});
    CHECK(h({proto::RelativeMouseEvent{ptr::button1, 5, 0}}) == Calls{"rel 5 0", "button 272 up", "flush"});
    CHECK(h({proto::RelativeMouseEvent{ptr::down | ptrx::button2, 0, 0},
             proto::RelativeMouseEvent{ptrx::button2, 0, 0}}) == Calls{"button 276 down", "button 276 up", "flush"});
    CHECK(h({proto::RelativeMouseEvent{0, 3, 3}}).empty());
    CHECK(h({proto::RelativeMouseEvent{ptr::move, -32768, 32767}}) == Calls{"rel -32768 32767", "flush"});
    // After relative motion the absolute position is unknown, so a click moves the pointer again.
    CHECK(h({proto::MouseEvent{ptr::move, 10, 20}}) == Calls{"abs 10 20", "flush"});
    CHECK(h({proto::RelativeMouseEvent{ptr::move, 1, 1}}) == Calls{"rel 1 1", "flush"});
    CHECK(h({proto::MouseEvent{ptr::down | ptr::button1, 10, 20}}) == Calls{"abs 10 20", "button 272 down", "flush"});
}

TEST_CASE("a synchronize event releases everything and records the toggle state")
{
    Harness h;
    CHECK_FALSE(h.translator.take_sync().has_value());
    CHECK(h({down(0x1E), proto::MouseEvent{ptr::down | ptr::button1, 1, 1}}) ==
          Calls{"key 30 down", "abs 1 1", "button 272 down", "flush"});
    CHECK(h({proto::SyncEvent{proto::sync_flags::caps_lock | proto::sync_flags::num_lock}, down(0x1D)}) ==
          Calls{"key 30 up", "button 272 up", "key 29 down", "flush"});
    CHECK(h.translator.take_sync() == platform::LockState{.num_lock = true, .caps_lock = true});
    CHECK_FALSE(h.translator.take_sync().has_value());

    CHECK(h({proto::SyncEvent{0xF}}) == Calls{"key 29 up", "flush"});
    CHECK(h({proto::SyncEvent{proto::sync_flags::scroll_lock}}).empty());
    CHECK(h.translator.take_sync() == platform::LockState{.scroll_lock = true});
    CHECK(h({proto::SyncEvent{proto::sync_flags::kana_lock}}).empty());
    CHECK(h.translator.take_sync() == platform::LockState{.kana_lock = true});
}

TEST_CASE("release_all releases every key and button")
{
    Harness h;
    CHECK(h({down(0x38, kbd::extended), down(0x1E), proto::MouseEvent{ptr::down | ptr::button1, 1, 1},
             proto::ExtendedMouseEvent{ptrx::down | ptrx::button2, 1, 1}}) ==
          Calls{"key 100 down", "key 30 down", "abs 1 1", "button 272 down", "button 276 down", "flush"});
    CHECK(h.release_all() == Calls{"key 30 up", "key 100 up", "button 272 up", "button 276 up", "flush"});
    CHECK(h.release_all().empty());
    // It also forgets half of a surrogate pair.
    CHECK(h({unicode(0xD83D)}).empty());
    CHECK(h.release_all().empty());
    CHECK(h({unicode(0xDE00)}).empty());
}

TEST_CASE("QoE timestamps and empty PDUs produce nothing")
{
    Harness h;
    CHECK(h({proto::QoeTimestampEvent{1234}}).empty());
    CHECK(h({}).empty());
}

TEST_CASE("any input leaves the sink balanced")
{
    Harness h;
    h.translator.set_geometry(640, 480, 1920, 1080, 100, 100);
    std::size_t downs = 0;
    std::size_t ups = 0;
    const auto count = [&](const Calls& calls) {
        for (const std::string& call : calls) {
            if (call.ends_with(" down")) {
                ++downs;
            } else if (call.ends_with(" up")) {
                ++ups;
            }
        }
    };
    for (std::uint32_t flags = 0; flags <= 0xFFFF; flags += 0x100) {
        for (std::uint32_t code = 0; code <= 0x1FF; ++code) {
            count(h({proto::KeyboardEvent{static_cast<std::uint16_t>(flags), static_cast<std::uint16_t>(code)}}));
        }
    }
    for (std::uint32_t unit = 0; unit <= 0xFFFF; ++unit) {
        count(h({unicode(static_cast<std::uint16_t>(unit)), unicode(static_cast<std::uint16_t>(unit), true)}));
    }
    for (std::uint32_t flags = 0; flags <= 0xFFFF; ++flags) {
        const auto f = static_cast<std::uint16_t>(flags);
        count(h({proto::MouseEvent{f, 65535, 0}, proto::ExtendedMouseEvent{f, 0, 65535},
                 proto::RelativeMouseEvent{f, -32768, 32767}}));
    }
    count(h.release_all());
    CHECK(downs > 0);
    CHECK(downs == ups);
    CHECK(h.release_all().empty());
}
