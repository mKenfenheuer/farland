// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

/// Licensing. farland never requires client access licenses: the server ends
/// the licensing phase at once with the "valid client" error message,
/// [MS-RDPBCGR] 2.2.1.12 and [MS-RDPELE] 2.2.2.7.
namespace farland::proto {

/// Server License Error PDU - Valid Client, including the basic security
/// header (SEC_LICENSE_PKT). The preamble flags are 0x03 without
/// EXTENDED_ERROR_MSG_SUPPORTED, as in [MS-RDPBCGR] 4.1.11; FreeRDP notes that
/// setting 0x80 crashes mstsc.
void encode_license_valid_client(Writer& w);

/// Client side: accepts the licensing PDU if it is the valid-client error;
/// any other licensing exchange is `Errc::unsupported`. `r` starts at the
/// security header.
[[nodiscard]] Result<void> decode_license_valid_client(Reader& r);

}  // namespace farland::proto
