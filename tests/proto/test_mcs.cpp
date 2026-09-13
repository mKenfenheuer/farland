// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/ber.hpp>
#include <farland/base/hexdump.hpp>
#include <farland/proto/mcs.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

namespace mcs = farland::proto::mcs;
using farland::Errc;
using farland::Reader;
using farland::to_hex;
using farland::Writer;
using farland::test::hex;

namespace {

template <class Pdu>
std::string encoded(const Pdu& pdu)
{
    Writer w;
    mcs::encode(w, pdu);
    return to_hex(w.view());
}

/// Decodes from `storage`, which must outlive the result (it holds spans into it).
mcs::DomainPdu decode(std::vector<std::byte>& storage, std::string_view text)
{
    storage = hex(text);
    Reader r(storage);
    return mcs::decode_domain_pdu(r).value();
}

}  // namespace

TEST_CASE("Connect-Initial round trip ([MS-RDPBCGR] 2.2.1.3)")
{
    const std::array selector{std::byte{0x01}};
    const std::array user_data{std::byte{0xde}, std::byte{0xad}};
    mcs::ConnectInitial pdu;
    pdu.calling_domain_selector = selector;
    pdu.called_domain_selector = selector;
    pdu.target = {34, 2, 0, 1, 0, 1, 65535, 2};
    pdu.minimum = {1, 1, 1, 1, 0, 1, 1056, 2};
    pdu.maximum = {65535, 65535, 65535, 1, 0, 1, 65535, 2};
    pdu.user_data = user_data;

    Writer w;
    mcs::encode_connect_initial(w, pdu);
    Reader r(w.view());
    const auto decoded = mcs::decode_connect_initial(r).value();
    CHECK(r.empty());
    CHECK(decoded.upward_flag);
    CHECK(decoded.target == pdu.target);
    CHECK(decoded.minimum == pdu.minimum);
    CHECK(decoded.maximum == pdu.maximum);
    CHECK(to_hex(decoded.user_data) == "de ad");
}

TEST_CASE("Connect-Response round trip ([MS-RDPBCGR] 2.2.1.4)")
{
    const std::array user_data{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    mcs::ConnectResponse pdu;
    pdu.parameters = {34, 3, 0, 1, 0, 1, 65528, 2};
    pdu.user_data = user_data;

    Writer w;
    mcs::encode_connect_response(w, pdu);
    CHECK(to_hex(w.view()).starts_with("7f 66"));
    Reader r(w.view());
    const auto decoded = mcs::decode_connect_response(r).value();
    CHECK(decoded.result == mcs::ResultCode::successful);
    CHECK(decoded.called_connect_id == 0);
    CHECK(decoded.parameters == pdu.parameters);
    CHECK(to_hex(decoded.user_data) == "01 02 03");
}

TEST_CASE("Domain parameters are negotiated like FreeRDP's server")
{
    mcs::ConnectInitial pdu;
    pdu.target = {34, 2, 0, 1, 0, 1, 65535, 2};
    pdu.maximum = {65535, 64535, 65535, 1, 0, 1, 65535, 2};
    const auto p = mcs::negotiate_domain_parameters(pdu);
    CHECK(p == mcs::DomainParameters{34, 3, 0, 1, 0, 1, 65528, 2});

    pdu.target.max_channel_ids = 2;
    pdu.target.max_mcs_pdu_size = 100;
    pdu.maximum.max_mcs_pdu_size = 4096;
    const auto small = mcs::negotiate_domain_parameters(pdu);
    CHECK(small.max_channel_ids == 4);
    CHECK(small.max_mcs_pdu_size == 4096);
}

TEST_CASE("Domain PDU encodings ([MS-RDPBCGR] 2.2.1.5 - 2.2.1.9)")
{
    CHECK(encoded(mcs::ErectDomainRequest{}) == "04 01 00 01 00");
    CHECK(encoded(mcs::AttachUserRequest{}) == "28");
    CHECK(encoded(mcs::AttachUserConfirm{mcs::ResultCode::successful, 1007}) == "2e 00 00 06");
    CHECK(encoded(mcs::ChannelJoinRequest{1007, 1003}) == "38 00 06 03 eb");
    CHECK(encoded(mcs::ChannelJoinConfirm{mcs::ResultCode::successful, 1007, 1003, 1003}) == "3e 00 00 06 03 eb 03 eb");
    CHECK(encoded(mcs::DisconnectProviderUltimatum{mcs::DisconnectReason::user_requested}) == "21 80");

    const std::array payload{std::byte{0xaa}, std::byte{0xbb}};
    CHECK(encoded(mcs::SendDataRequest{1007, 1003, payload}) == "64 00 06 03 eb 70 02 aa bb");
    CHECK(encoded(mcs::SendDataIndication{1007, 1003, payload}) == "68 00 06 03 eb 70 02 aa bb");
}

TEST_CASE("Domain PDUs decode to the right alternatives")
{
    std::vector<std::byte> storage;
    CHECK(std::holds_alternative<mcs::ErectDomainRequest>(decode(storage, "04 01 00 01 00")));
    CHECK(std::holds_alternative<mcs::AttachUserRequest>(decode(storage, "28")));

    const auto confirm = std::get<mcs::AttachUserConfirm>(decode(storage, "2e 00 00 06"));
    CHECK(confirm.initiator == 1007);

    const auto join = std::get<mcs::ChannelJoinRequest>(decode(storage, "38 00 06 03 eb"));
    CHECK(join.initiator == 1007);
    CHECK(join.channel_id == 1003);

    const auto join_confirm = std::get<mcs::ChannelJoinConfirm>(decode(storage, "3e 00 00 06 03 eb 03 eb"));
    CHECK(join_confirm.channel_id == 1003);

    const auto ultimatum = std::get<mcs::DisconnectProviderUltimatum>(decode(storage, "21 80"));
    CHECK(ultimatum.reason == mcs::DisconnectReason::user_requested);

    const auto data = std::get<mcs::SendDataRequest>(decode(storage, "64 00 06 03 eb 70 02 aa bb"));
    CHECK(data.initiator == 1007);
    CHECK(data.channel_id == 1003);
    CHECK(to_hex(data.data) == "aa bb");
}

TEST_CASE("Malformed domain PDUs are rejected")
{
    const auto check = [](std::string_view text, Errc expected) {
        const auto bytes = hex(text);
        Reader r(bytes);
        const auto pdu = mcs::decode_domain_pdu(r);
        REQUIRE_FALSE(pdu.has_value());
        CHECK(pdu.error().code == expected);
    };
    check("64 00 06 03 eb 70 05 aa bb", Errc::truncated);  // data length beyond the PDU
    check("28 00", Errc::trailing_data);                   // Attach User Request is one octet
    check("7c", Errc::unsupported);                        // choice 31
    check("22 80", Errc::invalid_value);                   // reason 5
    check("2e 10 00 06", Errc::invalid_value);             // result 16
}

TEST_CASE("Domain parameters as mstsc encodes them (65535 without a sign octet)")
{
    // targetParameters from [MS-RDPBCGR] 4.1.3.
    const auto bytes = hex("30 19 02 01 22 02 01 02 02 01 00 02 01 01 02 01 00 02 01 01 02 02 ff ff 02 01 02");
    // Decode through a full Connect-Initial built around these parameters.
    Writer w;
    farland::ber::write_constructed(w, farland::ber::application(101), [&bytes](Writer& body) {
        const std::array selector{std::byte{0x01}};
        farland::ber::write_octet_string(body, selector);
        farland::ber::write_octet_string(body, selector);
        farland::ber::write_boolean(body, true);
        body.bytes(bytes);
        body.bytes(bytes);
        body.bytes(bytes);
        farland::ber::write_octet_string(body, {});
    });
    Reader ci(w.view());
    const auto decoded = mcs::decode_connect_initial(ci).value();
    CHECK(decoded.target.max_mcs_pdu_size == 65535);
    CHECK(decoded.target.max_channel_ids == 34);
}
