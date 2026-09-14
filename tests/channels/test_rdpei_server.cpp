// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/channels/rdpei_server.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <variant>
#include <vector>

namespace rdpei = farland::channels::rdpei;
using rdpei::Contact;
using rdpei::ContactAction;
using rdpei::ContactKind;
using rdpei::RdpeiServer;

namespace {

using Bytes = std::vector<std::byte>;
namespace cf = rdpei::contact_flags;

constexpr std::uint32_t down = cf::down | cf::in_range | cf::in_contact;
constexpr std::uint32_t update = cf::update | cf::in_range | cf::in_contact;
constexpr std::uint32_t hover = cf::update | cf::in_range;
constexpr std::uint32_t up_to_hover = cf::up | cf::in_range;
constexpr std::uint32_t up = cf::up;
constexpr std::uint32_t canceled = cf::up | cf::canceled;
constexpr std::uint32_t out_of_range = cf::update;

rdpei::TouchContact tc(std::uint8_t id, std::int32_t x, std::int32_t y, std::uint32_t flags)
{
    return rdpei::TouchContact{.contact_id = id,
                               .x = x,
                               .y = y,
                               .contact_flags = flags,
                               .contact_rect = std::nullopt,
                               .orientation = std::nullopt,
                               .pressure = std::nullopt};
}

rdpei::PenContact pc(std::uint8_t device, std::int32_t x, std::int32_t y, std::uint32_t flags,
                     std::optional<std::uint32_t> pen_flags = std::nullopt)
{
    return rdpei::PenContact{.device_id = device,
                             .x = x,
                             .y = y,
                             .contact_flags = flags,
                             .pen_flags = pen_flags,
                             .pressure = std::nullopt,
                             .rotation = std::nullopt,
                             .tilt_x = std::nullopt,
                             .tilt_y = std::nullopt};
}

/// One touch frame with `contacts`.
Bytes touch(std::vector<rdpei::TouchContact> contacts)
{
    return rdpei::encode_client_pdu(rdpei::TouchEvent{0, {rdpei::TouchFrame{0, std::move(contacts)}}});
}

Bytes pen(std::vector<rdpei::PenContact> contacts)
{
    return rdpei::encode_client_pdu(rdpei::PenEvent{0, {rdpei::PenFrame{0, std::move(contacts)}}});
}

Bytes cs_ready(std::uint16_t max_contacts = 10, std::uint32_t flags = 0)
{
    return rdpei::encode_client_pdu(rdpei::CsReady{flags, rdpei::version::v300, max_contacts});
}

void feed(RdpeiServer& server, const Bytes& message)
{
    const auto result = server.receive(message);
    INFO((result.has_value() ? "" : result.error().message()));
    REQUIRE(result.has_value());
}

std::vector<rdpei::Event> events(RdpeiServer& server)
{
    std::vector<rdpei::Event> out;
    while (auto event = server.poll_event()) {
        out.push_back(std::move(*event));
    }
    return out;
}

/// The frames reported since the last call.
std::vector<std::vector<Contact>> frames(RdpeiServer& server)
{
    std::vector<std::vector<Contact>> out;
    for (auto& event : events(server)) {
        auto* frame = std::get_if<rdpei::event::Frame>(&event);
        REQUIRE(frame != nullptr);
        out.push_back(std::move(frame->contacts));
    }
    return out;
}

Contact touch_action(ContactAction action, std::uint8_t id, std::int32_t x, std::int32_t y)
{
    return Contact{.kind = ContactKind::touch,
                   .action = action,
                   .id = id,
                   .x = x,
                   .y = y,
                   .pressure = std::nullopt,
                   .pen_flags = 0};
}

Contact pen_action(ContactAction action, std::int32_t x, std::int32_t y, std::uint32_t flags = 0, std::uint8_t id = 0)
{
    return Contact{.kind = ContactKind::pen,
                   .action = action,
                   .id = id,
                   .x = x,
                   .y = y,
                   .pressure = std::nullopt,
                   .pen_flags = flags};
}

/// A server that sent SC_READY and got CS_READY.
RdpeiServer ready_server(rdpei::RdpeiServerConfig config = {}, std::uint16_t max_contacts = 10, std::uint32_t flags = 0)
{
    RdpeiServer server(config);
    server.start();
    static_cast<void>(server.take_output());
    feed(server, cs_ready(max_contacts, flags));
    static_cast<void>(events(server));
    return server;
}

}  // namespace

TEST_CASE("RdpeiServer sends SC_READY once")
{
    RdpeiServer server;
    CHECK_FALSE(server.ready());
    server.start();
    CHECK(server.take_output() ==
          std::vector<Bytes>{rdpei::encode_server_pdu(rdpei::ScReady{rdpei::version::v300, 0})});
    server.start();
    CHECK(server.take_output().empty());

    RdpeiServer v200({.protocol_version = rdpei::version::v200, .supported_features = 1, .max_touch_contacts = 32});
    v200.start();
    CHECK(v200.take_output() ==
          std::vector<Bytes>{rdpei::encode_server_pdu(rdpei::ScReady{rdpei::version::v200, std::nullopt})});
}

TEST_CASE("RdpeiServer becomes ready with CS_READY and bounds the contacts")
{
    RdpeiServer server;
    server.start();
    // Contacts before CS_READY are ignored.
    feed(server, touch({tc(0, 1, 1, down)}));
    CHECK(events(server).empty());
    CHECK(server.ignored_pdus() == 1);

    const auto flags = rdpei::cs_flags::show_touch_visuals;
    feed(server, cs_ready(100, flags));
    auto all = events(server);
    REQUIRE(all.size() == 1);
    const auto& ready = std::get<rdpei::event::Ready>(all[0]);
    CHECK(ready.client.flags == flags);
    CHECK(ready.client.max_touch_contacts == 100);
    CHECK(ready.max_touch_contacts == 32);  // farland's limit
    CHECK(ready.pen);
    CHECK_FALSE(ready.multipen);
    CHECK(server.ready());

    // A client that says 0 still gets one contact.
    feed(server, cs_ready(0));
    all = events(server);
    REQUIRE(all.size() == 1);
    CHECK(std::get<rdpei::event::Ready>(all[0]).max_touch_contacts == 1);
}

TEST_CASE("RdpeiServer ignores CS_READY before it sent SC_READY")
{
    RdpeiServer server;
    feed(server, cs_ready());
    CHECK(events(server).empty());
    CHECK_FALSE(server.ready());
}

TEST_CASE("RdpeiServer follows a touch from down to up")
{
    auto server = ready_server();
    feed(server, touch({tc(3, 10, 20, down)}));
    feed(server, touch({tc(3, 11, 21, update)}));
    CHECK(server.active_contacts(ContactKind::touch) == 1);
    feed(server, touch({tc(3, 11, 21, up)}));
    CHECK(frames(server) == std::vector<std::vector<Contact>>{{touch_action(ContactAction::down, 3, 10, 20)},
                                                              {touch_action(ContactAction::move, 3, 11, 21)},
                                                              {touch_action(ContactAction::up, 3, 11, 21)}});
    CHECK(server.active_contacts(ContactKind::touch) == 0);
    CHECK(server.violations() == 0);
}

TEST_CASE("RdpeiServer reports each frame of a PDU, with every contact of the frame")
{
    auto server = ready_server();
    rdpei::TouchEvent event{.encode_time = 5, .frames = {}};
    event.frames.push_back(rdpei::TouchFrame{0, {tc(0, 1, 1, down), tc(1, 2, 2, down)}});
    event.frames.push_back(rdpei::TouchFrame{1000, {tc(0, 3, 3, update), tc(1, 4, 4, canceled)}});
    feed(server, rdpei::encode_client_pdu(event));
    CHECK(frames(server) ==
          std::vector<std::vector<Contact>>{
              {touch_action(ContactAction::down, 0, 1, 1), touch_action(ContactAction::down, 1, 2, 2)},
              {touch_action(ContactAction::move, 0, 3, 3), touch_action(ContactAction::cancel, 1, 4, 4)}});
}

TEST_CASE("RdpeiServer tracks hovering contacts")
{
    auto server = ready_server();
    feed(server, touch({tc(0, 5, 5, hover)}));
    feed(server, touch({tc(0, 6, 6, down)}));
    feed(server, touch({tc(0, 6, 6, up_to_hover)}));
    feed(server, touch({tc(0, 7, 7, hover)}));
    feed(server, touch({tc(0, 7, 7, out_of_range)}));
    CHECK(frames(server) == std::vector<std::vector<Contact>>{{touch_action(ContactAction::hover, 0, 5, 5)},
                                                              {touch_action(ContactAction::down, 0, 6, 6)},
                                                              {touch_action(ContactAction::up, 0, 6, 6)},
                                                              {touch_action(ContactAction::hover, 0, 7, 7)},
                                                              {touch_action(ContactAction::leave, 0, 7, 7)}});

    SECTION("DISMISS_HOVERING_TOUCH_CONTACT ends a hovering contact only")
    {
        feed(server, touch({tc(1, 8, 8, hover), tc(2, 9, 9, down)}));
        static_cast<void>(events(server));
        feed(server, rdpei::encode_client_pdu(rdpei::DismissHoveringContact{1}));
        feed(server, rdpei::encode_client_pdu(rdpei::DismissHoveringContact{2}));
        feed(server, rdpei::encode_client_pdu(rdpei::DismissHoveringContact{200}));
        CHECK(frames(server) == std::vector<std::vector<Contact>>{{touch_action(ContactAction::leave, 1, 8, 8)}});
        CHECK(server.active_contacts(ContactKind::touch) == 1);
    }
}

TEST_CASE("RdpeiServer cancels contacts that break the state machine")
{
    auto server = ready_server();

    SECTION("an update of a contact that never came down is ignored")
    {
        feed(server, touch({tc(0, 1, 1, update)}));
        feed(server, touch({tc(0, 1, 1, up)}));
        CHECK(frames(server).empty());
        CHECK(server.violations() == 2);
    }

    SECTION("a second down cancels the contact, and its transaction is ignored until a new down")
    {
        feed(server, touch({tc(0, 1, 1, down)}));
        feed(server, touch({tc(0, 2, 2, down)}));
        feed(server, touch({tc(0, 3, 3, update)}));
        feed(server, touch({tc(0, 3, 3, up)}));
        feed(server, touch({tc(0, 4, 4, down)}));
        CHECK(frames(server) == std::vector<std::vector<Contact>>{{touch_action(ContactAction::down, 0, 1, 1)},
                                                                  {touch_action(ContactAction::cancel, 0, 1, 1)},
                                                                  {touch_action(ContactAction::down, 0, 4, 4)}});
        CHECK(server.violations() == 3);
    }

    SECTION("combinations [MS-RDPEI] 2.2.3.3.1.1 does not allow")
    {
        feed(server, touch({tc(0, 1, 1, down)}));
        feed(server, touch({tc(0, 1, 1, cf::down | cf::update | cf::in_range | cf::in_contact)}));
        feed(server, touch({tc(1, 1, 1, cf::down)}));
        feed(server, touch({tc(2, 1, 1, 0)}));
        feed(server, touch({tc(3, 1, 1, cf::in_range | cf::in_contact | 0x40)}));
        CHECK(frames(server) == std::vector<std::vector<Contact>>{{touch_action(ContactAction::down, 0, 1, 1)},
                                                                  {touch_action(ContactAction::cancel, 0, 1, 1)}});
        CHECK(server.violations() == 4);
    }

    SECTION("a hovering contact that breaks the rules leaves")
    {
        feed(server, touch({tc(0, 1, 1, hover)}));
        feed(server, touch({tc(0, 2, 2, update)}));
        CHECK(frames(server) == std::vector<std::vector<Contact>>{{touch_action(ContactAction::hover, 0, 1, 1)},
                                                                  {touch_action(ContactAction::leave, 0, 1, 1)}});
    }
}

TEST_CASE("RdpeiServer drops new contacts beyond the limit")
{
    auto server = ready_server({}, 2);
    feed(server, touch({tc(0, 0, 0, down), tc(1, 1, 1, down), tc(2, 2, 2, down)}));
    feed(server, touch({tc(2, 3, 3, update)}));
    CHECK(frames(server) == std::vector<std::vector<Contact>>{{touch_action(ContactAction::down, 0, 0, 0),
                                                               touch_action(ContactAction::down, 1, 1, 1)}});
    CHECK(server.violations() == 2);

    feed(server, touch({tc(0, 0, 0, up), tc(2, 5, 5, down)}));
    CHECK(frames(server) == std::vector<std::vector<Contact>>{{touch_action(ContactAction::up, 0, 0, 0),
                                                               touch_action(ContactAction::down, 2, 5, 5)}});
    CHECK(server.active_contacts(ContactKind::touch) == 2);
}

TEST_CASE("RdpeiServer releases every contact")
{
    auto server = ready_server();
    feed(server, touch({tc(0, 1, 1, down), tc(1, 2, 2, hover)}));
    feed(server, pen({pc(0, 3, 3, down)}));
    static_cast<void>(events(server));

    SECTION("on request")
    {
        server.release_all();
        CHECK(frames(server) == std::vector<std::vector<Contact>>{{touch_action(ContactAction::cancel, 0, 1, 1),
                                                                   touch_action(ContactAction::leave, 1, 2, 2),
                                                                   pen_action(ContactAction::cancel, 3, 3)}});
        server.release_all();
        CHECK(events(server).empty());
    }

    SECTION("when suspending, and ignores input until resumed")
    {
        server.suspend();
        CHECK(server.take_output() == std::vector<Bytes>{rdpei::encode_server_pdu(rdpei::SuspendInput{})});
        CHECK(frames(server).size() == 1);
        CHECK(server.suspended());
        server.suspend();
        CHECK(server.take_output().empty());

        feed(server, touch({tc(0, 1, 1, down)}));
        CHECK(events(server).empty());
        CHECK(server.ignored_pdus() == 1);

        server.resume();
        CHECK(server.take_output() == std::vector<Bytes>{rdpei::encode_server_pdu(rdpei::ResumeInput{})});
        server.resume();
        CHECK(server.take_output().empty());
        feed(server, touch({tc(0, 1, 1, down)}));
        CHECK(frames(server) == std::vector<std::vector<Contact>>{{touch_action(ContactAction::down, 0, 1, 1)}});
    }

    SECTION("when the client starts over with CS_READY")
    {
        feed(server, cs_ready());
        auto all = events(server);
        REQUIRE(all.size() == 2);
        CHECK(std::get<rdpei::event::Frame>(all[0]).contacts.size() == 3);
        CHECK(std::holds_alternative<rdpei::event::Ready>(all[1]));
        CHECK(server.active_contacts(ContactKind::touch) == 0);
    }
}

TEST_CASE("RdpeiServer follows the pen")
{
    SECTION("hover, down with the barrel button, up, leave, with pressure capped")
    {
        auto server = ready_server();
        auto touching = pc(0, 2, 2, down, rdpei::pen_flags::barrel_pressed);
        touching.pressure = 5000;
        feed(server, pen({pc(0, 1, 1, hover)}));
        feed(server, pen({touching}));
        feed(server, pen({pc(0, 2, 2, up_to_hover)}));
        feed(server, pen({pc(0, 2, 2, out_of_range)}));
        auto pressed = pen_action(ContactAction::down, 2, 2, rdpei::pen_flags::barrel_pressed);
        pressed.pressure = 1024;
        CHECK(frames(server) == std::vector<std::vector<Contact>>{{pen_action(ContactAction::hover, 1, 1)},
                                                                  {pressed},
                                                                  {pen_action(ContactAction::up, 2, 2)},
                                                                  {pen_action(ContactAction::leave, 2, 2)}});
    }

    SECTION("only device 0 without multipen")
    {
        auto server = ready_server();
        feed(server, pen({pc(1, 1, 1, down)}));
        CHECK(frames(server).empty());
        CHECK(server.violations() == 1);
    }

    SECTION("up to four pens with multipen")
    {
        auto server = ready_server({.protocol_version = rdpei::version::v300,
                                    .supported_features = rdpei::sc_features::multipen_injection_supported,
                                    .max_touch_contacts = 32},
                                   10, rdpei::cs_flags::enable_multipen_injection);
        rdpei::PenEvent event{.encode_time = 0, .frames = {}};
        event.frames.push_back(rdpei::PenFrame{0, {pc(0, 0, 0, down), pc(9, 1, 1, down), pc(2, 2, 2, down)}});
        event.frames.push_back(rdpei::PenFrame{0, {pc(3, 3, 3, down), pc(4, 4, 4, down)}});
        feed(server, rdpei::encode_client_pdu(event));
        const auto all = frames(server);
        REQUIRE(all.size() == 2);
        CHECK(all[0].size() == 3);
        CHECK(all[1] == std::vector<Contact>{pen_action(ContactAction::down, 3, 3, 0, 3)});
        CHECK(server.active_contacts(ContactKind::pen) == 4);
    }

    SECTION("not at all when the server offers version 1.0")
    {
        auto server =
            ready_server({.protocol_version = rdpei::version::v100, .supported_features = 0, .max_touch_contacts = 32});
        feed(server, pen({pc(0, 1, 1, down)}));
        CHECK(events(server).empty());
        CHECK(server.ignored_pdus() == 1);
    }
}

TEST_CASE("RdpeiServer takes several PDUs in a message, and nothing from a malformed one")
{
    auto server = ready_server();
    auto both = touch({tc(0, 1, 1, down)});
    const auto second = touch({tc(0, 2, 2, update)});
    both.insert(both.end(), second.begin(), second.end());
    feed(server, both);
    CHECK(frames(server).size() == 2);

    auto broken = touch({tc(0, 3, 3, update)});
    broken.push_back(std::byte{0x03});  // the start of a second header
    CHECK_FALSE(server.receive(broken).has_value());
    CHECK(events(server).empty());
    feed(server, touch({tc(0, 4, 4, update)}));
    CHECK(frames(server) == std::vector<std::vector<Contact>>{{touch_action(ContactAction::move, 0, 4, 4)}});
}
