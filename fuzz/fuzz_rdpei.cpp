// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Input Virtual Channel messages ([MS-RDPEI]) into the server. Input: one
// byte of options (bit 0: offer multipen, bit 1: offer only version 1.0),
// then records of [u16le length][channel message]; a record of length 0
// suspends or resumes. Every PDU that decodes must survive re-encoding, and
// the contact actions the server reports must follow the contact state
// machine of [MS-RDPEI] 3.1.1.1 within its limits.

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/channels/rdpei.hpp>
#include <farland/channels/rdpei_server.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <variant>
#include <vector>

using namespace farland;
namespace rdpei = farland::channels::rdpei;

namespace {

enum class Phase : std::uint8_t { out, hovering, engaged };

/// What the server's actions say about each contact.
struct Mirror {
    std::array<Phase, 256> touch{};
    std::array<Phase, 256> pen{};

    void apply(const rdpei::Contact& c)
    {
        Phase& phase = (c.kind == rdpei::ContactKind::touch ? touch : pen).at(c.id);
        using A = rdpei::ContactAction;
        switch (c.action) {
        case A::down:
            FARLAND_ASSERT(phase != Phase::engaged);
            phase = Phase::engaged;
            break;
        case A::move:
            FARLAND_ASSERT(phase == Phase::engaged);
            break;
        case A::up:
            FARLAND_ASSERT(phase == Phase::engaged);
            phase = Phase::out;  // or hovering; a hover or leave tells
            break;
        case A::cancel:
            FARLAND_ASSERT(phase == Phase::engaged);
            phase = Phase::out;
            break;
        case A::hover:
            FARLAND_ASSERT(phase != Phase::engaged);
            phase = Phase::hovering;
            break;
        case A::leave:
            phase = Phase::out;
            break;
        }
        if (c.pressure) {
            FARLAND_ASSERT(*c.pressure <= 1024);
        }
    }
};

void check_round_trip(std::span<const std::byte> message)
{
    std::span<const std::byte> rest = message;
    while (!rest.empty()) {
        const auto size = rdpei::frame_pdu(rest);
        if (!size) {
            return;
        }
        const auto pdu = rdpei::decode_client_pdu(rest.first(*size));
        if (pdu) {
            const auto again = rdpei::decode_client_pdu(rdpei::encode_client_pdu(*pdu));
            FARLAND_ASSERT(again.has_value() && *again == *pdu);
        }
        rest = rest.subspan(*size);
    }
}

void drain(rdpei::RdpeiServer& server, Mirror& mirror, const rdpei::RdpeiServerConfig& config)
{
    for (const auto& message : server.take_output()) {
        FARLAND_ASSERT(rdpei::decode_server_pdu(message).has_value());
    }
    while (auto event = server.poll_event()) {
        if (const auto* frame = std::get_if<rdpei::event::Frame>(&*event)) {
            FARLAND_ASSERT(!frame->contacts.empty());
            for (const auto& contact : frame->contacts) {
                mirror.apply(contact);
            }
        } else if (const auto* ready = std::get_if<rdpei::event::Ready>(&*event)) {
            FARLAND_ASSERT(ready->max_touch_contacts >= 1 && ready->max_touch_contacts <= config.max_touch_contacts);
        }
    }
    FARLAND_ASSERT(server.active_contacts(rdpei::ContactKind::touch) <= config.max_touch_contacts);
    FARLAND_ASSERT(server.active_contacts(rdpei::ContactKind::pen) <= 4);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    Reader r(std::as_bytes(std::span(data, size)));
    const auto options = r.u8();
    if (!options) {
        return 0;
    }
    rdpei::RdpeiServerConfig config;
    config.max_touch_contacts = 10;
    if ((*options & 1U) != 0) {
        config.supported_features = rdpei::sc_features::multipen_injection_supported;
    }
    if ((*options & 2U) != 0) {
        config.protocol_version = rdpei::version::v100;
    }
    rdpei::RdpeiServer server(config);
    Mirror mirror;
    server.start();
    drain(server, mirror, config);
    while (!r.empty()) {
        const auto length = r.u16le();
        if (!length) {
            break;
        }
        if (*length == 0) {
            if (server.suspended()) {
                server.resume();
            } else {
                server.suspend();
            }
        } else {
            const auto message = r.bytes(std::min<std::size_t>(*length, r.remaining())).value();
            check_round_trip(message);
            static_cast<void>(server.receive(message));
        }
        drain(server, mirror, config);
    }
    server.release_all();
    drain(server, mirror, config);
    FARLAND_ASSERT(server.active_contacts(rdpei::ContactKind::touch) == 0);
    FARLAND_ASSERT(server.active_contacts(rdpei::ContactKind::pen) == 0);
    FARLAND_ASSERT(std::ranges::all_of(mirror.touch, [](Phase p) { return p == Phase::out; }));
    return 0;
}
