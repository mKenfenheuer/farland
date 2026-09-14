// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Touch and pen input end to end, in process: a scripted client opens the
// RDPEI channel through drdynvc and sends what a touch client sends; the
// server's TouchInput validates it and the InputTranslator turns it into
// InputSink calls, as the session does.

#include <farland/channels/drdynvc.hpp>
#include <farland/channels/rdpei.hpp>
#include <farland/channels/svc.hpp>
#include <farland/platform/input_translator.hpp>
#include <farland/server/touch_input.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace dyn = farland::channels::drdynvc;
namespace rdpei = farland::channels::rdpei;
namespace svc = farland::channels::svc;
namespace te = farland::server::touch_event;
using farland::server::DynamicChannels;
using farland::server::TouchEvent;
using farland::server::TouchInput;
using Bytes = std::vector<std::byte>;
using Calls = std::vector<std::string>;

namespace {

namespace cf = rdpei::contact_flags;
constexpr std::uint32_t down = cf::down | cf::in_range | cf::in_contact;
constexpr std::uint32_t update = cf::update | cf::in_range | cf::in_contact;
constexpr std::uint32_t hover = cf::update | cf::in_range;

/// The client end of drdynvc.
class Client {
public:
    DynamicChannels::SendChunk sink()
    {
        return [this](std::span<const std::byte> chunk) { chunks_.emplace_back(chunk.begin(), chunk.end()); };
    }

    std::vector<dyn::ServerPdu> take()
    {
        std::vector<dyn::ServerPdu> out;
        for (const auto& chunk : std::exchange(chunks_, {})) {
            if (auto message = reassembler_.add(chunk).value()) {
                storage_.push_back(std::move(*message));
                out.push_back(dyn::decode_server_pdu(storage_.back()).value());
            }
        }
        return out;
    }

    /// The RDPEI PDUs the server sent on `channel` since the last call.
    std::vector<rdpei::ServerPdu> take_input(std::uint32_t channel)
    {
        std::vector<rdpei::ServerPdu> out;
        for (const auto& pdu : take()) {
            const auto* data = std::get_if<dyn::Data>(&pdu);
            REQUIRE(data != nullptr);
            REQUIRE(data->channel_id == channel);
            out.push_back(rdpei::decode_server_pdu(data->data).value());
        }
        return out;
    }

private:
    std::vector<Bytes> chunks_;
    std::deque<Bytes> storage_;
    svc::Reassembler reassembler_{std::size_t{1} << 20U};
};

class RecordingSink final : public farland::platform::InputSink {
public:
    explicit RecordingSink(bool touch) : touch_(touch) {}

    void key(std::uint32_t code, bool pressed) override { record(std::format("key {} {}", code, pressed)); }
    void pointer_motion_absolute(double x, double y) override { record(std::format("abs {} {}", x, y)); }
    void pointer_motion_relative(double dx, double dy) override { record(std::format("rel {} {}", dx, dy)); }
    void button(std::uint32_t code, bool pressed) override
    {
        record(std::format("button {} {}", code, pressed ? "down" : "up"));
    }
    void scroll_discrete(std::int32_t x, std::int32_t y) override { record(std::format("scroll {} {}", x, y)); }
    void text(char32_t /*codepoint*/) override {}
    void flush() override { record("flush"); }
    [[nodiscard]] bool accepts_touch() const override { return touch_; }
    void touch_down(std::uint32_t slot, double x, double y) override
    {
        record(std::format("touch down {} {} {}", slot, x, y));
    }
    void touch_motion(std::uint32_t slot, double x, double y) override
    {
        record(std::format("touch motion {} {} {}", slot, x, y));
    }
    void touch_up(std::uint32_t slot) override { record(std::format("touch up {}", slot)); }
    void touch_cancel(std::uint32_t slot) override { record(std::format("touch cancel {}", slot)); }

    Calls take() { return std::exchange(calls_, {}); }

private:
    void record(std::string call) { calls_.push_back(std::move(call)); }

    bool touch_;
    Calls calls_;
};

void send(DynamicChannels& channels, const dyn::ClientPdu& pdu)
{
    for (const auto& chunk : svc::encode_chunks(dyn::encode_client_pdu(pdu))) {
        REQUIRE(channels.receive(chunk).has_value());
    }
}

rdpei::TouchContact tc(std::uint8_t id, std::int32_t x, std::int32_t y, std::uint32_t flags)
{
    return rdpei::TouchContact{.contact_id = id,
                               .x = x,
                               .y = y,
                               .contact_flags = flags,
                               .contact_rect = rdpei::ContactRect{-2, -2, 2, 2},
                               .orientation = std::nullopt,
                               .pressure = std::nullopt};
}

rdpei::PenContact pc(std::int32_t x, std::int32_t y, std::uint32_t flags, std::uint32_t pen_flags = 0)
{
    return rdpei::PenContact{.device_id = 0,
                             .x = x,
                             .y = y,
                             .contact_flags = flags,
                             .pen_flags = pen_flags,
                             .pressure = 300,
                             .rotation = std::nullopt,
                             .tilt_x = 10,
                             .tilt_y = -10};
}

/// A client desktop of 1000x500 on a 2000x1000 backend desktop.
struct Harness {
    explicit Harness(bool touch = true) : sink(touch)
    {
        translator.set_geometry(1000, 500, 2000, 1000);
        channels.start();
        static_cast<void>(client.take());
        send(channels, dyn::CapsResponse{dyn::version3});
        REQUIRE(std::holds_alternative<farland::channels::dvc_event::CapabilitiesReady>(channels.poll_event().value()));
        input.emplace(channels);
        const auto requests = client.take();
        REQUIRE(requests.size() == 1);
        const auto& request = std::get<dyn::CreateRequest>(requests[0]);
        CHECK(request.name == "Microsoft::Windows::RDS::Input");
        id = request.channel_id;
    }

    /// Accepts the channel and answers SC_READY.
    void open(std::uint16_t max_contacts = 10)
    {
        send(channels, dyn::CreateResponse{id, 0});
        CHECK(pump().empty());
        CHECK(client.take_input(id) == std::vector<rdpei::ServerPdu>{rdpei::ScReady{rdpei::version::v300, 0}});
        client_send(rdpei::CsReady{rdpei::cs_flags::show_touch_visuals, rdpei::version::v300, max_contacts});
        const auto events = pump();
        REQUIRE(events.size() == 1);
        REQUIRE(std::holds_alternative<rdpei::event::Ready>(events[0]));
        CHECK(input->ready());
    }

    void client_send(const rdpei::ClientPdu& pdu)
    {
        const Bytes message = rdpei::encode_client_pdu(pdu);
        send(channels, dyn::Data{id, message});
    }

    void touch(std::vector<rdpei::TouchContact> contacts)
    {
        client_send(rdpei::TouchEvent{0, {rdpei::TouchFrame{0, std::move(contacts)}}});
    }

    void pen(rdpei::PenContact contact) { client_send(rdpei::PenEvent{0, {rdpei::PenFrame{0, {contact}}}}); }

    /// Hands the DVC events to TouchInput and its frames to the translator,
    /// as the session does; returns the other events.
    std::vector<TouchEvent> pump()
    {
        while (auto event = channels.poll_event()) {
            CHECK(input->handle(*event));
        }
        std::vector<TouchEvent> others;
        while (auto event = input->poll_event()) {
            if (const auto* frame = std::get_if<rdpei::event::Frame>(&*event)) {
                translator.translate(frame->contacts);
            } else {
                others.push_back(std::move(*event));
            }
        }
        return others;
    }

    Calls calls()
    {
        CHECK(pump().empty());
        return sink.take();
    }

    Client client;
    DynamicChannels channels{client.sink()};
    RecordingSink sink;
    farland::platform::InputTranslator translator{sink};
    std::optional<TouchInput> input;
    std::uint32_t id = 0;
};

}  // namespace

TEST_CASE("Touch input end to end: fingers become touches on the desktop")
{
    Harness h;
    h.open();

    h.touch({tc(0, 100, 50, down)});
    CHECK(h.calls() == Calls{"touch down 0 200 100", "flush"});
    h.touch({tc(0, 110, 60, update), tc(1, 500, 250, down)});
    CHECK(h.calls() == Calls{"touch motion 0 220 120", "touch down 1 1000 500", "flush"});
    h.touch({tc(0, 110, 60, cf::up), tc(1, 999, 499, update)});
    CHECK(h.calls() == Calls{"touch up 0", "touch motion 1 1998 998", "flush"});
    h.touch({tc(1, 999, 499, cf::up | cf::canceled)});
    CHECK(h.calls() == Calls{"touch cancel 1", "flush"});

    SECTION("contacts outside the client desktop are clamped into it")
    {
        h.touch({tc(2, -50, 9000, down)});
        CHECK(h.calls() == Calls{"touch down 2 0 999", "flush"});
    }

    SECTION("a broken transaction is canceled, and the rest of it ignored")
    {
        h.touch({tc(2, 10, 10, down)});
        h.touch({tc(2, 20, 20, down)});
        h.touch({tc(2, 30, 30, update)});
        CHECK(h.calls() == Calls{"touch down 2 20 20", "flush", "touch cancel 2", "flush"});
    }

    SECTION("a malformed message is ignored and the channel goes on")
    {
        const Bytes garbage{std::byte{0x03}, std::byte{0x00}, std::byte{0x07}, std::byte{0x00}};
        send(h.channels, dyn::Data{h.id, garbage});
        CHECK(h.calls().empty());
        h.touch({tc(3, 1, 1, down)});
        CHECK(h.calls() == Calls{"touch down 3 2 2", "flush"});
    }

    SECTION("the translator cancels what is still down at the end of the session")
    {
        h.touch({tc(4, 1, 1, down), tc(5, 2, 2, down)});
        static_cast<void>(h.calls());
        h.translator.release_all();
        CHECK(h.sink.take() == Calls{"touch cancel 4", "touch cancel 5", "flush"});
    }
}

TEST_CASE("Touch input end to end: the first finger drives the pointer without a touchscreen")
{
    Harness h(false);
    h.open();
    h.touch({tc(0, 100, 50, down)});
    h.touch({tc(0, 110, 50, update), tc(1, 300, 300, down)});  // the second finger is dropped
    h.touch({tc(1, 310, 300, update)});
    h.touch({tc(0, 110, 50, cf::up)});
    CHECK(h.calls() ==
          Calls{"abs 200 100", "button 272 down", "flush", "abs 220 100", "flush", "button 272 up", "flush"});
    h.touch({tc(1, 310, 300, cf::up)});
    h.touch({tc(2, 10, 10, down)});
    CHECK(h.calls() == Calls{"abs 20 20", "button 272 down", "flush"});
}

TEST_CASE("Touch input end to end: the pen drives the pointer and a button")
{
    Harness h;
    h.open();
    h.pen(pc(10, 10, hover));
    h.pen(pc(11, 11, down));
    h.pen(pc(12, 12, update));
    h.pen(pc(12, 12, cf::up | cf::in_range));
    h.pen(pc(13, 13, down, rdpei::pen_flags::barrel_pressed));
    h.pen(pc(13, 13, cf::up | cf::canceled));
    h.pen(pc(13, 13, hover));
    h.pen(pc(13, 13, cf::update));
    CHECK(h.calls() == Calls{"abs 20 20", "flush", "abs 22 22", "button 272 down", "flush", "abs 24 24", "flush",
                             "button 272 up", "flush", "abs 26 26", "button 273 down", "flush", "button 273 up",
                             "flush", "abs 26 26", "flush"});
}

TEST_CASE("Touch input end to end: the channel goes away")
{
    SECTION("refused by a client without a digitizer")
    {
        Harness h;
        send(h.channels, dyn::CreateResponse{h.id, static_cast<std::int32_t>(0x80004005)});
        const auto events = h.pump();
        REQUIRE(events.size() == 1);
        CHECK(std::holds_alternative<te::Closed>(events[0]));
        CHECK(h.input->closed());
    }

    SECTION("closed by the client with a finger down: the finger is canceled")
    {
        Harness h;
        h.open();
        h.touch({tc(0, 100, 50, down)});
        static_cast<void>(h.calls());
        send(h.channels, dyn::Close{h.id});
        const auto events = h.pump();
        REQUIRE(events.size() == 1);
        CHECK(std::holds_alternative<te::Closed>(events[0]));
        CHECK(h.sink.take() == Calls{"touch cancel 0", "flush"});
    }

    SECTION("suspended by the server: fingers are canceled and SUSPEND_INPUT goes out")
    {
        Harness h;
        h.open();
        h.touch({tc(0, 100, 50, down)});
        static_cast<void>(h.calls());
        h.input->suspend();
        CHECK(h.client.take_input(h.id) == std::vector<rdpei::ServerPdu>{rdpei::SuspendInput{}});
        CHECK(h.calls() == Calls{"touch cancel 0", "flush"});
        h.input->resume();
        CHECK(h.client.take_input(h.id) == std::vector<rdpei::ServerPdu>{rdpei::ResumeInput{}});
    }
}
