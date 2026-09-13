// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Walks arbitrary input as nested BER/DER TLVs. Checks that DER values
// re-encode to exactly the bytes they were decoded from.

#include <farland/base/assert.hpp>
#include <farland/base/ber.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <span>

namespace {

using namespace farland;

constexpr int max_depth = 32;

void check_der_round_trip(const ber::Tlv& tlv, std::span<const std::byte> original)
{
    Writer w;
    if (tlv.tag == ber::tags::integer || tlv.tag == ber::tags::enumerated) {
        const auto value = ber::decode_integer(tlv, ber::Rules::der);
        if (!value.has_value()) {
            return;
        }
        ber::write_integer(w, *value, tlv.tag);
    } else if (tlv.tag == ber::tags::boolean) {
        const auto value = ber::decode_boolean(tlv, ber::Rules::der);
        if (!value.has_value()) {
            return;
        }
        ber::write_boolean(w, *value, tlv.tag);
    } else {
        ber::write_tlv(w, tlv.tag, tlv.value);
    }
    FARLAND_ASSERT(std::ranges::equal(w.view(), original));
}

void walk(Reader r, ber::Rules rules, int depth)
{
    while (!r.empty()) {
        const std::size_t start = r.position();
        const auto tlv = ber::read_tlv(r, rules);
        if (!tlv.has_value()) {
            return;
        }
        if (rules == ber::Rules::der) {
            check_der_round_trip(*tlv, r.data().subspan(start, r.position() - start));
        }
        if (tlv->tag.constructed) {
            if (depth < max_depth) {
                walk(tlv->reader(), rules, depth + 1);
            }
            continue;
        }
        static_cast<void>(ber::decode_integer(*tlv, rules));
        static_cast<void>(ber::decode_unsigned(*tlv, rules));
        static_cast<void>(ber::decode_boolean(*tlv, rules));
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const auto input = std::as_bytes(std::span(data, size));
    walk(Reader(input), ber::Rules::ber, 0);
    walk(Reader(input), ber::Rules::der, 0);
    return 0;
}
