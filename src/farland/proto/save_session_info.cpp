// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/proto/save_session_info.hpp>

#include <algorithm>

namespace farland::proto {

namespace {

constexpr std::uint32_t arc_sc_size = 28;       ///< ARC_SC_PRIVATE_PACKET cbLen, 2.2.4.2
constexpr std::uint32_t logon_errors_size = 8;  ///< TS_LOGON_ERRORS_INFO
constexpr std::size_t field_header_size = 4;    ///< TS_LOGON_INFO_FIELD cbFieldData, 2.2.10.1.1.4.1
constexpr std::size_t pad_size = 570;           ///< TS_LOGON_INFO_EXTENDED Pad
constexpr std::size_t fixed_size = 2 + 4;       ///< Length and FieldsPresent
constexpr std::uint32_t known_fields = logon_ex::auto_reconnect_cookie | logon_ex::logon_errors;

}  // namespace

void encode_save_session_info(Writer& w, const LogonInfoExtended& info)
{
    // [MS-RDPBCGR] 2.2.10.1.1.4: Length is "the total size in bytes of this
    // structure, including the variable LogonFields field". farland counts
    // the whole structure, pad included. FreeRDP's rdp_write_logon_info_ex
    // also includes the pad but counts the cookie field as 28 rather than
    // 4 + 28; FreeRDP's reader only requires Length - 6 more bytes, which
    // both values satisfy.
    std::uint32_t fields = 0;
    std::size_t length = fixed_size + pad_size;
    if (info.auto_reconnect_cookie) {
        fields |= logon_ex::auto_reconnect_cookie;
        length += field_header_size + arc_sc_size;
    }
    if (info.logon_errors) {
        fields |= logon_ex::logon_errors;
        length += field_header_size + logon_errors_size;
    }
    w.u32le(info_type::logon_extended_info);
    w.u16le(static_cast<std::uint16_t>(length));
    w.u32le(fields);
    // Fields in their implicit order: auto-reconnect cookie, then logon errors.
    if (const auto& cookie = info.auto_reconnect_cookie) {
        w.u32le(arc_sc_size);  // cbFieldData
        w.u32le(arc_sc_size);  // cbLen
        w.u32le(cookie->version);
        w.u32le(cookie->logon_id);
        w.bytes(cookie->random_bits);
    }
    if (const auto& errors = info.logon_errors) {
        w.u32le(logon_errors_size);
        w.u32le(errors->error_notification_type);
        w.u32le(errors->error_notification_data);
    }
    w.zeros(pad_size);
}

Result<LogonInfoExtended> decode_save_session_info(Reader& r)
{
    const std::size_t type_offset = r.offset();
    FARLAND_TRY(const std::uint32_t type, r.u32le());
    if (type != info_type::logon_extended_info) {
        return fail(Errc::unsupported, "Save Session Info type other than extended logon info", type_offset);
    }
    const std::size_t length_offset = r.offset();
    FARLAND_TRY(const std::uint16_t length, r.u16le());
    FARLAND_TRY(const std::uint32_t fields, r.u32le());
    if (length < fixed_size || length - fixed_size > r.remaining()) {
        return fail(Errc::invalid_length, "TS_LOGON_INFO_EXTENDED length out of range", length_offset);
    }
    if ((fields & ~known_fields) != 0) {
        return fail(Errc::unsupported, "unknown TS_LOGON_INFO_EXTENDED field", length_offset + 2);
    }

    LogonInfoExtended info;
    if ((fields & logon_ex::auto_reconnect_cookie) != 0) {
        const std::size_t field_offset = r.offset();
        FARLAND_TRY(const std::uint32_t field_size, r.u32le());
        FARLAND_TRY(const std::uint32_t cb_len, r.u32le());
        if (field_size != arc_sc_size || cb_len != arc_sc_size) {
            return fail(Errc::invalid_length, "ARC_SC_PRIVATE_PACKET is not 28 bytes", field_offset);
        }
        ServerAutoReconnectCookie cookie;
        FARLAND_TRY(cookie.version, r.u32le());
        FARLAND_TRY(cookie.logon_id, r.u32le());
        FARLAND_TRY(const auto random, r.bytes(cookie.random_bits.size()));
        std::ranges::copy(random, cookie.random_bits.begin());
        info.auto_reconnect_cookie = cookie;
    }
    if ((fields & logon_ex::logon_errors) != 0) {
        const std::size_t field_offset = r.offset();
        FARLAND_TRY(const std::uint32_t field_size, r.u32le());
        if (field_size != logon_errors_size) {
            return fail(Errc::invalid_length, "TS_LOGON_ERRORS_INFO is not 8 bytes", field_offset);
        }
        LogonErrorsInfo errors;
        FARLAND_TRY(errors.error_notification_type, r.u32le());
        FARLAND_TRY(errors.error_notification_data, r.u32le());
        info.logon_errors = errors;
    }
    FARLAND_TRY_VOID(r.skip(pad_size));  // "Values in this field MUST be ignored."
    return info;
}

}  // namespace farland::proto
