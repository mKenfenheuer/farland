// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/hexdump.hpp>
#include <farland/proto/x224.hpp>

#include "support/bytes.hpp"
#include "support/transcript.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

namespace proto = farland::proto;
using farland::Errc;
using farland::Reader;
using farland::to_hex;
using farland::Writer;
using farland::test::ascii;
using farland::test::hex;

namespace {

farland::Result<proto::ConnectionRequest> decode_cr(std::span<const std::byte> packet)
{
    Reader r(packet);
    FARLAND_TRY(Reader tpdu, proto::read_tpkt(r));
    return proto::decode_connection_request(tpdu);
}

}  // namespace

TEST_CASE("Connection Request without cookie (mstsc)")
{
    const auto records =
        farland::test::load_transcript(FARLAND_TEST_DATA_DIR "/transcripts/x224-connection-request.txt");
    const auto request = decode_cr(records.at(0).bytes).value();
    CHECK(request.cookie.empty());
    CHECK(request.routing_token.empty());
    REQUIRE(request.negotiation.has_value());
    CHECK(request.negotiation->flags == 0);
    CHECK(request.negotiation->requested_protocols ==
          (proto::protocol::ssl | proto::protocol::hybrid | proto::protocol::hybrid_ex));
    CHECK_FALSE(request.correlation_id.has_value());
}

TEST_CASE("Connection Request with an mstshash cookie ([MS-RDPBCGR] 2.2.1.1)")
{
    proto::ConnectionRequest request;
    request.cookie = "eltons";
    request.negotiation = proto::ConnectionRequest::Negotiation{0, proto::protocol::ssl | proto::protocol::hybrid};

    Writer w;
    proto::encode_connection_request(w, request);
    // LI = 6 fixed + 25 cookie + 8 RDP_NEG_REQ = 39; TPKT length = 4 + 1 + 39 = 44.
    CHECK(to_hex(w.view()) == "03 00 00 2c 27 e0 00 00 00 00 00 "
                              "43 6f 6f 6b 69 65 3a 20 6d 73 74 73 68 61 73 68 3d 65 6c 74 6f 6e 73 0d 0a "
                              "01 00 08 00 03 00 00 00");

    const auto decoded = decode_cr(w.view()).value();
    CHECK(decoded.cookie == "eltons");
    CHECK(decoded.routing_token.empty());
    CHECK(decoded.negotiation->requested_protocols == 3);
}

TEST_CASE("Connection Request with a load-balancing routing token")
{
    proto::ConnectionRequest request;
    const std::string_view token = "Cookie: msts=3640205228.15629.0000";
    request.routing_token.assign(ascii(token).begin(), ascii(token).end());
    request.negotiation = proto::ConnectionRequest::Negotiation{0, proto::protocol::ssl};

    Writer w;
    proto::encode_connection_request(w, request);
    const auto decoded = decode_cr(w.view()).value();
    CHECK(decoded.cookie.empty());
    CHECK(decoded.routing_token == request.routing_token);
    CHECK(decoded.negotiation->requested_protocols == proto::protocol::ssl);
}

TEST_CASE("Connection Request with correlation info ([MS-RDPBCGR] 2.2.1.1.2)")
{
    proto::ConnectionRequest request;
    request.negotiation = proto::ConnectionRequest::Negotiation{0, proto::protocol::hybrid_ex};
    std::array<std::byte, 16> id{};
    for (std::size_t i = 0; i < id.size(); ++i) {
        id.at(i) = static_cast<std::byte>(i + 1);
    }
    request.correlation_id = id;

    Writer w;
    proto::encode_connection_request(w, request);
    const auto decoded = decode_cr(w.view()).value();
    CHECK(decoded.negotiation->flags == proto::neg_req_flags::correlation_info_present);
    REQUIRE(decoded.correlation_id.has_value());
    CHECK(*decoded.correlation_id == id);
}

TEST_CASE("Malformed Connection Requests are rejected")
{
    // LI claims one byte more than present.
    CHECK(decode_cr(hex("03 00 00 13 0f e0 00 00 00 00 00 01 00 08 00 0b 00 00 00")).error().code ==
          Errc::invalid_length);
    // Cookie without CRLF.
    CHECK(decode_cr(hex("03 00 00 0f 0a e0 00 00 00 00 00 43 6f 6f 6b")).error().code == Errc::invalid_value);
    // RDP_NEG_REQ length 9.
    CHECK(decode_cr(hex("03 00 00 13 0e e0 00 00 00 00 00 01 00 09 00 0b 00 00 00")).error().code ==
          Errc::invalid_length);
    // Class 4.
    CHECK(decode_cr(hex("03 00 00 13 0e e0 00 00 00 00 04 01 00 08 00 0b 00 00 00")).error().code == Errc::unsupported);
    // Trailing byte after RDP_NEG_REQ.
    CHECK(decode_cr(hex("03 00 00 14 0f e0 00 00 00 00 00 01 00 08 00 0b 00 00 00 ff")).error().code ==
          Errc::trailing_data);
}

TEST_CASE("Connection Confirm with RDP_NEG_RSP and RDP_NEG_FAILURE ([MS-RDPBCGR] 2.2.1.2)")
{
    Writer w;
    proto::encode_connection_confirm(
        w, {proto::NegotiationResponse{proto::neg_rsp_flags::extended_client_data_supported, proto::protocol::ssl}});
    CHECK(to_hex(w.view()) == "03 00 00 13 0e d0 00 00 12 34 00 02 01 08 00 01 00 00 00");

    Reader r(w.view());
    Reader tpdu = proto::read_tpkt(r).value();
    CHECK(proto::peek_tpdu_code(tpdu).value() == proto::TpduCode::connection_confirm);
    const auto confirm = proto::decode_connection_confirm(tpdu).value();
    const auto& response = std::get<proto::NegotiationResponse>(confirm.result);
    CHECK(response.selected_protocol == proto::protocol::ssl);
    CHECK(response.flags == proto::neg_rsp_flags::extended_client_data_supported);

    Writer failure;
    proto::encode_connection_confirm(failure, {proto::NegotiationFailureCode::ssl_required_by_server});
    CHECK(to_hex(failure.view()) == "03 00 00 13 0e d0 00 00 12 34 00 03 00 08 00 01 00 00 00");
}

TEST_CASE("Data TPDU framing")
{
    Writer w;
    const auto start = proto::begin_data_tpdu(w);
    w.u8(0xaa);
    proto::end_tpkt(w, start);
    CHECK(to_hex(w.view()) == "03 00 00 08 02 f0 80 aa");

    Reader r(w.view());
    Reader tpdu = proto::read_tpkt(r).value();
    CHECK(proto::peek_tpdu_code(tpdu).value() == proto::TpduCode::data);
    Reader data = proto::decode_data_tpdu(tpdu).value();
    CHECK(to_hex(data.rest()) == "aa");

    const auto bad = hex("03 00 00 08 02 f0 00 aa");
    Reader bad_reader(bad);
    Reader bad_tpdu = proto::read_tpkt(bad_reader).value();
    CHECK(proto::decode_data_tpdu(bad_tpdu).error().code == Errc::invalid_value);
}
