// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/proto/license.hpp>
#include <farland/proto/security.hpp>

namespace farland::proto {

namespace {

constexpr std::uint8_t error_alert = 0xFF;
constexpr std::uint8_t preamble_version_3_0 = 0x03;
constexpr std::uint8_t preamble_version_mask = 0x0F;
constexpr std::uint16_t valid_client_message_size = 16;
constexpr std::uint32_t status_valid_client = 0x00000007;
constexpr std::uint32_t st_no_transition = 0x00000002;
constexpr std::uint16_t bb_error_blob = 0x0004;

}  // namespace

void encode_license_valid_client(Writer& w)
{
    write_basic_security_header(w, sec_flags::license_pkt);
    // LICENSE_PREAMBLE
    w.u8(error_alert);
    w.u8(preamble_version_3_0);
    w.u16le(valid_client_message_size);
    // LICENSE_ERROR_MESSAGE
    w.u32le(status_valid_client);
    w.u32le(st_no_transition);
    w.u16le(bb_error_blob);
    w.u16le(0);
}

Result<void> decode_license_valid_client(Reader& r)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint16_t flags, read_basic_security_header(r));
    if ((flags & sec_flags::license_pkt) == 0) {
        return fail(Errc::invalid_value, "expected a licensing PDU", start);
    }
    const std::size_t preamble = r.offset();
    FARLAND_TRY(const std::uint8_t type, r.u8());
    FARLAND_TRY(const std::uint8_t version, r.u8());
    FARLAND_TRY(const std::uint16_t size, r.u16le());
    if (type != error_alert) {
        return fail(Errc::unsupported, "server requires a licensing exchange", preamble);
    }
    if ((version & preamble_version_mask) < preamble_version_3_0 || size < valid_client_message_size) {
        return fail(Errc::invalid_value, "malformed license preamble", preamble);
    }
    FARLAND_TRY(const std::uint32_t code, r.u32le());
    FARLAND_TRY(const std::uint32_t transition, r.u32le());
    if (code != status_valid_client || transition != st_no_transition) {
        return fail(Errc::unsupported, "license error other than STATUS_VALID_CLIENT", preamble + 4);
    }
    return {};  // The error blob carries nothing of interest.
}

}  // namespace farland::proto
