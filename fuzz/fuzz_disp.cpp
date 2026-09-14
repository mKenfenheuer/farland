// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Display Control PDUs ([MS-RDPEDISP]). Input: one byte that picks the
// server's monitor limit, then one DVC message. A PDU that decodes must
// encode to the same bytes, and a monitor layout that decodes goes through
// the server's validation, which must stay inside the limits it enforces.

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/channels/disp.hpp>
#include <farland/server/display_layout.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <span>
#include <vector>

using namespace farland;
namespace disp = farland::channels::disp;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const auto input = std::as_bytes(std::span(data, size));
    if (input.empty()) {
        return 0;
    }
    const std::uint32_t limit = 1 + (std::to_integer<std::uint32_t>(input[0]) % disp::max_monitors);
    const auto message = input.subspan(1);
    const auto pdu = disp::decode(message, limit);
    if (!pdu) {
        return 0;
    }
    const auto encoded = disp::encode(*pdu);
    FARLAND_ASSERT(std::ranges::equal(encoded, message));
    if (const auto* layout = std::get_if<disp::MonitorLayoutPdu>(&*pdu)) {
        FARLAND_ASSERT(!layout->monitors.empty() && layout->monitors.size() <= limit);
        server::DisplayLimits limits;
        limits.max_monitors = limit;
        if (const auto checked = server::DisplayLayout::from_disp(*layout, limits)) {
            FARLAND_ASSERT(checked->monitors().size() <= limit);
            FARLAND_ASSERT(checked->width() <= limits.max_extent && checked->height() <= limits.max_extent);
            FARLAND_ASSERT(checked->area() <= limits.max_area());
            for (const auto& monitor : checked->monitors()) {
                FARLAND_ASSERT(monitor.rect.width >= limits.min_size && monitor.rect.width <= limits.max_size);
                FARLAND_ASSERT(monitor.rect.x + monitor.rect.width <= checked->width());
                FARLAND_ASSERT(monitor.rect.y + monitor.rect.height <= checked->height());
            }
        }
    }
    return 0;
}
