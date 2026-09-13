// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Slow-path and fast-path input. Events that decode must re-encode to the
// same number of events in both forms.

#include <farland/base/assert.hpp>
#include <farland/proto/input.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <span>

using namespace farland;

namespace {

bool fits_fastpath(const proto::InputEvent& event)
{
    if (const auto* key = std::get_if<proto::KeyboardEvent>(&event)) {
        return key->code <= 0xFF;
    }
    if (const auto* sync = std::get_if<proto::SyncEvent>(&event)) {
        return sync->toggle_flags <= 0x1F;
    }
    return true;
}

void check(const std::vector<proto::InputEvent>& events)
{
    if (events.size() <= 0xFF && std::ranges::all_of(events, fits_fastpath)) {
        Writer w;
        proto::encode_fastpath_input(w, events);
        Reader r(w.view());
        const auto again = proto::decode_fastpath_input(r);
        FARLAND_ASSERT(again.has_value() && again->size() == events.size());
    }
    Writer slow;
    proto::encode_slow_path_input(slow, events);
    Reader r(slow.view());
    const auto again = proto::decode_slow_path_input(r);
    const auto without_qoe = std::ranges::count_if(
        events, [](const proto::InputEvent& e) { return !std::holds_alternative<proto::QoeTimestampEvent>(e); });
    FARLAND_ASSERT(again.has_value() && std::cmp_equal(again->size(), without_qoe));
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const auto input = std::as_bytes(std::span(data, size));
    Reader fast(input);
    if (auto events = proto::decode_fastpath_input(fast); events.has_value()) {
        check(*events);
    }
    Reader slow(input);
    if (auto events = proto::decode_slow_path_input(slow); events.has_value()) {
        check(*events);
    }
    return 0;
}
