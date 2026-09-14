// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/channels/rdpei.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <variant>
#include <vector>

/// The server side of the Input Virtual Channel Extension ([MS-RDPEI] 3.2)
/// as a sans-IO state machine: the SC_READY/CS_READY exchange, suspend and
/// resume, and the lifetime of every touch and pen contact ([MS-RDPEI]
/// 3.1.1.1), turned into validated contact actions.
///
/// Contract for callers:
/// - Call `start()` once the channel is open, then send every message from
///   `take_output()` on it, after every call.
/// - Feed each reassembled channel message to `receive()`. A message may hold
///   several PDUs back to back. An error means a malformed message; nothing
///   from it was applied, and the channel can go on ([MS-RDPEI] 3.1.5.1:
///   such messages SHOULD be ignored).
/// - Handle the events from `poll_event()`: one `Frame` per touch or pen
///   frame that changed something.
///
/// Decided here where [MS-RDPEI] leaves room:
/// - Touch and pen PDUs before CS_READY, while suspended, and pen PDUs when
///   the configured version has no pen (below 2.0) are ignored.
/// - A second CS_READY starts over: every contact is released first.
/// - A contact that breaks the state machine of 3.1.1.1 (an invalid flag
///   combination or transition, a pen device id other than 0 without
///   multipen, a new contact beyond the limit) is canceled if engaged, and
///   stays out of range, so the rest of its transaction is ignored until the
///   client starts a new one with a down or hover (3.2.5.3, 3.2.5.7).
/// - frameOffset and encodeTime are not used: frames are injected as they
///   arrive.
namespace farland::channels::rdpei {

struct RdpeiServerConfig {
    /// Sent in SC_READY; pen input needs version 2.0 or later.
    std::uint32_t protocol_version = version::v300;
    /// Sent in SC_READY from version 3.0 on.
    std::uint32_t supported_features = 0;
    /// farland limit on active touch contacts, whatever the client's
    /// maxTouchContacts says.
    std::size_t max_touch_contacts = 32;
};

enum class ContactKind : std::uint8_t { touch, pen };

/// What happened to a contact, from the states of [MS-RDPEI] 3.1.1.1.
enum class ContactAction : std::uint8_t {
    down,    ///< out of range or hovering → engaged
    move,    ///< engaged → engaged
    up,      ///< engaged → hovering or out of range
    cancel,  ///< engaged → out of range, canceled (by the client, a violation or a release)
    hover,   ///< out of range or hovering → hovering
    leave,   ///< hovering → out of range
};

/// One validated contact change, in client desktop pixels relative to the
/// virtual-desktop origin.
struct Contact {
    ContactKind kind = ContactKind::touch;
    ContactAction action = ContactAction::move;
    /// The touch contactId or the pen deviceId.
    std::uint8_t id = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    /// 0-1024, when the client sent it.
    std::optional<std::uint32_t> pressure;
    /// The pen's penFlags (pen_flags::*), 0 when absent.
    std::uint32_t pen_flags = 0;

    friend bool operator==(const Contact&, const Contact&) = default;
};

namespace event {
/// The client sent CS_READY: contacts may follow. Comes again when it
/// starts over.
struct Ready {
    CsReady client;
    /// Active touch contacts allowed: the client's maxTouchContacts (at least
    /// 1) up to RdpeiServerConfig::max_touch_contacts.
    std::size_t max_touch_contacts = 0;
    bool pen = false;
    bool multipen = false;
};
/// The changes of one touch or pen frame, or of releasing everything.
struct Frame {
    std::vector<Contact> contacts;
};
}  // namespace event

using Event = std::variant<event::Ready, event::Frame>;

class RdpeiServer {
public:
    explicit RdpeiServer(RdpeiServerConfig config = {});

    /// Queues SC_READY ([MS-RDPEI] 3.2.3). Once; later calls do nothing.
    void start();
    [[nodiscard]] Result<void> receive(std::span<const std::byte> message);

    /// Queues SUSPEND_INPUT and releases every contact ([MS-RDPEI] 3.2.5.4).
    /// Only once ready and not suspended.
    void suspend();
    /// Queues RESUME_INPUT if suspended ([MS-RDPEI] 3.2.5.5).
    void resume();
    /// Cancels every engaged contact and lets every hovering one leave, as
    /// one Frame: for a closed channel, a disconnection or a suspension.
    void release_all();

    [[nodiscard]] std::vector<std::vector<std::byte>> take_output();
    [[nodiscard]] std::optional<Event> poll_event();

    [[nodiscard]] bool ready() const noexcept { return state_ == State::ready; }
    [[nodiscard]] bool suspended() const noexcept { return suspended_; }
    [[nodiscard]] std::size_t active_contacts(ContactKind kind) const noexcept;
    /// Contacts that broke the state machine or the limits, and touch or pen
    /// PDUs that were ignored, since the start.
    [[nodiscard]] std::uint64_t violations() const noexcept { return violations_; }
    [[nodiscard]] std::uint64_t ignored_pdus() const noexcept { return ignored_; }

private:
    enum class State : std::uint8_t { idle, waiting_cs_ready, ready };
    enum class Phase : std::uint8_t { out_of_range, hovering, engaged };

    struct Slot {
        Phase phase = Phase::out_of_range;
        std::int32_t x = 0;
        std::int32_t y = 0;
    };
    using Slots = std::array<Slot, 256>;

    void apply(const ClientPdu& pdu);
    void apply(const CsReady& pdu);
    void apply(const TouchEvent& pdu);
    void apply(const PenEvent& pdu);
    void apply(const DismissHoveringContact& pdu);
    /// Runs one contact through the state machine and appends what it did.
    void step(Contact contact, std::uint32_t flags, std::vector<Contact>& out);
    void emit(std::vector<Contact> contacts);

    [[nodiscard]] Slots& slots(ContactKind kind) noexcept { return kind == ContactKind::touch ? touch_ : pen_; }
    [[nodiscard]] std::size_t limit(ContactKind kind) const noexcept;

    RdpeiServerConfig config_;
    State state_ = State::idle;
    bool suspended_ = false;
    std::size_t max_touch_ = 0;
    bool multipen_ = false;
    Slots touch_{};
    Slots pen_{};
    std::vector<std::vector<std::byte>> output_;
    std::deque<Event> events_;
    std::uint64_t violations_ = 0;
    std::uint64_t ignored_ = 0;
};

}  // namespace farland::channels::rdpei
