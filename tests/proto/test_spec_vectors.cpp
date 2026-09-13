// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Decodes the annotated examples of [MS-RDPBCGR] section 4 (tests/data/spec)
// and checks that farland's encoders reproduce them byte for byte where the
// PDU carries no encryption-specific content.

#include <farland/base/hexdump.hpp>
#include <farland/proto/mcs.hpp>
#include <farland/proto/x224.hpp>

#include "support/transcript.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace proto = farland::proto;
namespace mcs = farland::proto::mcs;
using farland::Reader;
using farland::to_hex;
using farland::Writer;

namespace {

std::vector<std::byte> spec(const std::string& name)
{
    const auto records = farland::test::load_transcript(std::string(FARLAND_TEST_DATA_DIR) + "/spec/" + name);
    REQUIRE(records.size() == 1);
    return records[0].bytes;
}

/// The MCS PDU inside a TPKT + X.224 Data TPDU.
mcs::DomainPdu decode_domain(std::span<const std::byte> packet)
{
    Reader r(packet);
    Reader tpdu = proto::read_tpkt(r).value();
    Reader data = proto::decode_data_tpdu(tpdu).value();
    return mcs::decode_domain_pdu(data).value();
}

template <class Pdu>
std::string wrapped(const Pdu& pdu)
{
    Writer w;
    const auto start = proto::begin_data_tpdu(w);
    mcs::encode(w, pdu);
    proto::end_tpkt(w, start);
    return to_hex(w.view());
}

}  // namespace

TEST_CASE("4.1.1 Client X.224 Connection Request")
{
    const auto bytes = spec("bcgr-4.1.1-x224-connection-request.txt");
    Reader r(bytes);
    Reader tpdu = proto::read_tpkt(r).value();
    const auto request = proto::decode_connection_request(tpdu).value();
    CHECK(request.cookie == "eltons");
    REQUIRE(request.negotiation.has_value());
    CHECK(request.negotiation->requested_protocols == proto::protocol::rdp);

    Writer w;
    proto::encode_connection_request(w, request);
    CHECK(to_hex(w.view()) == to_hex(bytes));
}

TEST_CASE("4.1.2 Server X.224 Connection Confirm")
{
    Writer w;
    proto::encode_connection_confirm(w, {proto::NegotiationResponse{0, proto::protocol::rdp}});
    CHECK(to_hex(w.view()) == to_hex(spec("bcgr-4.1.2-x224-connection-confirm.txt")));
}

TEST_CASE("4.1.5 - 4.1.7 Erect Domain and Attach User")
{
    const auto erect = spec("bcgr-4.1.5-mcs-erect-domain-request.txt");
    const auto erect_pdu = std::get<mcs::ErectDomainRequest>(decode_domain(erect));
    CHECK(wrapped(erect_pdu) == to_hex(erect));

    const auto attach = spec("bcgr-4.1.6-mcs-attach-user-request.txt");
    CHECK(std::holds_alternative<mcs::AttachUserRequest>(decode_domain(attach)));
    CHECK(wrapped(mcs::AttachUserRequest{}) == to_hex(attach));

    const auto confirm = spec("bcgr-4.1.7-mcs-attach-user-confirm.txt");
    const auto confirm_pdu = std::get<mcs::AttachUserConfirm>(decode_domain(confirm));
    CHECK(confirm_pdu.initiator == 1007);
    CHECK(wrapped(confirm_pdu) == to_hex(confirm));
}

TEST_CASE("4.1.8 Channel Join Request and Confirm for every channel")
{
    const std::vector<std::pair<std::string, std::uint16_t>> channels{
        {"4.1.8.1", 1007}, {"4.1.8.2", 1003}, {"4.1.8.3", 1004}, {"4.1.8.4", 1005}, {"4.1.8.5", 1006}};
    for (const auto& [section, id] : channels) {
        INFO("channel " << id);
        const auto suffix = std::to_string(id) + ".txt";
        const auto request = spec("bcgr-" + section + ".1-mcs-channel-join-request-" + suffix);
        const auto request_pdu = std::get<mcs::ChannelJoinRequest>(decode_domain(request));
        CHECK(request_pdu.initiator == 1007);
        CHECK(request_pdu.channel_id == id);
        CHECK(wrapped(request_pdu) == to_hex(request));

        const auto confirm = spec("bcgr-" + section + ".2-mcs-channel-join-confirm-" + suffix);
        const auto confirm_pdu = std::get<mcs::ChannelJoinConfirm>(decode_domain(confirm));
        CHECK(confirm_pdu.result == mcs::ResultCode::successful);
        CHECK(confirm_pdu.requested == id);
        CHECK(confirm_pdu.channel_id == id);
        CHECK(wrapped(confirm_pdu) == to_hex(confirm));
    }
}

TEST_CASE("4.2.3 Disconnect Provider Ultimatum")
{
    const auto bytes = spec("bcgr-4.2.3-disconnect-provider-ultimatum.txt");
    const auto pdu = std::get<mcs::DisconnectProviderUltimatum>(decode_domain(bytes));
    CHECK(pdu.reason == mcs::DisconnectReason::user_requested);
    CHECK(wrapped(pdu) == to_hex(bytes));
}
