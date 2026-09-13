// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/proto/security.hpp>

namespace farland::proto {

Result<std::uint16_t> read_basic_security_header(Reader& r)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint16_t flags, r.u16le());
    FARLAND_TRY_VOID(r.skip(2));  // flagsHi: unused, and not always zero in practice
    if ((flags & sec_flags::encrypt) != 0) {
        return fail(Errc::invalid_value, "RDP-encrypted PDU under Enhanced RDP Security", start);
    }
    return flags;
}

void write_basic_security_header(Writer& w, std::uint16_t flags)
{
    w.u16le(flags);
    w.u16le(0);
}

}  // namespace farland::proto
