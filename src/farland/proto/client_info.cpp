// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/proto/client_info.hpp>

#include <algorithm>
#include <array>
#include <vector>

namespace farland::proto {

namespace {

constexpr std::size_t time_zone_name_size = 64;
constexpr std::size_t system_time_size = 16;
constexpr std::uint16_t max_client_address_size = 80;
constexpr std::uint16_t max_client_dir_size = 512;
constexpr std::uint16_t max_dst_key_name_size = 254;
constexpr std::uint16_t auto_reconnect_cookie_size = 28;

std::string decode_ansi(std::span<const std::byte> bytes)
{
    std::string out;
    for (const std::byte b : bytes) {
        const auto c = std::to_integer<unsigned char>(b);
        if (c == 0) {
            break;
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else {  // Latin-1 to UTF-8
            out.push_back(static_cast<char>(0xC0U | (c >> 6U)));
            out.push_back(static_cast<char>(0x80U | (c & 0x3FU)));
        }
    }
    return out;
}

/// Reads one of the five TS_INFO_PACKET strings: `size` bytes plus the
/// mandatory terminator (two bytes when Unicode, one otherwise).
Result<std::string> read_info_string(Reader& r, std::uint16_t size, bool unicode)
{
    if (size > max_info_string_size) {
        return fail(Errc::limit_exceeded, "Client Info string longer than 512 bytes", r.offset());
    }
    if (unicode && size % 2 != 0) {
        return fail(Errc::invalid_length, "odd length for a Unicode Client Info string", r.offset());
    }
    FARLAND_TRY(const auto bytes, r.bytes(size));
    FARLAND_TRY_VOID(r.skip(unicode ? 2 : 1));
    return unicode ? utf16le_to_utf8(bytes) : decode_ansi(bytes);
}

/// A Unicode string whose length field counts the terminator (clientAddress, clientDir).
Result<std::string> read_counted_unicode(Reader& r, std::uint16_t max)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint16_t size, r.u16le());
    if (size > max) {
        return fail(Errc::limit_exceeded, "Client Info extended string too long", start);
    }
    FARLAND_TRY(const auto bytes, r.bytes(size));
    return utf16le_to_utf8(bytes);
}

Result<TimeZoneInformation> read_time_zone(Reader& r)
{
    TimeZoneInformation tz;
    FARLAND_TRY(const std::uint32_t bias, r.u32le());
    FARLAND_TRY(const auto standard_name, r.bytes(time_zone_name_size));
    FARLAND_TRY(const auto standard_date, r.bytes(system_time_size));
    FARLAND_TRY(const std::uint32_t standard_bias, r.u32le());
    FARLAND_TRY(const auto daylight_name, r.bytes(time_zone_name_size));
    FARLAND_TRY(const auto daylight_date, r.bytes(system_time_size));
    FARLAND_TRY(const std::uint32_t daylight_bias, r.u32le());
    tz.bias = static_cast<std::int32_t>(bias);
    tz.standard_name = utf16le_to_utf8(standard_name);
    std::ranges::copy(standard_date, tz.standard_date.begin());
    tz.standard_bias = static_cast<std::int32_t>(standard_bias);
    tz.daylight_name = utf16le_to_utf8(daylight_name);
    std::ranges::copy(daylight_date, tz.daylight_date.begin());
    tz.daylight_bias = static_cast<std::int32_t>(daylight_bias);
    return tz;
}

Result<ExtendedInfo> read_extended_info(Reader& r)
{
    ExtendedInfo ext;
    FARLAND_TRY(ext.client_address_family, r.u16le());
    FARLAND_TRY(ext.client_address, read_counted_unicode(r, max_client_address_size));
    FARLAND_TRY(ext.client_dir, read_counted_unicode(r, max_client_dir_size));
    if (r.empty()) {
        return ext;
    }
    FARLAND_TRY(ext.time_zone, read_time_zone(r));
    if (r.empty()) {
        return ext;
    }
    FARLAND_TRY(ext.session_id, r.u32le());
    if (r.empty()) {
        return ext;
    }
    FARLAND_TRY(ext.performance_flags, r.u32le());
    if (r.empty()) {
        return ext;
    }
    const std::size_t cookie_offset = r.offset();
    FARLAND_TRY(const std::uint16_t cookie_size, r.u16le());
    if (cookie_size != 0 && cookie_size != auto_reconnect_cookie_size) {
        return fail(Errc::invalid_length, "auto-reconnect cookie is not 28 bytes", cookie_offset);
    }
    if (cookie_size == auto_reconnect_cookie_size) {
        FARLAND_TRY(Reader cookie, r.sub(cookie_size));
        AutoReconnectCookie arc;
        FARLAND_TRY(const std::uint32_t length, cookie.u32le());
        if (length != auto_reconnect_cookie_size) {
            return fail(Errc::invalid_length, "ARC_CS_PRIVATE_PACKET length is not 28", cookie_offset + 2);
        }
        FARLAND_TRY(arc.version, cookie.u32le());
        FARLAND_TRY(arc.logon_id, cookie.u32le());
        FARLAND_TRY(const auto verifier, cookie.bytes(16));
        std::ranges::copy(verifier, arc.security_verifier.begin());
        ext.auto_reconnect_cookie = arc;
    }
    if (r.empty()) {
        return ext;
    }
    FARLAND_TRY_VOID(r.skip(4));  // reserved1, reserved2
    if (r.empty()) {
        return ext;
    }
    const std::size_t key_offset = r.offset();
    FARLAND_TRY(const std::uint16_t key_size, r.u16le());
    if (key_size > max_dst_key_name_size) {
        return fail(Errc::limit_exceeded, "dynamic DST time zone key name too long", key_offset);
    }
    FARLAND_TRY(const auto key, r.bytes(key_size));
    ext.dynamic_dst_time_zone_key_name = utf16le_to_utf8(key);
    if (r.empty()) {
        return ext;
    }
    FARLAND_TRY(const std::uint16_t disabled, r.u16le());
    ext.dynamic_daylight_time_disabled = disabled != 0;
    return ext;  // Later additions to the structure are ignored.
}

void write_fixed_utf16(Writer& w, std::string_view text, std::size_t size)
{
    auto encoded = utf8_to_utf16le(text);
    if (encoded.size() > size - 2) {
        encoded.resize(size - 2);
    }
    w.bytes(encoded);
    w.zeros(size - encoded.size());
}

void write_counted_unicode(Writer& w, std::string_view text)
{
    const auto encoded = utf8_to_utf16le(text);
    w.u16le(static_cast<std::uint16_t>(encoded.size() + 2));
    w.bytes(encoded);
    w.u16le(0);
}

}  // namespace

Result<ClientInfo> decode_client_info(Reader& r)
{
    ClientInfo info;
    FARLAND_TRY(info.code_page, r.u32le());
    FARLAND_TRY(info.flags, r.u32le());
    FARLAND_TRY(const std::uint16_t cb_domain, r.u16le());
    FARLAND_TRY(const std::uint16_t cb_user_name, r.u16le());
    FARLAND_TRY(const std::uint16_t cb_password, r.u16le());
    FARLAND_TRY(const std::uint16_t cb_alternate_shell, r.u16le());
    FARLAND_TRY(const std::uint16_t cb_working_dir, r.u16le());
    const bool unicode = (info.flags & info_flags::unicode) != 0;
    FARLAND_TRY(info.domain, read_info_string(r, cb_domain, unicode));
    FARLAND_TRY(info.user_name, read_info_string(r, cb_user_name, unicode));
    FARLAND_TRY(auto password, read_info_string(r, cb_password, unicode));
    info.password = SecretString(std::move(password));
    FARLAND_TRY(info.alternate_shell, read_info_string(r, cb_alternate_shell, unicode));
    FARLAND_TRY(info.working_dir, read_info_string(r, cb_working_dir, unicode));
    if (!r.empty()) {
        FARLAND_TRY(info.extended, read_extended_info(r));
    }
    return info;
}

void encode_client_info(Writer& w, const ClientInfo& info)
{
    const auto domain = utf8_to_utf16le(info.domain);
    const auto user = utf8_to_utf16le(info.user_name);
    auto password = utf8_to_utf16le(info.password.view());
    const auto shell = utf8_to_utf16le(info.alternate_shell);
    const auto dir = utf8_to_utf16le(info.working_dir);
    const std::array<const std::vector<std::byte>*, 5> strings{&domain, &user, &password, &shell, &dir};
    for (const auto* s : strings) {
        FARLAND_ASSERT(s->size() <= max_info_string_size);
    }

    w.u32le(info.code_page);
    w.u32le(info.flags | info_flags::unicode);
    w.u16le(static_cast<std::uint16_t>(domain.size()));
    w.u16le(static_cast<std::uint16_t>(user.size()));
    w.u16le(static_cast<std::uint16_t>(password.size()));
    w.u16le(static_cast<std::uint16_t>(shell.size()));
    w.u16le(static_cast<std::uint16_t>(dir.size()));
    for (const auto* s : strings) {
        w.bytes(*s);
        w.u16le(0);
    }
    secure_zero(password);

    if (!info.extended) {
        return;
    }
    const auto& ext = *info.extended;
    w.u16le(ext.client_address_family);
    write_counted_unicode(w, ext.client_address);
    write_counted_unicode(w, ext.client_dir);
    if (!ext.time_zone) {
        return;
    }
    const auto& tz = *ext.time_zone;
    w.u32le(static_cast<std::uint32_t>(tz.bias));
    write_fixed_utf16(w, tz.standard_name, time_zone_name_size);
    w.bytes(tz.standard_date);
    w.u32le(static_cast<std::uint32_t>(tz.standard_bias));
    write_fixed_utf16(w, tz.daylight_name, time_zone_name_size);
    w.bytes(tz.daylight_date);
    w.u32le(static_cast<std::uint32_t>(tz.daylight_bias));
    if (!ext.session_id) {
        return;
    }
    w.u32le(*ext.session_id);
    if (!ext.performance_flags) {
        return;
    }
    w.u32le(*ext.performance_flags);
    if (ext.auto_reconnect_cookie) {
        w.u16le(auto_reconnect_cookie_size);
        w.u32le(auto_reconnect_cookie_size);
        w.u32le(ext.auto_reconnect_cookie->version);
        w.u32le(ext.auto_reconnect_cookie->logon_id);
        w.bytes(ext.auto_reconnect_cookie->security_verifier);
    } else {
        w.u16le(0);
    }
    if (!ext.dynamic_dst_time_zone_key_name) {
        return;
    }
    w.u16le(0);  // reserved1
    w.u16le(0);  // reserved2
    const auto key = utf8_to_utf16le(*ext.dynamic_dst_time_zone_key_name);
    FARLAND_ASSERT(key.size() <= max_dst_key_name_size);
    w.u16le(static_cast<std::uint16_t>(key.size()));
    w.bytes(key);
    if (ext.dynamic_daylight_time_disabled) {
        w.u16le(*ext.dynamic_daylight_time_disabled ? 1 : 0);
    }
}

}  // namespace farland::proto
