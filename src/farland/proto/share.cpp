// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/proto/share.hpp>

namespace farland::proto {

namespace {

constexpr std::uint16_t ts_protocol_version = 0x0010;
constexpr std::uint16_t flow_marker = 0x8000;
constexpr std::size_t share_control_header_size = 6;
constexpr std::uint8_t stream_low = 0x01;
constexpr std::uint8_t packet_compressed = 0x20;
constexpr std::uint16_t max_source_descriptor = 64;

std::span<const std::byte> as_bytes(std::string_view text)
{
    return std::as_bytes(std::span(text));
}

Result<std::string> read_source_descriptor(Reader& r, std::uint16_t length)
{
    if (length > max_source_descriptor) {
        return fail(Errc::limit_exceeded, "source descriptor longer than 64 bytes", r.offset());
    }
    FARLAND_TRY(const auto bytes, r.bytes(length));
    std::string text;
    for (const std::byte b : bytes) {
        const auto c = std::to_integer<char>(b);
        if (c == '\0') {
            break;
        }
        text.push_back(c);
    }
    return text;
}

/// The common part of Demand Active and Confirm Active after shareId (and
/// originatorId): descriptor, then the combined capability block.
Result<caps::CapabilitySets> read_capabilities(Reader& body, std::string& descriptor)
{
    FARLAND_TRY(const std::uint16_t descriptor_length, body.u16le());
    const std::size_t combined_offset = body.offset();
    FARLAND_TRY(const std::uint16_t combined_length, body.u16le());
    FARLAND_TRY(descriptor, read_source_descriptor(body, descriptor_length));
    if (combined_length < 4) {
        return fail(Errc::invalid_length, "lengthCombinedCapabilities below 4", combined_offset);
    }
    FARLAND_TRY(Reader combined, body.sub(combined_length));
    FARLAND_TRY(const std::uint16_t count, combined.u16le());
    FARLAND_TRY_VOID(combined.skip(2));  // pad2Octets
    FARLAND_TRY(auto sets, caps::decode_capability_sets(combined, count));
    FARLAND_TRY_VOID(combined.expect_end("capability sets"));
    return sets;
}

void write_capabilities(Writer& w, std::string_view descriptor, const caps::CapabilitySets& sets)
{
    w.u16le(static_cast<std::uint16_t>(descriptor.size() + 1));
    const std::size_t combined_pos = w.size();
    w.u16le(0);  // lengthCombinedCapabilities, patched below
    w.bytes(as_bytes(descriptor));
    w.u8(0);
    const std::size_t combined_start = w.size();
    w.u16le(0);  // numberCapabilities, patched below
    w.u16le(0);  // pad2Octets
    const std::uint16_t count = caps::encode_capability_sets(w, sets);
    const std::size_t combined = w.size() - combined_start;
    FARLAND_ASSERT(combined <= 0xFFFF);
    w.patch_u16le(combined_pos, static_cast<std::uint16_t>(combined));
    w.patch_u16le(combined_start, count);
}

Result<Rectangle16> read_rectangle(Reader& r)
{
    Rectangle16 rect;
    FARLAND_TRY(rect.left, r.u16le());
    FARLAND_TRY(rect.top, r.u16le());
    FARLAND_TRY(rect.right, r.u16le());
    FARLAND_TRY(rect.bottom, r.u16le());
    return rect;
}

void write_rectangle(Writer& w, const Rectangle16& rect)
{
    w.u16le(rect.left);
    w.u16le(rect.top);
    w.u16le(rect.right);
    w.u16le(rect.bottom);
}

}  // namespace

Result<ShareControl> read_share_control(Reader& r)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint16_t total_length, r.u16le());
    if (total_length == flow_marker) {
        // Flow PDU, [MS-RDPBCGR] 2.2.8.1.1.1.1 note: skip it entirely.
        FARLAND_TRY_VOID(r.skip(r.remaining()));
        return ShareControl{pdu_type::flow, 0, Reader{}};
    }
    if (total_length < share_control_header_size || total_length != r.remaining() + 2) {
        return fail(Errc::invalid_length, "share control totalLength does not match the PDU", start);
    }
    FARLAND_TRY(const std::uint16_t type, r.u16le());
    FARLAND_TRY(const std::uint16_t source, r.u16le());
    return ShareControl{static_cast<std::uint16_t>(type & 0x000FU), source, r.sub(r.remaining()).value()};
}

std::size_t begin_share_control(Writer& w, std::uint16_t type, std::uint16_t source)
{
    const std::size_t start = w.size();
    w.u16le(0);  // totalLength, patched by end_share_control
    w.u16le(static_cast<std::uint16_t>(type | ts_protocol_version));
    w.u16le(source);
    return start;
}

void end_share_control(Writer& w, std::size_t start)
{
    const std::size_t length = w.size() - start;
    FARLAND_ASSERT(length <= 0x7FFF);
    w.patch_u16le(start, static_cast<std::uint16_t>(length));
}

Result<ShareData> read_share_data(Reader& body)
{
    ShareData data;
    FARLAND_TRY(data.share_id, body.u32le());
    FARLAND_TRY_VOID(body.skip(1));  // pad1: not always zero in practice
    FARLAND_TRY(data.stream_id, body.u8());
    FARLAND_TRY_VOID(body.skip(2));  // uncompressedLength: inconsistent between implementations
    FARLAND_TRY(data.type2, body.u8());
    const std::size_t compression_offset = body.offset();
    FARLAND_TRY(const std::uint8_t compressed_type, body.u8());
    FARLAND_TRY_VOID(body.skip(2));  // compressedLength
    if ((compressed_type & packet_compressed) != 0) {
        return fail(Errc::unsupported, "compressed share data PDU", compression_offset);
    }
    FARLAND_TRY(data.payload, body.sub(body.remaining()));
    return data;
}

void write_data_pdu(Writer& w, std::uint32_t share_id, std::uint16_t source, std::uint8_t type2,
                    std::span<const std::byte> payload)
{
    const std::size_t start = begin_share_control(w, pdu_type::data, source);
    w.u32le(share_id);
    w.u8(0);  // pad1
    w.u8(stream_low);
    // uncompressedLength counts from pduType2 on, as in [MS-RDPBCGR] 4.1.14.
    FARLAND_ASSERT(payload.size() + 4 <= 0xFFFF);
    w.u16le(static_cast<std::uint16_t>(payload.size() + 4));
    w.u8(type2);
    w.u8(0);     // compressedType
    w.u16le(0);  // compressedLength
    w.bytes(payload);
    end_share_control(w, start);
}

Result<DemandActive> decode_demand_active(Reader& body)
{
    DemandActive pdu;
    FARLAND_TRY(pdu.share_id, body.u32le());
    FARLAND_TRY(pdu.capabilities, read_capabilities(body, pdu.source_descriptor));
    if (!body.empty()) {
        FARLAND_TRY(pdu.session_id, body.u32le());
    }
    return pdu;
}

Result<ConfirmActive> decode_confirm_active(Reader& body)
{
    ConfirmActive pdu;
    FARLAND_TRY(pdu.share_id, body.u32le());
    FARLAND_TRY(pdu.originator_id, body.u16le());
    FARLAND_TRY(pdu.capabilities, read_capabilities(body, pdu.source_descriptor));
    // Some clients pad the PDU; nothing after the capability sets is used.
    return pdu;
}

Result<DeactivateAll> decode_deactivate_all(Reader& body)
{
    DeactivateAll pdu;
    FARLAND_TRY(pdu.share_id, body.u32le());
    if (!body.empty()) {
        FARLAND_TRY(const std::uint16_t length, body.u16le());
        FARLAND_TRY_VOID(body.skip(length));
    }
    return pdu;
}

void encode_demand_active(Writer& w, std::uint16_t source, const DemandActive& pdu)
{
    const std::size_t start = begin_share_control(w, pdu_type::demand_active, source);
    w.u32le(pdu.share_id);
    write_capabilities(w, pdu.source_descriptor, pdu.capabilities);
    w.u32le(pdu.session_id);
    end_share_control(w, start);
}

void encode_confirm_active(Writer& w, std::uint16_t source, const ConfirmActive& pdu)
{
    const std::size_t start = begin_share_control(w, pdu_type::confirm_active, source);
    w.u32le(pdu.share_id);
    w.u16le(pdu.originator_id);
    write_capabilities(w, pdu.source_descriptor, pdu.capabilities);
    end_share_control(w, start);
}

void encode_deactivate_all(Writer& w, std::uint16_t source, const DeactivateAll& pdu)
{
    const std::size_t start = begin_share_control(w, pdu_type::deactivate_all, source);
    w.u32le(pdu.share_id);
    w.u16le(1);  // lengthSourceDescriptor
    w.u8(0);
    end_share_control(w, start);
}

Result<DataPdu> decode_data_pdu(ShareData& data)
{
    Reader& r = data.payload;
    switch (data.type2) {
    case pdu_type2::synchronize: {
        Synchronize pdu;
        FARLAND_TRY(pdu.message_type, r.u16le());
        FARLAND_TRY(pdu.target_user, r.u16le());
        if (pdu.message_type != 1) {
            return fail(Errc::invalid_value, "Synchronize messageType is not SYNCMSGTYPE_SYNC", r.offset() - 4);
        }
        return pdu;
    }
    case pdu_type2::control: {
        Control pdu;
        FARLAND_TRY(pdu.action, r.u16le());
        FARLAND_TRY(pdu.grant_id, r.u16le());
        FARLAND_TRY(pdu.control_id, r.u32le());
        return pdu;
    }
    case pdu_type2::font_list: {
        FontList pdu;
        FARLAND_TRY(pdu.number_fonts, r.u16le());
        FARLAND_TRY(pdu.total_number_fonts, r.u16le());
        FARLAND_TRY(pdu.list_flags, r.u16le());
        FARLAND_TRY(pdu.entry_size, r.u16le());
        return pdu;
    }
    case pdu_type2::font_map: {
        FontMap pdu;
        FARLAND_TRY(pdu.number_entries, r.u16le());
        FARLAND_TRY(pdu.total_number_entries, r.u16le());
        FARLAND_TRY(pdu.map_flags, r.u16le());
        FARLAND_TRY(pdu.entry_size, r.u16le());
        return pdu;
    }
    case pdu_type2::set_error_info: {
        FARLAND_TRY(const std::uint32_t code, r.u32le());
        return SetErrorInfo{code};
    }
    case pdu_type2::refresh_rect: {
        RefreshRect pdu;
        FARLAND_TRY(const std::uint8_t count, r.u8());
        FARLAND_TRY_VOID(r.skip(3));  // pad3Octets
        for (std::uint8_t i = 0; i < count; ++i) {
            FARLAND_TRY(const auto rect, read_rectangle(r));
            pdu.areas.push_back(rect);
        }
        return pdu;
    }
    case pdu_type2::suppress_output: {
        SuppressOutput pdu;
        const std::size_t start = r.offset();
        FARLAND_TRY(const std::uint8_t allow, r.u8());
        if (allow > 1) {
            return fail(Errc::invalid_value, "allowDisplayUpdates is neither 0 nor 1", start);
        }
        FARLAND_TRY_VOID(r.skip(3));  // pad3Octets
        pdu.allow_display_updates = allow == 1;
        if (pdu.allow_display_updates) {
            FARLAND_TRY(pdu.desktop_rect, read_rectangle(r));
        }
        return pdu;
    }
    case pdu_type2::shutdown_request:
        return ShutdownRequest{};
    case pdu_type2::shutdown_denied:
        return ShutdownDenied{};
    case pdu_type2::frame_acknowledge: {
        FARLAND_TRY(const std::uint32_t frame, r.u32le());
        return FrameAcknowledgePdu{frame};
    }
    case pdu_type2::input: {
        FARLAND_TRY(auto events, decode_slow_path_input(r));
        return InputPdu{std::move(events)};
    }
    default:
        return OtherDataPdu{data.type2};
    }
}

void encode(Writer& w, const Synchronize& pdu)
{
    w.u16le(pdu.message_type);
    w.u16le(pdu.target_user);
}

void encode(Writer& w, const Control& pdu)
{
    w.u16le(pdu.action);
    w.u16le(pdu.grant_id);
    w.u32le(pdu.control_id);
}

void encode(Writer& w, const FontList& pdu)
{
    w.u16le(pdu.number_fonts);
    w.u16le(pdu.total_number_fonts);
    w.u16le(pdu.list_flags);
    w.u16le(pdu.entry_size);
}

void encode(Writer& w, const FontMap& pdu)
{
    w.u16le(pdu.number_entries);
    w.u16le(pdu.total_number_entries);
    w.u16le(pdu.map_flags);
    w.u16le(pdu.entry_size);
}

void encode(Writer& w, const SetErrorInfo& pdu)
{
    w.u32le(pdu.error_info);
}

void encode(Writer& w, const RefreshRect& pdu)
{
    FARLAND_ASSERT(pdu.areas.size() <= 0xFF);
    w.u8(static_cast<std::uint8_t>(pdu.areas.size()));
    w.zeros(3);
    for (const auto& rect : pdu.areas) {
        write_rectangle(w, rect);
    }
}

void encode(Writer& w, const SuppressOutput& pdu)
{
    w.u8(pdu.allow_display_updates ? 1 : 0);
    w.zeros(3);
    if (pdu.allow_display_updates) {
        write_rectangle(w, pdu.desktop_rect.value_or(Rectangle16{}));
    }
}

void encode(Writer& w, const FrameAcknowledgePdu& pdu)
{
    w.u32le(pdu.frame_id);
}

void encode(Writer& w, const InputPdu& pdu)
{
    encode_slow_path_input(w, pdu.events);
}

void encode(Writer& /*w*/, const ShutdownRequest& /*pdu*/) {}

void encode(Writer& /*w*/, const ShutdownDenied& /*pdu*/) {}

std::uint8_t type2_of(const DataPdu& pdu)
{
    return std::visit(
        [](const auto& p) -> std::uint8_t {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, Synchronize>) {
                return pdu_type2::synchronize;
            } else if constexpr (std::is_same_v<T, Control>) {
                return pdu_type2::control;
            } else if constexpr (std::is_same_v<T, FontList>) {
                return pdu_type2::font_list;
            } else if constexpr (std::is_same_v<T, FontMap>) {
                return pdu_type2::font_map;
            } else if constexpr (std::is_same_v<T, SetErrorInfo>) {
                return pdu_type2::set_error_info;
            } else if constexpr (std::is_same_v<T, RefreshRect>) {
                return pdu_type2::refresh_rect;
            } else if constexpr (std::is_same_v<T, SuppressOutput>) {
                return pdu_type2::suppress_output;
            } else if constexpr (std::is_same_v<T, ShutdownRequest>) {
                return pdu_type2::shutdown_request;
            } else if constexpr (std::is_same_v<T, ShutdownDenied>) {
                return pdu_type2::shutdown_denied;
            } else if constexpr (std::is_same_v<T, FrameAcknowledgePdu>) {
                return pdu_type2::frame_acknowledge;
            } else if constexpr (std::is_same_v<T, InputPdu>) {
                return pdu_type2::input;
            } else {
                return p.type2;
            }
        },
        pdu);
}

}  // namespace farland::proto
