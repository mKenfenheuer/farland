// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>
#include <farland/proto/capabilities.hpp>
#include <farland/proto/input.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

/// Share Control and Share Data PDUs: capability exchange, connection
/// finalization and the slow-path data PDUs, [MS-RDPBCGR] 2.2.1.13 - 2.2.1.22,
/// 2.2.3, 2.2.8.1.1 and 2.2.11 - 2.2.12.
namespace farland::proto {

/// Share Control Header pduType, [MS-RDPBCGR] 2.2.8.1.1.1.1.
namespace pdu_type {
inline constexpr std::uint16_t demand_active = 0x1;
inline constexpr std::uint16_t confirm_active = 0x3;
inline constexpr std::uint16_t deactivate_all = 0x6;
inline constexpr std::uint16_t data = 0x7;
inline constexpr std::uint16_t server_redirect = 0xA;
/// Not on the wire: a Flow PDU (totalLength 0x8000), which carries nothing RDP uses.
inline constexpr std::uint16_t flow = 0x0;
}  // namespace pdu_type

/// Share Data Header pduType2, [MS-RDPBCGR] 2.2.8.1.1.1.2.
namespace pdu_type2 {
inline constexpr std::uint8_t update = 2;
inline constexpr std::uint8_t control = 20;
inline constexpr std::uint8_t pointer = 27;
inline constexpr std::uint8_t input = 28;
inline constexpr std::uint8_t synchronize = 31;
inline constexpr std::uint8_t refresh_rect = 33;
inline constexpr std::uint8_t play_sound = 34;
inline constexpr std::uint8_t suppress_output = 35;
inline constexpr std::uint8_t shutdown_request = 36;
inline constexpr std::uint8_t shutdown_denied = 37;
inline constexpr std::uint8_t save_session_info = 38;
inline constexpr std::uint8_t font_list = 39;
inline constexpr std::uint8_t font_map = 40;
inline constexpr std::uint8_t set_keyboard_indicators = 41;
inline constexpr std::uint8_t bitmap_cache_persistent_list = 43;
inline constexpr std::uint8_t bitmap_cache_error = 44;
inline constexpr std::uint8_t set_keyboard_ime_status = 45;
inline constexpr std::uint8_t offscreen_cache_error = 46;
inline constexpr std::uint8_t set_error_info = 47;
inline constexpr std::uint8_t draw_nine_grid_error = 48;
inline constexpr std::uint8_t draw_gdiplus_error = 49;
inline constexpr std::uint8_t arc_status = 50;
inline constexpr std::uint8_t status_info = 54;
inline constexpr std::uint8_t monitor_layout = 55;
inline constexpr std::uint8_t frame_acknowledge = 56;
}  // namespace pdu_type2

/// Control PDU actions, [MS-RDPBCGR] 2.2.1.15.1.
namespace control_action {
inline constexpr std::uint16_t request_control = 1;
inline constexpr std::uint16_t granted_control = 2;
inline constexpr std::uint16_t detach = 3;
inline constexpr std::uint16_t cooperate = 4;
}  // namespace control_action

/// Set Error Info codes farland sends, [MS-RDPBCGR] 2.2.5.1.1.
namespace errinfo {
inline constexpr std::uint32_t none = 0x00000000;
inline constexpr std::uint32_t rpc_initiated_disconnect = 0x00000001;
inline constexpr std::uint32_t rpc_initiated_logoff = 0x00000002;
inline constexpr std::uint32_t idle_timeout = 0x00000003;
inline constexpr std::uint32_t logon_timeout = 0x00000004;
inline constexpr std::uint32_t disconnected_by_other_connection = 0x00000005;
inline constexpr std::uint32_t out_of_memory = 0x00000006;
inline constexpr std::uint32_t server_denied_connection = 0x00000007;
inline constexpr std::uint32_t server_insufficient_privileges = 0x00000009;
inline constexpr std::uint32_t rpc_initiated_disconnect_by_user = 0x0000000B;
inline constexpr std::uint32_t logoff_by_user = 0x0000000C;
inline constexpr std::uint32_t bad_capabilities = 0x000010E0;  ///< ERRINFO_BAD_CAPABILITIES? see spec
}  // namespace errinfo

/// Share Control Header plus a reader over the rest of the PDU.
struct ShareControl {
    std::uint16_t type = 0;
    std::uint16_t source = 0;
    Reader body;
};

/// Reads the Share Control Header of the PDU filling `r`.
[[nodiscard]] Result<ShareControl> read_share_control(Reader& r);
/// Starts a Share Control PDU; finish with `end_share_control`.
[[nodiscard]] std::size_t begin_share_control(Writer& w, std::uint16_t type, std::uint16_t source);
void end_share_control(Writer& w, std::size_t start);

/// Share Data Header plus a reader over the payload.
struct ShareData {
    std::uint32_t share_id = 0;
    std::uint8_t stream_id = 0;
    std::uint8_t type2 = 0;
    Reader payload;
};
[[nodiscard]] Result<ShareData> read_share_data(Reader& body);
/// Writes a complete Data PDU: Share Control Header, Share Data Header, payload.
void write_data_pdu(Writer& w, std::uint32_t share_id, std::uint16_t source, std::uint8_t type2,
                    std::span<const std::byte> payload);

/// Demand Active (2.2.1.13.1.1) and Confirm Active (2.2.1.13.2.1).
struct DemandActive {
    std::uint32_t share_id = 0;
    std::string source_descriptor = "RDP";
    caps::CapabilitySets capabilities;
    std::uint32_t session_id = 0;
};
struct ConfirmActive {
    std::uint32_t share_id = 0;
    std::uint16_t originator_id = 0x03EA;
    std::string source_descriptor = "MSTSC";
    caps::CapabilitySets capabilities;
};
/// Deactivate All, [MS-RDPBCGR] 2.2.3.1.1.
struct DeactivateAll {
    std::uint32_t share_id = 0;
};

/// Decode from a Share Control body; capability bodies refer into the input.
[[nodiscard]] Result<DemandActive> decode_demand_active(Reader& body);
[[nodiscard]] Result<ConfirmActive> decode_confirm_active(Reader& body);
[[nodiscard]] Result<DeactivateAll> decode_deactivate_all(Reader& body);
/// Encode complete Share Control PDUs.
void encode_demand_active(Writer& w, std::uint16_t source, const DemandActive& pdu);
void encode_confirm_active(Writer& w, std::uint16_t source, const ConfirmActive& pdu);
void encode_deactivate_all(Writer& w, std::uint16_t source, const DeactivateAll& pdu);

// Data PDU payloads -----------------------------------------------------------

struct Synchronize {  // 2.2.1.14.1
    std::uint16_t message_type = 1;
    std::uint16_t target_user = 0;
};
struct Control {  // 2.2.1.15.1
    std::uint16_t action = 0;
    std::uint16_t grant_id = 0;
    std::uint32_t control_id = 0;
};
struct FontList {  // 2.2.1.18.1
    std::uint16_t number_fonts = 0;
    std::uint16_t total_number_fonts = 0;
    std::uint16_t list_flags = 0x0003;
    std::uint16_t entry_size = 0x0032;
};
struct FontMap {  // 2.2.1.22.1
    std::uint16_t number_entries = 0;
    std::uint16_t total_number_entries = 0;
    std::uint16_t map_flags = 0x0003;
    std::uint16_t entry_size = 0x0004;
};
struct SetErrorInfo {  // 2.2.5.1.1
    std::uint32_t error_info = 0;
};
/// TS_RECTANGLE16, inclusive bounds.
struct Rectangle16 {
    std::uint16_t left = 0;
    std::uint16_t top = 0;
    std::uint16_t right = 0;
    std::uint16_t bottom = 0;
    friend bool operator==(const Rectangle16&, const Rectangle16&) = default;
};
struct RefreshRect {  // 2.2.11.2.1
    std::vector<Rectangle16> areas;
};
struct SuppressOutput {  // 2.2.11.3.1
    bool allow_display_updates = true;
    std::optional<Rectangle16> desktop_rect;
};
struct ShutdownRequest {};    // 2.2.2.2.1
struct ShutdownDenied {};     // 2.2.2.3.1
struct FrameAcknowledgePdu {  // [MS-RDPRFX] 2.2.3.1
    std::uint32_t frame_id = 0;
};
struct InputPdu {  // 2.2.8.1.1.3
    std::vector<InputEvent> events;
};
/// A data PDU farland recognises but does not act on (Persistent Key List,
/// Suppress-free status PDUs, ...).
struct OtherDataPdu {
    std::uint8_t type2 = 0;
};

using DataPdu = std::variant<Synchronize, Control, FontList, FontMap, SetErrorInfo, RefreshRect, SuppressOutput,
                             ShutdownRequest, ShutdownDenied, FrameAcknowledgePdu, InputPdu, OtherDataPdu>;

/// Decodes the payload of a Data PDU according to its pduType2.
[[nodiscard]] Result<DataPdu> decode_data_pdu(ShareData& data);

/// Payload encoders, for `write_data_pdu`.
void encode(Writer& w, const Synchronize& pdu);
void encode(Writer& w, const Control& pdu);
void encode(Writer& w, const FontList& pdu);
void encode(Writer& w, const FontMap& pdu);
void encode(Writer& w, const SetErrorInfo& pdu);
void encode(Writer& w, const RefreshRect& pdu);
void encode(Writer& w, const SuppressOutput& pdu);
void encode(Writer& w, const FrameAcknowledgePdu& pdu);
void encode(Writer& w, const InputPdu& pdu);
/// Shutdown Request and Denied have no payload.
void encode(Writer& w, const ShutdownRequest& pdu);
void encode(Writer& w, const ShutdownDenied& pdu);

/// pduType2 for each encodable payload.
[[nodiscard]] std::uint8_t type2_of(const DataPdu& pdu);

}  // namespace farland::proto
