// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/channels/rdpei_server.hpp>

#include <algorithm>
#include <utility>

namespace farland::channels::rdpei {

namespace {

namespace cf = contact_flags;

/// Pens injected at once with multipen ([MS-RDPEI] 2.2.3.1).
constexpr std::size_t max_pens = 4;
constexpr std::uint32_t max_pressure = 1024;

}  // namespace

RdpeiServer::RdpeiServer(RdpeiServerConfig config) : config_(config) {}

void RdpeiServer::start()
{
    if (state_ != State::idle) {
        return;
    }
    ScReady ready{.protocol_version = config_.protocol_version, .supported_features = std::nullopt};
    if (config_.protocol_version >= version::v300) {
        ready.supported_features = config_.supported_features;
    }
    output_.push_back(encode_server_pdu(ready));
    state_ = State::waiting_cs_ready;
}

Result<void> RdpeiServer::receive(std::span<const std::byte> message)
{
    // Everything is decoded before anything is applied.
    std::vector<ClientPdu> pdus;
    while (!message.empty()) {
        FARLAND_TRY(const auto size, frame_pdu(message));
        FARLAND_TRY(auto pdu, decode_client_pdu(message.first(size)));
        pdus.push_back(std::move(pdu));
        message = message.subspan(size);
    }
    for (const auto& pdu : pdus) {
        apply(pdu);
    }
    return {};
}

void RdpeiServer::suspend()
{
    if (!ready() || suspended_) {
        return;
    }
    output_.push_back(encode_server_pdu(SuspendInput{}));
    suspended_ = true;
    release_all();
}

void RdpeiServer::resume()
{
    if (!suspended_) {
        return;
    }
    output_.push_back(encode_server_pdu(ResumeInput{}));
    suspended_ = false;
}

void RdpeiServer::release_all()
{
    std::vector<Contact> released;
    for (const auto kind : {ContactKind::touch, ContactKind::pen}) {
        auto& table = slots(kind);
        for (std::size_t id = 0; id < table.size(); ++id) {
            Slot& slot = table.at(id);
            if (slot.phase == Phase::out_of_range) {
                continue;
            }
            released.push_back(Contact{
                .kind = kind,
                .action = slot.phase == Phase::engaged ? ContactAction::cancel : ContactAction::leave,
                .id = static_cast<std::uint8_t>(id),
                .x = slot.x,
                .y = slot.y,
                .pressure = std::nullopt,
                .pen_flags = 0,
            });
            slot.phase = Phase::out_of_range;
        }
    }
    emit(std::move(released));
}

std::vector<std::vector<std::byte>> RdpeiServer::take_output()
{
    return std::exchange(output_, {});
}

std::optional<Event> RdpeiServer::poll_event()
{
    if (events_.empty()) {
        return std::nullopt;
    }
    Event event = std::move(events_.front());
    events_.pop_front();
    return event;
}

std::size_t RdpeiServer::active_contacts(ContactKind kind) const noexcept
{
    const auto& table = kind == ContactKind::touch ? touch_ : pen_;
    return static_cast<std::size_t>(
        std::ranges::count_if(table, [](const Slot& s) { return s.phase != Phase::out_of_range; }));
}

std::size_t RdpeiServer::limit(ContactKind kind) const noexcept
{
    if (kind == ContactKind::touch) {
        return max_touch_;
    }
    return multipen_ ? max_pens : 1;
}

void RdpeiServer::apply(const ClientPdu& pdu)
{
    std::visit([this](const auto& p) { apply(p); }, pdu);
}

void RdpeiServer::apply(const CsReady& pdu)
{
    if (state_ == State::idle) {
        ++ignored_;  // no SC_READY was sent
        return;
    }
    if (state_ == State::ready) {
        release_all();
    }
    state_ = State::ready;
    // A client without a touch digitizer may say 0 and still send pen input.
    max_touch_ =
        std::clamp<std::size_t>(pdu.max_touch_contacts, 1, std::max<std::size_t>(config_.max_touch_contacts, 1));
    multipen_ = (config_.supported_features & sc_features::multipen_injection_supported) != 0 &&
                (pdu.flags & cs_flags::enable_multipen_injection) != 0 && config_.protocol_version >= version::v300;
    events_.emplace_back(event::Ready{
        .client = pdu,
        .max_touch_contacts = max_touch_,
        .pen = config_.protocol_version >= version::v200,
        .multipen = multipen_,
    });
}

void RdpeiServer::apply(const TouchEvent& pdu)
{
    if (!ready() || suspended_) {
        ++ignored_;
        return;
    }
    for (const auto& frame : pdu.frames) {
        std::vector<Contact> changes;
        for (const auto& c : frame.contacts) {
            step(Contact{.kind = ContactKind::touch,
                         .action = ContactAction::move,
                         .id = c.contact_id,
                         .x = c.x,
                         .y = c.y,
                         .pressure = c.pressure ? std::optional(std::min(*c.pressure, max_pressure)) : std::nullopt,
                         .pen_flags = 0},
                 c.contact_flags, changes);
        }
        emit(std::move(changes));
    }
}

void RdpeiServer::apply(const PenEvent& pdu)
{
    if (!ready() || suspended_ || config_.protocol_version < version::v200) {
        ++ignored_;
        return;
    }
    for (const auto& frame : pdu.frames) {
        std::vector<Contact> changes;
        for (const auto& c : frame.contacts) {
            const Contact contact{
                .kind = ContactKind::pen,
                .action = ContactAction::move,
                .id = c.device_id,
                .x = c.x,
                .y = c.y,
                .pressure = c.pressure ? std::optional(std::min(*c.pressure, max_pressure)) : std::nullopt,
                .pen_flags = c.pen_flags.value_or(0),
            };
            if (c.device_id != 0 && !multipen_) {
                ++violations_;  // [MS-RDPEI] 2.2.3.7.1.1: deviceId MUST be 0
                continue;
            }
            step(contact, c.contact_flags, changes);
        }
        emit(std::move(changes));
    }
}

void RdpeiServer::apply(const DismissHoveringContact& pdu)
{
    if (!ready()) {
        ++ignored_;
        return;
    }
    // [MS-RDPEI] 3.2.5.6: only a hovering contact is affected.
    Slot& slot = touch_.at(pdu.contact_id);
    if (slot.phase != Phase::hovering) {
        return;
    }
    slot.phase = Phase::out_of_range;
    emit({Contact{.kind = ContactKind::touch,
                  .action = ContactAction::leave,
                  .id = pdu.contact_id,
                  .x = slot.x,
                  .y = slot.y,
                  .pressure = std::nullopt,
                  .pen_flags = 0}});
}

void RdpeiServer::step(Contact contact, std::uint32_t flags, std::vector<Contact>& out)
{
    Slot& slot = slots(contact.kind).at(contact.id);
    const Phase from = slot.phase;
    // The target phase and action for each combination [MS-RDPEI]
    // 2.2.3.3.1.1 allows, when the contact's current phase permits it.
    std::optional<std::pair<Phase, ContactAction>> next;
    switch (flags) {
    case cf::down | cf::in_range | cf::in_contact:
        if (from != Phase::engaged) {
            next = {Phase::engaged, ContactAction::down};
        }
        break;
    case cf::update | cf::in_range | cf::in_contact:
        if (from == Phase::engaged) {
            next = {Phase::engaged, ContactAction::move};
        }
        break;
    case cf::update | cf::in_range:
        if (from != Phase::engaged) {
            next = {Phase::hovering, ContactAction::hover};
        }
        break;
    case cf::up | cf::in_range:
        if (from == Phase::engaged) {
            next = {Phase::hovering, ContactAction::up};
        }
        break;
    case cf::up:
        if (from == Phase::engaged) {
            next = {Phase::out_of_range, ContactAction::up};
        }
        break;
    case cf::up | cf::canceled:
        if (from == Phase::engaged) {
            next = {Phase::out_of_range, ContactAction::cancel};
        }
        break;
    case cf::update:
    case cf::update | cf::canceled:
        if (from == Phase::hovering) {
            next = {Phase::out_of_range, ContactAction::leave};
        }
        break;
    default:
        break;
    }
    if (next && from == Phase::out_of_range && active_contacts(contact.kind) >= limit(contact.kind)) {
        next.reset();  // a new contact beyond the limit
    }
    if (!next) {
        ++violations_;
        if (from != Phase::out_of_range) {
            // Cancel the transaction where the contact last was.
            out.push_back(Contact{.kind = contact.kind,
                                  .action = from == Phase::engaged ? ContactAction::cancel : ContactAction::leave,
                                  .id = contact.id,
                                  .x = slot.x,
                                  .y = slot.y,
                                  .pressure = std::nullopt,
                                  .pen_flags = 0});
            slot.phase = Phase::out_of_range;
        }
        return;
    }
    slot.phase = next->first;
    slot.x = contact.x;
    slot.y = contact.y;
    contact.action = next->second;
    out.push_back(contact);
}

void RdpeiServer::emit(std::vector<Contact> contacts)
{
    if (!contacts.empty()) {
        events_.emplace_back(event::Frame{std::move(contacts)});
    }
}

}  // namespace farland::channels::rdpei
