// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Framing and X.224: Connection Request/Confirm must never crash, and a
// Connection Request that decodes must re-encode to one that decodes the same.

#include <farland/base/assert.hpp>
#include <farland/proto/framing.hpp>
#include <farland/proto/x224.hpp>

#include "fuzz.hpp"

#include <span>

namespace {

using namespace farland;

void check_request(const proto::ConnectionRequest& request)
{
    // Tokens that contain CRLF cannot round-trip; the decoder stops at the first one.
    Writer w;
    proto::encode_connection_request(w, request);
    Reader r(w.view());
    auto tpdu = proto::read_tpkt(r);
    FARLAND_ASSERT(tpdu.has_value());
    const auto again = proto::decode_connection_request(*tpdu);
    FARLAND_ASSERT(again.has_value());
    FARLAND_ASSERT(again->cookie == request.cookie);
    FARLAND_ASSERT(again->negotiation.has_value() == request.negotiation.has_value());
    if (request.negotiation) {
        FARLAND_ASSERT(again->negotiation->requested_protocols == request.negotiation->requested_protocols);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const auto input = std::as_bytes(std::span(data, size));
    static_cast<void>(proto::peek_frame(input));

    Reader r(input);
    auto tpdu = proto::read_tpkt(r);
    if (!tpdu) {
        return 0;
    }
    const Reader start = *tpdu;
    static_cast<void>(proto::peek_tpdu_code(start));
    if (auto request = proto::decode_connection_request(*tpdu); request.has_value()) {
        // A cookie that happens to start like the mstshash prefix is re-encoded as
        // a cookie; skip inputs where that changes the meaning.
        if (request->routing_token.empty() || request->cookie.empty()) {
            check_request(*request);
        }
    }
    Reader confirm = start;
    static_cast<void>(proto::decode_connection_confirm(confirm));
    Reader data_tpdu = start;
    static_cast<void>(proto::decode_data_tpdu(data_tpdu));
    return 0;
}
