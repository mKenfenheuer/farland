// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Save Session Info PDU data (TS_SAVE_SESSION_INFO_PDU_DATA with extended
// logon info), as a client receives it. Whatever decodes must re-encode to
// something that decodes to the same values.

#include <farland/base/assert.hpp>
#include <farland/proto/save_session_info.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <span>

using namespace farland;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    Reader r(std::as_bytes(std::span(data, size)));
    const auto info = proto::decode_save_session_info(r);
    if (!info) {
        return 0;
    }
    Writer first;
    proto::encode_save_session_info(first, *info);
    Reader again(first.view());
    const auto decoded = proto::decode_save_session_info(again);
    FARLAND_ASSERT(decoded.has_value() && again.empty());
    Writer second;
    proto::encode_save_session_info(second, *decoded);
    FARLAND_ASSERT(std::ranges::equal(first.view(), second.view()));
    return 0;
}
