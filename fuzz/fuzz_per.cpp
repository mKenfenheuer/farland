// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Interprets the input as a stream of (operation, operand bytes) and runs the
// PER decoders on it. Every value that decodes must re-encode and decode back
// to the same value.

#include <farland/base/assert.hpp>
#include <farland/base/per.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <span>

namespace {

using namespace farland;

constexpr std::uint16_t channel_id_min = 1001;
constexpr std::size_t h221_key_min = 4;

template <class T, class Encode, class Decode>
void round_trip(const T& value, Encode encode, Decode decode)
{
    Writer w;
    encode(w, value);
    Reader r(w.view());
    const auto decoded = decode(r);
    FARLAND_ASSERT(decoded.has_value() && *decoded == value && r.empty());
}

bool step(Reader& r, std::uint8_t op)
{
    switch (op % 10U) {
    case 0: {
        const auto v = per::read_length(r);
        if (v.has_value()) {
            round_trip(*v, per::write_length, per::read_length);
        }
        return v.has_value();
    }
    case 1: {
        const auto v = per::read_integer(r);
        if (v.has_value()) {
            round_trip(*v, per::write_integer, per::read_integer);
        }
        return v.has_value();
    }
    case 2: {
        const auto v = per::read_integer16(r, channel_id_min);
        if (v.has_value()) {
            round_trip(
                *v, [](Writer& w, std::uint16_t x) { per::write_integer16(w, x, channel_id_min); },
                [](Reader& in) { return per::read_integer16(in, channel_id_min); });
        }
        return v.has_value();
    }
    case 3:
        return per::read_enumerated(r, 16).has_value();
    case 4: {
        const auto v = per::read_object_identifier(r);
        if (v.has_value()) {
            round_trip(*v, per::write_object_identifier, per::read_object_identifier);
        }
        return v.has_value();
    }
    case 5: {
        const auto v = per::read_octet_string(r, h221_key_min);
        if (v.has_value()) {
            Writer w;
            per::write_octet_string(w, *v, h221_key_min);
            Reader back(w.view());
            const auto decoded = per::read_octet_string(back, h221_key_min);
            FARLAND_ASSERT(decoded.has_value() && std::ranges::equal(*decoded, *v) && back.empty());
        }
        return v.has_value();
    }
    case 6:
        return per::read_numeric_string(r, 1).has_value();
    case 7:
        return per::read_padding(r, 1).has_value();
    case 8:
        return per::read_choice(r).has_value() && per::read_selection(r).has_value();
    default:
        return per::read_number_of_sets(r).has_value();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    Reader r(std::as_bytes(std::span(data, size)));
    while (!r.empty()) {
        const auto op = r.u8();
        if (!op.has_value() || !step(r, *op)) {
            break;
        }
    }
    return 0;
}
