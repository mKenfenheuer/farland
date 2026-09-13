// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include <cstdint>

/// Security headers under Enhanced RDP Security (TLS, CredSSP): only a few
/// PDUs carry the basic TS_SECURITY_HEADER, and none is encrypted by RDP
/// itself, [MS-RDPBCGR] 2.2.8.1.1.2.1 and 5.4.
namespace farland::proto {

/// TS_SECURITY_HEADER flags.
namespace sec_flags {
inline constexpr std::uint16_t exchange_pkt = 0x0001;
inline constexpr std::uint16_t transport_req = 0x0002;
inline constexpr std::uint16_t transport_rsp = 0x0004;
inline constexpr std::uint16_t encrypt = 0x0008;
inline constexpr std::uint16_t reset_seqno = 0x0010;
inline constexpr std::uint16_t ignore_seqno = 0x0020;
inline constexpr std::uint16_t info_pkt = 0x0040;
inline constexpr std::uint16_t license_pkt = 0x0080;
inline constexpr std::uint16_t license_encrypt_cs = 0x0200;
inline constexpr std::uint16_t redirection_pkt = 0x0400;
inline constexpr std::uint16_t secure_checksum = 0x0800;
inline constexpr std::uint16_t autodetect_req = 0x1000;
inline constexpr std::uint16_t autodetect_rsp = 0x2000;
inline constexpr std::uint16_t heartbeat = 0x4000;
inline constexpr std::uint16_t flagshi_valid = 0x8000;
}  // namespace sec_flags

/// Reads a basic security header and returns its flags. An encrypted PDU is
/// an error: under Enhanced RDP Security the peer must not use RDP encryption.
[[nodiscard]] Result<std::uint16_t> read_basic_security_header(Reader& r);
void write_basic_security_header(Writer& w, std::uint16_t flags);

}  // namespace farland::proto
