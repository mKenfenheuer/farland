// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Static virtual channel chunks. Input: one byte that picks the chunk size,
// then records of [u16le length][Virtual Channel PDU payload] fed to a
// Reassembler. Every complete message must survive chunking and reassembly
// with the picked chunk size.

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/channels/svc.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <span>
#include <vector>

using namespace farland;
namespace svc = farland::channels::svc;

namespace {

constexpr std::size_t max_message = std::size_t{64} * 1024;

void check_round_trip(const std::vector<std::byte>& message, std::size_t max_chunk)
{
    const auto chunks = svc::encode_chunks(message, max_chunk);
    FARLAND_ASSERT(!chunks.empty());
    svc::Reassembler reassembler(message.size(), max_chunk);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        FARLAND_ASSERT(chunks[i].size() <= svc::header_size + max_chunk);
        auto result = reassembler.add(chunks[i]);
        FARLAND_ASSERT(result.has_value());
        FARLAND_ASSERT(result->has_value() == (i + 1 == chunks.size()));
        if (result->has_value()) {
            FARLAND_ASSERT(**result == message);
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    Reader r(std::as_bytes(std::span(data, size)));
    const auto mode = r.u8();
    if (!mode) {
        return 0;
    }
    const std::size_t max_chunk = 1 + (std::size_t{*mode} * 64);
    svc::Reassembler reassembler(max_message);
    while (!r.empty()) {
        const auto length = r.u16le();
        if (!length) {
            break;
        }
        const auto pdu = r.bytes(std::min<std::size_t>(*length, r.remaining())).value();
        const auto result = reassembler.add(pdu);
        if (result.has_value() && result->has_value()) {
            FARLAND_ASSERT((*result)->size() <= max_message);
            check_round_trip(**result, max_chunk);
        }
    }
    return 0;
}
