// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Broker protocol: farlandd parses what a (possibly compromised) agent
// sends, and the agent what farlandd sends. Input: records of one byte whose
// bit 0 says whether a descriptor came along, then a framed message. Every
// frame must decode or fail cleanly; decoding is canonical, so a decoded
// message must encode to exactly its frame; and farlandd's AgentLink must
// accept nothing before a hello with the right token.

#include <farland/base/assert.hpp>
#include <farland/server/broker.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <span>

namespace broker = farland::server::broker;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    auto input = std::as_bytes(std::span(data, size));
    broker::Token token{};
    token.fill(std::byte{0x42});
    broker::AgentLink link(token);
    bool greeted = false;
    while (input.size() > 1) {
        const bool with_fd = (std::to_integer<unsigned>(input.front()) & 1U) != 0;
        input = input.subspan(1);
        const auto length = broker::message_length(input);
        if (!length || !*length) {
            break;
        }
        const auto frame = input.first(**length);
        input = input.subspan(**length);

        const auto message = broker::decode(frame);
        if (message.has_value()) {
            FARLAND_ASSERT(std::ranges::equal(broker::encode(*message), frame));
            static_cast<void>(broker::decode_from(broker::Sender::daemon, frame, with_fd));
        }
        const auto accepted = link.receive(frame, with_fd);
        if (!accepted.has_value()) {
            break;  // farlandd drops a misbehaving agent
        }
        FARLAND_ASSERT(message.has_value());
        if (std::holds_alternative<broker::Hello>(*accepted)) {
            FARLAND_ASSERT(!greeted);
            FARLAND_ASSERT(std::get<broker::Hello>(*message).token == token);
            greeted = true;
        } else {
            FARLAND_ASSERT(greeted);
            FARLAND_ASSERT(!std::holds_alternative<broker::NewConnection>(*accepted));
            FARLAND_ASSERT(!with_fd);
        }
    }
    return 0;
}
