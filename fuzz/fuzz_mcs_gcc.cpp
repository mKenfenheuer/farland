// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// MCS and GCC: the input is tried as a Connect-Initial (with the GCC
// Conference Create Request and client data inside), a Connect-Response and
// a domain PDU. Client data that decodes must survive a round trip.

#include <farland/base/assert.hpp>
#include <farland/proto/gcc.hpp>
#include <farland/proto/mcs.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <span>

namespace {

using namespace farland;
namespace gcc = proto::gcc;
namespace mcs = proto::mcs;

void check_client_data(const gcc::ClientData& data)
{
    // Names longer than the wire fields are truncated on encode; compare only
    // what survives unchanged.
    Writer w;
    gcc::encode_client_data(w, data);
    Reader r(w.view());
    const auto again = gcc::decode_client_data(r);
    FARLAND_ASSERT(again.has_value());
    FARLAND_ASSERT(again->core.desktop_width == data.core.desktop_width);
    FARLAND_ASSERT(again->core.early_capability_flags == data.core.early_capability_flags);
    FARLAND_ASSERT(again->network.has_value() == data.network.has_value());
    if (data.network) {
        FARLAND_ASSERT(again->network->channels.size() == data.network->channels.size());
    }
    FARLAND_ASSERT(again->monitor.has_value() == data.monitor.has_value());
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const auto input = std::as_bytes(std::span(data, size));

    Reader initial_reader(input);
    if (auto initial = mcs::decode_connect_initial(initial_reader); initial.has_value()) {
        static_cast<void>(mcs::negotiate_domain_parameters(*initial));
        Reader user_data(initial->user_data);
        if (auto blocks = gcc::decode_conference_create_request(user_data); blocks.has_value()) {
            Reader block_reader(*blocks);
            if (auto client = gcc::decode_client_data(block_reader); client.has_value()) {
                bool names_fit =
                    !client->network || std::ranges::all_of(client->network->channels,
                                                            [](const gcc::ChannelDef& c) { return c.name.size() < 8; });
                if (names_fit) {
                    check_client_data(*client);
                }
            }
        }
    }

    Reader response_reader(input);
    if (auto response = mcs::decode_connect_response(response_reader); response.has_value()) {
        Reader user_data(response->user_data);
        if (auto blocks = gcc::decode_conference_create_response(user_data); blocks.has_value()) {
            Reader block_reader(*blocks);
            static_cast<void>(gcc::decode_server_data(block_reader));
        }
    }

    // The raw input as client data blocks and as a domain PDU.
    Reader blocks(input);
    static_cast<void>(gcc::decode_client_data(blocks));
    Reader server_blocks(input);
    static_cast<void>(gcc::decode_server_data(server_blocks));
    Reader domain(input);
    static_cast<void>(mcs::decode_domain_pdu(domain));
    return 0;
}
