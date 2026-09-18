// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/platform/portal/ei_input.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using farland::platform::portal::EiInput;

TEST_CASE("EiInput rejects an invalid EIS socket")
{
    CHECK_FALSE(EiInput::connect_fd(-1).has_value());
    CHECK_FALSE(EiInput::connect_socket("/nonexistent/farland-test-eis-socket").has_value());
}

#if !defined(FARLAND_TEST_HAVE_LIBEIS)

TEST_CASE("EiInput against an in-process EIS server")
{
    SKIP("libeis-1.0 is not available");
}

#else

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <libeis.h>
#include <linux/input-event-codes.h>
#include <memory>
#include <poll.h>
#include <utility>
#include <vector>

namespace {

using Capability = EiInput::Capability;

/// One event the EIS server received, with the raw values kept for checks
/// that need more than the text form.
struct Received {
    enum eis_event_type type {};
    std::string device;
    std::string text;
    double x = 0;
    double y = 0;
    std::uint32_t sequence = 0;
};

std::string device_name(struct eis_event* event)
{
    struct eis_device* device = eis_event_get_device(event);
    const char* name = device != nullptr ? eis_device_get_name(device) : nullptr;
    return name != nullptr ? name : "?";
}

/// An EIS server like a compositor's: one seat, a keyboard, a relative
/// pointer and an absolute pointer spanning two monitors, the second at a
/// physical scale of 1.5 (1280x720 logical, 1920x1080 pixels).
class FakeEis {
public:
    /// Without a path the server hands out client sockets itself, as the
    /// portal does with ConnectToEIS.
    explicit FakeEis(const std::string& socket_path = {}) : eis_(eis_new(nullptr))
    {
        REQUIRE(eis_ != nullptr);
        eis_log_set_priority(eis_, EIS_LOG_PRIORITY_ERROR);
        if (socket_path.empty()) {
            REQUIRE(eis_setup_backend_fd(eis_) == 0);
        } else {
            REQUIRE(eis_setup_backend_socket(eis_, socket_path.c_str()) == 0);
        }
    }

    ~FakeEis()
    {
        for (auto* device : {keyboard, pointer, absolute, touchscreen}) {
            if (device != nullptr) {
                eis_device_unref(device);
            }
        }
        if (seat_ != nullptr) {
            eis_seat_unref(seat_);
        }
        if (client_ != nullptr) {
            eis_client_unref(client_);
        }
        eis_unref(eis_);
    }

    FakeEis(const FakeEis&) = delete;
    FakeEis& operator=(const FakeEis&) = delete;
    FakeEis(FakeEis&&) = delete;
    FakeEis& operator=(FakeEis&&) = delete;

    [[nodiscard]] int add_client() { return eis_backend_fd_add_client(eis_); }
    [[nodiscard]] int fd() const { return eis_get_fd(eis_); }

    void dispatch()
    {
        eis_dispatch(eis_);
        while (struct eis_event* event = eis_get_event(eis_)) {
            handle(event);
            eis_event_unref(event);
        }
    }

    void disconnect_client() { eis_client_disconnect(client_); }

    [[nodiscard]] std::vector<Received> take() { return std::exchange(events, {}); }

    struct eis_device* keyboard = nullptr;
    struct eis_device* pointer = nullptr;
    struct eis_device* absolute = nullptr;
    struct eis_device* touchscreen = nullptr;
    std::vector<Received> events;
    std::string client_name;
    bool client_is_sender = false;
    bool client_gone = false;
    /// Adds the keyboard paused instead of resumed.
    bool keyboard_paused = false;
    /// Adds a touchscreen over both monitors.
    bool with_touch = false;

private:
    struct RegionSpec {
        std::uint32_t x, y, width, height;
        double scale;
        const char* mapping_id;
    };

    struct eis_device* add_device(const char* name, std::initializer_list<enum eis_device_capability> caps,
                                  std::initializer_list<RegionSpec> regions = {}, bool resume = true)
    {
        struct eis_device* device = eis_seat_new_device(seat_);
        eis_device_configure_name(device, name);
        for (const auto cap : caps) {
            eis_device_configure_capability(device, cap);
        }
        for (const auto& spec : regions) {
            struct eis_region* region = eis_device_new_region(device);
            eis_region_set_offset(region, spec.x, spec.y);
            eis_region_set_size(region, spec.width, spec.height);
            eis_region_set_physical_scale(region, spec.scale);
            eis_region_set_mapping_id(region, spec.mapping_id);
            eis_region_add(region);
            eis_region_unref(region);
        }
        eis_device_add(device);
        if (resume) {
            eis_device_resume(device);
        }
        return device;
    }

    void handle(struct eis_event* event)
    {
        const auto type = eis_event_get_type(event);
        // Protocol round trips, not input: EIS_EVENT_PONG (90) and
        // EIS_EVENT_SYNC (91), which newer libeis versions report. Matched by
        // number, since older libeis headers (Ubuntu 24.04) name neither.
        if (const int number = static_cast<int>(type); number == 90 || number == 91) {
            return;
        }
        Received r;
        r.type = type;
        r.device = device_name(event);
        switch (type) {
        case EIS_EVENT_CLIENT_CONNECT: {
            client_ = eis_client_ref(eis_event_get_client(event));
            client_name = eis_client_get_name(client_);
            client_is_sender = eis_client_is_sender(client_);
            eis_client_connect(client_);
            seat_ = eis_client_new_seat(client_, "seat0");
            for (const auto cap : {EIS_DEVICE_CAP_KEYBOARD, EIS_DEVICE_CAP_POINTER, EIS_DEVICE_CAP_POINTER_ABSOLUTE,
                                   EIS_DEVICE_CAP_BUTTON, EIS_DEVICE_CAP_SCROLL, EIS_DEVICE_CAP_TOUCH}) {
                eis_seat_configure_capability(seat_, cap);
            }
            eis_seat_add(seat_);
            return;
        }
        case EIS_EVENT_CLIENT_DISCONNECT:
            client_gone = true;
            return;
        case EIS_EVENT_SEAT_BIND:
            if (keyboard == nullptr && eis_event_seat_has_capability(event, EIS_DEVICE_CAP_KEYBOARD)) {
                keyboard = add_device("keyboard", {EIS_DEVICE_CAP_KEYBOARD}, {}, !keyboard_paused);
            }
            if (pointer == nullptr && eis_event_seat_has_capability(event, EIS_DEVICE_CAP_POINTER)) {
                pointer = add_device("pointer", {EIS_DEVICE_CAP_POINTER, EIS_DEVICE_CAP_BUTTON, EIS_DEVICE_CAP_SCROLL});
            }
            if (absolute == nullptr && eis_event_seat_has_capability(event, EIS_DEVICE_CAP_POINTER_ABSOLUTE)) {
                absolute = add_device("absolute",
                                      {EIS_DEVICE_CAP_POINTER_ABSOLUTE, EIS_DEVICE_CAP_BUTTON, EIS_DEVICE_CAP_SCROLL},
                                      {{0, 0, 1920, 1080, 1.0, "monitor-a"}, {1920, 0, 1280, 720, 1.5, "monitor-b"}});
            }
            if (with_touch && touchscreen == nullptr && eis_event_seat_has_capability(event, EIS_DEVICE_CAP_TOUCH)) {
                touchscreen =
                    add_device("touch", {EIS_DEVICE_CAP_TOUCH},
                               {{0, 0, 1920, 1080, 1.0, "monitor-a"}, {1920, 0, 1280, 720, 1.5, "monitor-b"}});
            }
            return;
        case EIS_EVENT_DEVICE_START_EMULATING:
            r.sequence = eis_event_emulating_get_sequence(event);
            r.text = r.device + " start";
            break;
        case EIS_EVENT_DEVICE_STOP_EMULATING:
            r.text = r.device + " stop";
            break;
        case EIS_EVENT_FRAME:
            r.text = r.device + " frame";
            break;
        case EIS_EVENT_KEYBOARD_KEY:
            r.text = std::format("{} key {} {}", r.device, eis_event_keyboard_get_key(event),
                                 eis_event_keyboard_get_key_is_press(event) ? "down" : "up");
            break;
        case EIS_EVENT_BUTTON_BUTTON:
            r.text = std::format("{} button {} {}", r.device, eis_event_button_get_button(event),
                                 eis_event_button_get_is_press(event) ? "down" : "up");
            break;
        case EIS_EVENT_POINTER_MOTION:
            r.x = eis_event_pointer_get_dx(event);
            r.y = eis_event_pointer_get_dy(event);
            r.text = std::format("{} motion {:.2f} {:.2f}", r.device, r.x, r.y);
            break;
        case EIS_EVENT_POINTER_MOTION_ABSOLUTE:
            r.x = eis_event_pointer_get_absolute_x(event);
            r.y = eis_event_pointer_get_absolute_y(event);
            r.text = std::format("{} abs {:.2f} {:.2f}", r.device, r.x, r.y);
            break;
        case EIS_EVENT_TOUCH_DOWN:
        case EIS_EVENT_TOUCH_MOTION:
            // libei numbers touches itself; the id goes into `sequence`.
            r.sequence = eis_event_touch_get_id(event);
            r.x = eis_event_touch_get_x(event);
            r.y = eis_event_touch_get_y(event);
            r.text = std::format("{} {} {:.2f} {:.2f}", r.device, type == EIS_EVENT_TOUCH_DOWN ? "down" : "motion", r.x,
                                 r.y);
            break;
        case EIS_EVENT_TOUCH_UP:
            // A cancel is an up too (with eis_event_touch_get_is_cancel()
            // from libeis 1.3 on).
            r.sequence = eis_event_touch_get_id(event);
            r.text = r.device + " up";
            break;
        case EIS_EVENT_SCROLL_DISCRETE:
            r.text = std::format("{} discrete {} {}", r.device, eis_event_scroll_get_discrete_dx(event),
                                 eis_event_scroll_get_discrete_dy(event));
            break;
        case EIS_EVENT_SCROLL_DELTA:
            r.text = std::format("{} scroll {:.2f} {:.2f}", r.device, eis_event_scroll_get_dx(event),
                                 eis_event_scroll_get_dy(event));
            break;
        default:
            r.text = std::format("{} {}", r.device, eis_event_type_to_string(type));
            break;
        }
        events.push_back(std::move(r));
    }

    struct eis* eis_;
    struct eis_client* client_ = nullptr;
    struct eis_seat* seat_ = nullptr;
};

/// Runs both sides until `done` holds (at most about five seconds).
template <class Predicate>
bool pump(FakeEis& server, EiInput* input, Predicate done)
{
    for (int i = 0; i < 500; ++i) {
        server.dispatch();
        if (input != nullptr) {
            input->dispatch();
        }
        if (done()) {
            return true;
        }
        std::array<pollfd, 2> fds{{{.fd = server.fd(), .events = POLLIN, .revents = 0},
                                   {.fd = input != nullptr ? input->fd() : -1, .events = POLLIN, .revents = 0}}};
        ::poll(fds.data(), fds.size(), 10);
    }
    return done();
}

std::vector<std::string> texts(const std::vector<Received>& events, const std::string& device = {})
{
    std::vector<std::string> out;
    for (const auto& e : events) {
        if (device.empty() || e.device == device) {
            out.push_back(e.text);
        }
    }
    return out;
}

constexpr double sentinel_dx = 0.25;

/// A connected client with all three devices resumed.
struct Session {
    FakeEis server;
    std::unique_ptr<EiInput> input;
    std::vector<Received> started;

    /// With `keyboard_paused`, the keyboard is added but not resumed; with
    /// `touch`, a touchscreen is added as well.
    explicit Session(bool keyboard_paused = false, bool touch = false)
    {
        server.keyboard_paused = keyboard_paused;
        server.with_touch = touch;
        auto connected = EiInput::connect_fd(server.add_client());
        REQUIRE(connected.has_value());
        input = std::move(*connected);
        const auto expected_starts = (keyboard_paused ? 2 : 3) + (touch ? 1 : 0);
        REQUIRE(pump(server, input.get(), [&] {
            return input->can_send(Capability::keyboard) != keyboard_paused && input->can_send(Capability::pointer) &&
                   input->can_send(Capability::pointer_absolute) && input->can_send(Capability::touch) == touch &&
                   std::ranges::count(server.events, EIS_EVENT_DEVICE_START_EMULATING, &Received::type) ==
                       expected_starts;
        }));
        started = server.take();
    }

    /// Runs only the client until `done` holds, so what it sends stays unread
    /// in the server's socket.
    template <class Predicate>
    bool pump_client(Predicate done)
    {
        for (int i = 0; i < 500 && !done(); ++i) {
            pollfd fd{.fd = input->fd(), .events = POLLIN, .revents = 0};
            ::poll(&fd, 1, 10);
            input->dispatch();
        }
        return done();
    }

    /// Everything the server received up to now. A relative motion on the
    /// pointer marks the end, and is removed again.
    std::vector<Received> sync()
    {
        input->pointer_motion_relative(sentinel_dx, 0);
        input->flush();
        const auto sentinel_seen = [&] {
            const auto& ev = server.events;
            return ev.size() >= 2 && ev.back().type == EIS_EVENT_FRAME && ev.back().device == "pointer" &&
                   ev[ev.size() - 2].type == EIS_EVENT_POINTER_MOTION && ev[ev.size() - 2].x == sentinel_dx;
        };
        REQUIRE(pump(server, input.get(), sentinel_seen));
        auto events = server.take();
        events.resize(events.size() - 2);
        return events;
    }
};

}  // namespace

TEST_CASE("EiInput binds the seat and emulates on every device")
{
    Session s;
    CHECK(s.input->state() == EiInput::State::connected);
    CHECK_FALSE(s.input->closed());
    CHECK(s.server.client_name == "farland");
    CHECK(s.server.client_is_sender);
    CHECK(s.input->can_send(Capability::button));
    CHECK(s.input->can_send(Capability::scroll));
    // The seat offers touch, but the server created no touch device.
    CHECK_FALSE(s.input->can_send(Capability::touch));

    std::vector<std::uint32_t> sequences;
    for (const auto& e : s.started) {
        REQUIRE(e.type == EIS_EVENT_DEVICE_START_EMULATING);
        sequences.push_back(e.sequence);
    }
    REQUIRE(sequences.size() == 3);
    // libeis up to 1.5 drops the sequence number and reports 0.
    if (std::ranges::find(sequences, 0U) == sequences.end()) {
        CHECK(sequences[0] < sequences[1]);
        CHECK(sequences[1] < sequences[2]);
    }

    // Nothing sent, nothing framed.
    s.input->flush();
    CHECK(s.sync().empty());
}

TEST_CASE("EiInput sends keys through the keyboard device")
{
    Session s;
    s.input->key(KEY_A, true);
    s.input->key(KEY_A, true);  // a client's key repeat: the compositor repeats by itself
    s.input->key(KEY_A, false);
    s.input->key(KEY_B, false);  // never pressed
    s.input->text(U'é');         // no text input in EIS
    s.input->flush();
    CHECK(texts(s.sync()) == std::vector<std::string>{"keyboard key 30 down", "keyboard key 30 up", "keyboard frame"});
}

TEST_CASE("EiInput maps desktop pixels into the absolute device's regions")
{
    Session s;

    SECTION("regions at their offsets, scaled by their physical scale")
    {
        s.input->pointer_motion_absolute(100, 200);
        s.input->pointer_motion_absolute(2880, 540);  // the second monitor: (1920, 0) + (960, 540) / 1.5
        s.input->pointer_motion_absolute(-50, 5000);  // below the first monitor: clamped into it
        s.input->pointer_motion_absolute(5000, 100);  // right of the second monitor
        s.input->flush();
        const auto events = s.sync();
        CHECK(texts(events) == std::vector<std::string>{"absolute abs 100.00 200.00", "absolute abs 2560.00 360.00",
                                                        "absolute abs 0.00 1080.00", "absolute abs 3200.00 66.67",
                                                        "absolute frame"});
        REQUIRE(events.size() == 5);
        // Clamped points stay inside the half-open regions.
        CHECK(events[2].y < 1080);
        CHECK(events[3].x < 3200);
    }

    SECTION("outputs matched by mapping id")
    {
        // The second monitor left of the first in the desktop.
        s.input->set_outputs({{.desktop = {0, 0, 1920, 1080}, .mapping_id = "monitor-b"},
                              {.desktop = {1920, 0, 1920, 1080}, .mapping_id = "monitor-a"}});
        s.input->pointer_motion_absolute(960, 540);
        s.input->pointer_motion_absolute(2000, 100);
        s.input->pointer_motion_absolute(4000, 100);  // right of everything: clamped into monitor-a
        s.input->flush();
        CHECK(texts(s.sync()) == std::vector<std::string>{"absolute abs 2560.00 360.00", "absolute abs 80.00 100.00",
                                                          "absolute abs 1920.00 100.00", "absolute frame"});
    }

    SECTION("outputs without a matching region fall back to the region layout")
    {
        s.input->set_outputs({{.desktop = {0, 0, 10, 10}, .mapping_id = "elsewhere"}});
        s.input->pointer_motion_absolute(100, 200);
        s.input->flush();
        CHECK(texts(s.sync()) == std::vector<std::string>{"absolute abs 100.00 200.00", "absolute frame"});
    }
}

TEST_CASE("EiInput sends touches through the touch device, mapped into its regions")
{
    Session s(false, true);
    CHECK(s.input->accepts_touch());
    s.input->touch_down(7, 100, 200);
    s.input->touch_down(9, 2880, 540);  // the second monitor
    s.input->flush();
    s.input->touch_motion(7, 110, 210);
    s.input->touch_motion(9, 100, 100);  // onto the first monitor, on the same touchscreen
    s.input->touch_motion(3, 1, 1);      // never down
    s.input->touch_up(7);
    s.input->touch_cancel(9);
    s.input->touch_up(7);  // already up
    s.input->flush();
    const auto events = s.sync();
    // libei ends a frame after each touch that goes up.
    CHECK(texts(events) == std::vector<std::string>{"touch down 100.00 200.00", "touch down 2560.00 360.00",
                                                    "touch frame", "touch motion 110.00 210.00",
                                                    "touch motion 100.00 100.00", "touch up", "touch frame", "touch up",
                                                    "touch frame"});
    REQUIRE(events.size() == 9);
    CHECK(events[0].sequence != events[1].sequence);
    CHECK(events[3].sequence == events[0].sequence);
    CHECK(events[4].sequence == events[1].sequence);

    SECTION("a second down of a slot moves it")
    {
        s.input->touch_down(1, 10, 10);
        s.input->touch_down(1, 20, 20);
        s.input->flush();
        CHECK(texts(s.sync()) ==
              std::vector<std::string>{"touch down 10.00 10.00", "touch motion 20.00 20.00", "touch frame"});
    }

    SECTION("touches still down are lifted when EiInput goes away")
    {
        s.input->touch_down(1, 10, 10);
        s.input->flush();
        static_cast<void>(s.sync());
        s.input.reset();
        REQUIRE(pump(s.server, nullptr, [&] { return s.server.client_gone; }));
        CHECK(texts(s.server.take(), "touch") ==
              std::vector<std::string>{"touch up", "touch frame", "touch stop", "touch EIS_EVENT_DEVICE_CLOSED"});
    }
}

TEST_CASE("EiInput sends relative motion, buttons and wheel steps")
{
    Session s;
    s.input->pointer_motion_relative(5, -3);
    s.input->button(BTN_LEFT, true);  // goes where the pointer last moved: the relative pointer
    s.input->pointer_motion_absolute(10, 10);
    s.input->button(BTN_LEFT, false);  // the release follows its press
    s.input->button(BTN_RIGHT, true);
    s.input->button(BTN_RIGHT, false);
    s.input->scroll_discrete(0, 120);
    s.input->scroll_discrete(-240, 0);
    s.input->scroll_discrete(0, 0);  // nothing to send
    s.input->flush();
    const auto events = s.sync();
    CHECK(texts(events, "pointer") == std::vector<std::string>{"pointer motion 5.00 -3.00", "pointer button 272 down",
                                                               "pointer button 272 up", "pointer frame"});
    CHECK(texts(events, "absolute") == std::vector<std::string>{"absolute abs 10.00 10.00", "absolute button 273 down",
                                                                "absolute button 273 up", "absolute scroll 0.00 30.00",
                                                                "absolute scroll -60.00 0.00", "absolute frame"});
    CHECK(texts(events, "keyboard").empty());
}

TEST_CASE("EiInput frames once per flush on each device that got events")
{
    Session s;
    s.input->key(KEY_LEFTSHIFT, true);
    s.input->pointer_motion_absolute(1, 2);
    s.input->key(KEY_A, true);
    s.input->key(KEY_A, false);
    s.input->key(KEY_LEFTSHIFT, false);
    s.input->flush();
    s.input->pointer_motion_absolute(3, 4);
    s.input->flush();
    const auto events = s.sync();
    CHECK(texts(events, "keyboard") == std::vector<std::string>{"keyboard key 42 down", "keyboard key 30 down",
                                                                "keyboard key 30 up", "keyboard key 42 up",
                                                                "keyboard frame"});
    CHECK(texts(events, "absolute") == std::vector<std::string>{"absolute abs 1.00 2.00", "absolute frame",
                                                                "absolute abs 3.00 4.00", "absolute frame"});
}

TEST_CASE("EiInput holds releases back while a device is paused")
{
    // libeis up to 1.5 ignores eis_device_pause() once the client emulates on
    // the device, so the server pauses while the client's start and presses
    // are still unread in its socket: a compositor pausing a device the client
    // just started using. libeis drops what arrives for the paused device.
    Session s(true);
    CHECK_FALSE(s.input->can_send(Capability::keyboard));
    s.input->key(KEY_Z, true);  // dropped: no keyboard yet
    s.input->flush();

    eis_device_resume(s.server.keyboard);
    REQUIRE(s.pump_client([&] { return s.input->can_send(Capability::keyboard); }));
    s.input->key(KEY_A, true);
    s.input->key(KEY_LEFTSHIFT, true);
    s.input->flush();

    eis_device_pause(s.server.keyboard);
    REQUIRE(s.pump_client([&] { return !s.input->can_send(Capability::keyboard); }));
    s.input->key(KEY_A, false);          // held back until the resume
    s.input->key(KEY_B, true);           // dropped: the keyboard is paused
    s.input->key(KEY_LEFTSHIFT, false);  // released ...
    s.input->key(KEY_LEFTSHIFT, true);   // ... and pressed again: still held
    s.input->key(KEY_Z, false);          // its press never went out
    s.input->flush();
    CHECK(texts(s.sync(), "keyboard").empty());

    eis_device_resume(s.server.keyboard);
    REQUIRE(pump(s.server, s.input.get(), [&] { return s.input->can_send(Capability::keyboard); }));
    CHECK(texts(s.sync(), "keyboard") ==
          std::vector<std::string>{"keyboard start", "keyboard key 30 up", "keyboard frame"});

    s.input->key(KEY_B, false);  // its press never went out
    s.input->key(KEY_LEFTSHIFT, false);
    s.input->flush();
    CHECK(texts(s.sync()) == std::vector<std::string>{"keyboard key 42 up", "keyboard frame"});
}

TEST_CASE("EiInput releases held keys and buttons when destroyed")
{
    Session s;
    s.input->key(KEY_LEFTCTRL, true);
    s.input->pointer_motion_absolute(10, 10);
    s.input->button(BTN_LEFT, true);
    s.input->flush();
    static_cast<void>(s.sync());

    s.input.reset();
    REQUIRE(pump(s.server, nullptr, [&] { return s.server.client_gone; }));
    const auto events = s.server.take();
    CHECK(texts(events, "keyboard") == std::vector<std::string>{"keyboard key 29 up", "keyboard frame", "keyboard stop",
                                                                "keyboard EIS_EVENT_DEVICE_CLOSED"});
    CHECK(texts(events, "absolute") == std::vector<std::string>{"absolute button 272 up", "absolute frame",
                                                                "absolute stop", "absolute EIS_EVENT_DEVICE_CLOSED"});
    CHECK(texts(events, "pointer") == std::vector<std::string>{"pointer stop", "pointer EIS_EVENT_DEVICE_CLOSED"});
}

TEST_CASE("EiInput notices when the EIS server disconnects")
{
    Session s;
    s.input->key(KEY_A, true);
    s.input->flush();
    static_cast<void>(s.sync());

    s.server.disconnect_client();
    REQUIRE(pump(s.server, s.input.get(), [&] { return s.input->closed(); }));
    CHECK(s.input->state() == EiInput::State::disconnected);
    CHECK_FALSE(s.input->can_send(Capability::keyboard));

    // Input after the disconnection goes nowhere, without crashing.
    s.input->key(KEY_A, false);
    s.input->pointer_motion_absolute(1, 1);
    s.input->flush();
    s.input->dispatch();
    s.input.reset();
}

TEST_CASE("EiInput connects to an EIS socket path")
{
    std::string dir_template = (std::filesystem::temp_directory_path() / "farland-eis-XXXXXX").string();
    REQUIRE(::mkdtemp(dir_template.data()) != nullptr);
    const std::filesystem::path dir(dir_template);
    const std::string path = (dir / "eis-0").string();
    {
        FakeEis server(path);
        auto input = EiInput::connect_socket(path);
        REQUIRE(input.has_value());
        REQUIRE(pump(server, input->get(), [&] { return (*input)->can_send(Capability::keyboard); }));
        (*input)->key(KEY_Q, true);
        (*input)->key(KEY_Q, false);
        (*input)->flush();
        REQUIRE(pump(server, input->get(),
                     [&] { return std::ranges::count(server.events, EIS_EVENT_KEYBOARD_KEY, &Received::type) == 2; }));
        CHECK(texts(server.events, "keyboard") == std::vector<std::string>{"keyboard start", "keyboard key 16 down",
                                                                           "keyboard key 16 up", "keyboard frame"});
    }
    std::filesystem::remove_all(dir);
}

#endif
