// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

/// PDU codecs for the Input Virtual Channel Extension ([MS-RDPEI] 2.2):
/// multitouch and pen frames on the dynamic virtual channel
/// "Microsoft::Windows::RDS::Input", for both directions.
///
/// Decoding is strict about structure: the variable-length integers, every
/// length and count, and the field presence flags must add up to exactly the
/// PDU's `pduLength`. Field values (contact flags, pressure, angles) are
/// passed on as sent; RdpeiServer judges them.
namespace farland::channels::rdpei {

/// [MS-RDPEI] 2.1. Sent without the terminating NUL here.
inline constexpr std::string_view channel_name = "Microsoft::Windows::RDS::Input";

/// RDPINPUT_HEADER, [MS-RDPEI] 2.2.2.6.
inline constexpr std::size_t header_size = 6;

/// RDPINPUT_HEADER eventId values, [MS-RDPEI] 2.2.2.6.
namespace event_id {
inline constexpr std::uint16_t sc_ready = 0x0001;
inline constexpr std::uint16_t cs_ready = 0x0002;
inline constexpr std::uint16_t touch = 0x0003;
inline constexpr std::uint16_t suspend_input = 0x0004;
inline constexpr std::uint16_t resume_input = 0x0005;
inline constexpr std::uint16_t dismiss_hovering_touch_contact = 0x0006;
inline constexpr std::uint16_t pen = 0x0008;
}  // namespace event_id

/// protocolVersion values, [MS-RDPEI] 2.2.3.1 and 2.2.3.2.
namespace version {
inline constexpr std::uint32_t v100 = 0x00010000;  ///< touch only
inline constexpr std::uint32_t v101 = 0x00010001;  ///< touch only
inline constexpr std::uint32_t v200 = 0x00020000;  ///< touch and pen
inline constexpr std::uint32_t v300 = 0x00030000;  ///< adds supportedFeatures to SC_READY
}  // namespace version

/// SC_READY supportedFeatures, [MS-RDPEI] 2.2.3.1.
namespace sc_features {
inline constexpr std::uint32_t multipen_injection_supported = 0x00000001;
}  // namespace sc_features

/// CS_READY flags, [MS-RDPEI] 2.2.3.2.
namespace cs_flags {
inline constexpr std::uint32_t show_touch_visuals = 0x00000001;
inline constexpr std::uint32_t disable_timestamp_injection = 0x00000002;
inline constexpr std::uint32_t enable_multipen_injection = 0x00000004;
}  // namespace cs_flags

/// RDPINPUT_TOUCH_CONTACT fieldsPresent, [MS-RDPEI] 2.2.3.3.1.1.
namespace touch_fields {
inline constexpr std::uint16_t contact_rect = 0x0001;
inline constexpr std::uint16_t orientation = 0x0002;
inline constexpr std::uint16_t pressure = 0x0004;
inline constexpr std::uint16_t all = contact_rect | orientation | pressure;
}  // namespace touch_fields

/// RDPINPUT_PEN_CONTACT fieldsPresent, [MS-RDPEI] 2.2.3.7.1.1.
namespace pen_fields {
inline constexpr std::uint16_t pen_flags = 0x0001;
inline constexpr std::uint16_t pressure = 0x0002;
inline constexpr std::uint16_t rotation = 0x0004;
inline constexpr std::uint16_t tilt_x = 0x0008;
inline constexpr std::uint16_t tilt_y = 0x0010;
inline constexpr std::uint16_t all = pen_flags | pressure | rotation | tilt_x | tilt_y;
}  // namespace pen_fields

/// contactFlags of touch and pen contacts, [MS-RDPEI] 2.2.3.3.1.1 and 2.2.3.7.1.1.
namespace contact_flags {
inline constexpr std::uint32_t down = 0x0001;
inline constexpr std::uint32_t update = 0x0002;
inline constexpr std::uint32_t up = 0x0004;
inline constexpr std::uint32_t in_range = 0x0008;
inline constexpr std::uint32_t in_contact = 0x0010;
inline constexpr std::uint32_t canceled = 0x0020;
}  // namespace contact_flags

/// penFlags, [MS-RDPEI] 2.2.3.7.1.1.
namespace pen_flags {
inline constexpr std::uint32_t barrel_pressed = 0x0001;
inline constexpr std::uint32_t eraser_pressed = 0x0002;
inline constexpr std::uint32_t inverted = 0x0004;
}  // namespace pen_flags

/// Value ranges of the variable-length integers, [MS-RDPEI] 2.2.2.
inline constexpr std::uint16_t max_two_byte_unsigned = 0x7FFF;
inline constexpr std::int16_t max_two_byte_signed = 0x3FFF;
inline constexpr std::uint32_t max_four_byte_unsigned = 0x3FFFFFFF;
inline constexpr std::int32_t max_four_byte_signed = 0x1FFFFFFF;
inline constexpr std::uint64_t max_eight_byte_unsigned = 0x1FFFFFFFFFFFFFFF;

/// farland limits. A client batches the frames it could not send yet into
/// one PDU, and every contact of a frame carries a distinct 8-bit id; up to
/// four pens report at once ([MS-RDPEI] 2.2.3.1).
inline constexpr std::size_t max_frames = 128;
inline constexpr std::size_t max_touch_contacts_per_frame = 256;
inline constexpr std::size_t max_pen_contacts_per_frame = 4;
/// Largest client PDU accepted; also the channel's reassembly limit.
inline constexpr std::size_t max_pdu_size = std::size_t{64} * 1024;

/// The variable-length integers of [MS-RDPEI] 2.2.2.1 to 2.2.2.5. Decoding
/// accepts values in more bytes than needed (FreeRDP sends 0x7F in two);
/// encoding uses the fewest. The encoders assert that the value is in range.
[[nodiscard]] Result<std::uint16_t> read_two_byte_unsigned(Reader& r);
[[nodiscard]] Result<std::int16_t> read_two_byte_signed(Reader& r);
[[nodiscard]] Result<std::uint32_t> read_four_byte_unsigned(Reader& r);
[[nodiscard]] Result<std::int32_t> read_four_byte_signed(Reader& r);
[[nodiscard]] Result<std::uint64_t> read_eight_byte_unsigned(Reader& r);
void write_two_byte_unsigned(Writer& w, std::uint16_t value);
void write_two_byte_signed(Writer& w, std::int16_t value);
void write_four_byte_unsigned(Writer& w, std::uint32_t value);
void write_four_byte_signed(Writer& w, std::int32_t value);
void write_eight_byte_unsigned(Writer& w, std::uint64_t value);

/// RDPINPUT_SC_READY_PDU, [MS-RDPEI] 2.2.3.1.
struct ScReady {
    std::uint32_t protocol_version = version::v300;
    /// Present from version 3.0 on (a SHOULD).
    std::optional<std::uint32_t> supported_features;

    friend bool operator==(const ScReady&, const ScReady&) = default;
};

/// RDPINPUT_CS_READY_PDU, [MS-RDPEI] 2.2.3.2.
struct CsReady {
    std::uint32_t flags = 0;
    std::uint32_t protocol_version = version::v100;
    std::uint16_t max_touch_contacts = 0;

    friend bool operator==(const CsReady&, const CsReady&) = default;
};

/// The contact rectangle, relative to the contact point, [MS-RDPEI] 2.2.3.3.1.1.
struct ContactRect {
    std::int16_t left = 0;
    std::int16_t top = 0;
    std::int16_t right = 0;
    std::int16_t bottom = 0;

    friend bool operator==(const ContactRect&, const ContactRect&) = default;
};

/// RDPINPUT_TOUCH_CONTACT, [MS-RDPEI] 2.2.3.3.1.1. The optional fields
/// are present exactly when set.
struct TouchContact {
    std::uint8_t contact_id = 0;
    /// Relative to the virtual-desktop origin, in client desktop pixels.
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t contact_flags = 0;
    std::optional<ContactRect> contact_rect;
    std::optional<std::uint32_t> orientation;  ///< degrees, 0-359
    std::optional<std::uint32_t> pressure;     ///< 0-1024

    friend bool operator==(const TouchContact&, const TouchContact&) = default;
};

/// RDPINPUT_TOUCH_FRAME, [MS-RDPEI] 2.2.3.3.1.
struct TouchFrame {
    /// Microseconds since the previous frame.
    std::uint64_t frame_offset = 0;
    std::vector<TouchContact> contacts;

    friend bool operator==(const TouchFrame&, const TouchFrame&) = default;
};

/// RDPINPUT_TOUCH_EVENT_PDU, [MS-RDPEI] 2.2.3.3.
struct TouchEvent {
    /// Milliseconds from the oldest frame to its encoding.
    std::uint32_t encode_time = 0;
    std::vector<TouchFrame> frames;  ///< oldest first

    friend bool operator==(const TouchEvent&, const TouchEvent&) = default;
};

/// RDPINPUT_SUSPEND_INPUT_PDU, [MS-RDPEI] 2.2.3.4.
struct SuspendInput {
    friend bool operator==(const SuspendInput&, const SuspendInput&) = default;
};

/// RDPINPUT_RESUME_INPUT_PDU, [MS-RDPEI] 2.2.3.5.
struct ResumeInput {
    friend bool operator==(const ResumeInput&, const ResumeInput&) = default;
};

/// RDPINPUT_DISMISS_HOVERING_TOUCH_CONTACT_PDU, [MS-RDPEI] 2.2.3.6.
struct DismissHoveringContact {
    std::uint8_t contact_id = 0;

    friend bool operator==(const DismissHoveringContact&, const DismissHoveringContact&) = default;
};

/// RDPINPUT_PEN_CONTACT, [MS-RDPEI] 2.2.3.7.1.1. The optional fields are
/// present exactly when set.
struct PenContact {
    /// 0 unless multipen injection was negotiated.
    std::uint8_t device_id = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t contact_flags = 0;
    std::optional<std::uint32_t> pen_flags;
    std::optional<std::uint32_t> pressure;  ///< 0-1024
    std::optional<std::uint16_t> rotation;  ///< degrees clockwise, 0-359
    std::optional<std::int16_t> tilt_x;     ///< degrees, -90 to 90, positive to the right
    std::optional<std::int16_t> tilt_y;     ///< degrees, -90 to 90, positive toward the user

    friend bool operator==(const PenContact&, const PenContact&) = default;
};

/// RDPINPUT_PEN_FRAME, [MS-RDPEI] 2.2.3.7.1.
struct PenFrame {
    std::uint64_t frame_offset = 0;
    std::vector<PenContact> contacts;

    friend bool operator==(const PenFrame&, const PenFrame&) = default;
};

/// RDPINPUT_PEN_EVENT_PDU, [MS-RDPEI] 2.2.3.7.
struct PenEvent {
    std::uint32_t encode_time = 0;
    std::vector<PenFrame> frames;  ///< oldest first

    friend bool operator==(const PenEvent&, const PenEvent&) = default;
};

using ClientPdu = std::variant<CsReady, TouchEvent, DismissHoveringContact, PenEvent>;
using ServerPdu = std::variant<ScReady, SuspendInput, ResumeInput>;

/// The size of the first PDU in `stream` (its `pduLength`), after checking
/// that it is at least a header, at most `max_size`, and complete.
[[nodiscard]] Result<std::size_t> frame_pdu(std::span<const std::byte> stream, std::size_t max_size = max_pdu_size);

/// One client-to-server PDU that fills `bytes` exactly. An eventId the
/// client does not send is Errc::unsupported.
[[nodiscard]] Result<ClientPdu> decode_client_pdu(std::span<const std::byte> bytes);
/// One server-to-client PDU that fills `bytes` exactly.
[[nodiscard]] Result<ServerPdu> decode_server_pdu(std::span<const std::byte> bytes);

/// Encoders assert that every value fits its encoding and that counts stay
/// within the limits above.
[[nodiscard]] std::vector<std::byte> encode_client_pdu(const ClientPdu& pdu);
[[nodiscard]] std::vector<std::byte> encode_server_pdu(const ServerPdu& pdu);

}  // namespace farland::channels::rdpei
