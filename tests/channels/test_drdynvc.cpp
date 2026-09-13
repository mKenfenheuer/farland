// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/channels/drdynvc.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <variant>
#include <vector>

using farland::Errc;
using farland::test::hex;
namespace dyn = farland::channels::drdynvc;

namespace {

using Bytes = std::vector<std::byte>;

Bytes filled(std::size_t size, std::uint8_t value = 0x71)
{
    return Bytes(size, std::byte{value});
}

Bytes concat(Bytes a, const Bytes& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

template <class T, class Variant>
T get(const farland::Result<Variant>& result)
{
    REQUIRE(result.has_value());
    const auto* value = std::get_if<T>(&*result);
    REQUIRE(value != nullptr);
    return *value;
}

template <class T>
Bytes encoded(const T& pdu)
{
    farland::Writer w;
    dyn::encode(w, pdu);
    return std::move(w).take();
}

}  // namespace

TEST_CASE("[MS-RDPEDYC] 4.1.1 DVC Capabilities Request (Version 2)")
{
    const auto bytes = hex("58 00 02 00 33 33 11 11 3d 0a a7 04");
    const auto caps = get<dyn::CapsRequest>(dyn::decode_server_pdu(bytes));
    CHECK(caps.version == 2);
    CHECK(caps.priority_charges == dyn::PriorityCharges{0x3333, 0x1111, 0x0a3d, 0x04a7});
    // farland leaves the unused Sp field zero.
    CHECK(encoded(caps) == hex("50 00 02 00 33 33 11 11 3d 0a a7 04"));

    const auto v1 = get<dyn::CapsRequest>(dyn::decode_server_pdu(hex("50 00 01 00")));
    CHECK(v1.version == 1);
    CHECK(v1.priority_charges == dyn::PriorityCharges{});
    CHECK(encoded(dyn::CapsRequest{1, {1, 2, 3, 4}}) == hex("50 00 01 00"));
    CHECK(encoded(dyn::CapsRequest{}) == hex("50 00 03 00 a8 03 cc 0c 92 24 55 55"));
}

TEST_CASE("[MS-RDPEDYC] 4.1.2 DVC Capabilities Response")
{
    const auto bytes = hex("50 00 02 00");
    CHECK(get<dyn::CapsResponse>(dyn::decode_client_pdu(bytes)).version == 2);
    CHECK(encoded(dyn::CapsResponse{2}) == bytes);
    // mstsc sends 0x5c: Sp is uninitialized ([MS-RDPEDYC] 6, <7>).
    CHECK(get<dyn::CapsResponse>(dyn::decode_client_pdu(hex("5c 00 03 00"))).version == 3);
}

TEST_CASE("[MS-RDPEDYC] 4.2.1 DVC Create Request")
{
    const auto bytes = hex("10 03 74 65 73 74 64 76 63 00");
    const auto create = get<dyn::CreateRequest>(dyn::decode_server_pdu(bytes));
    CHECK(create == dyn::CreateRequest{3, 0, "testdvc"});
    CHECK(encoded(create) == bytes);
    CHECK(encoded(dyn::CreateRequest{3, 2, "a"}) == hex("18 03 61 00"));
}

TEST_CASE("[MS-RDPEDYC] 4.2.2 DVC Create Response")
{
    const auto bytes = hex("10 03 00 00 00 00");
    CHECK(get<dyn::CreateResponse>(dyn::decode_client_pdu(bytes)) == dyn::CreateResponse{3, 0});
    CHECK(encoded(dyn::CreateResponse{3, 0}) == bytes);

    // CreationStatus is a signed HRESULT ([MS-RDPEDYC] 2.2.2.2).
    const auto failed = get<dyn::CreateResponse>(dyn::decode_client_pdu(hex("10 03 90 04 07 80")));
    CHECK(failed.creation_status == std::bit_cast<std::int32_t>(std::uint32_t{0x80070490}));
    CHECK(failed.creation_status < 0);
}

TEST_CASE("[MS-RDPEDYC] 4.3.1 and 4.3.2: a 3195-byte message as Data First and two Data PDUs")
{
    const auto first_bytes = concat(hex("24 03 7b 0c"), filled(1596));
    const auto first = get<dyn::DataFirst>(dyn::decode_client_pdu(first_bytes));
    CHECK(first.channel_id == 3);
    CHECK(first.length == 3195);
    CHECK(first.data.size() == 1596);
    CHECK(encoded(first) == first_bytes);

    const auto data_bytes = concat(hex("34 03"), filled(1598));
    const auto data = get<dyn::Data>(dyn::decode_client_pdu(data_bytes));
    CHECK(data.channel_id == 3);
    CHECK(data.data.size() == 1598);
    const auto last = get<dyn::Data>(dyn::decode_client_pdu(hex("34 03 71")));
    CHECK(last.data.size() == 1);

    // farland produces the same PDUs, with Sp zero.
    const auto message = filled(3195);
    const auto pdus = dyn::encode_data(3, message);
    REQUIRE(pdus.size() == 3);
    CHECK(pdus[0] == first_bytes);
    CHECK(pdus[1] == concat(hex("30 03"), filled(1598)));
    CHECK(pdus[2] == hex("30 03 71"));
}

TEST_CASE("[MS-RDPEDYC] 4.3.3 and 4.3.4: compressed data PDUs")
{
    const auto first_bytes = hex("64 03 7b 0c e0 26 38 c4 3f f4 74 01");
    const auto first = get<dyn::DataFirstCompressed>(dyn::decode_client_pdu(first_bytes));
    CHECK(first.channel_id == 3);
    CHECK(first.length == 3195);
    CHECK(std::ranges::equal(first.data, hex("e0 26 38 c4 3f f4 74 01")));
    CHECK(encoded(first) == first_bytes);

    for (const auto& bytes : {hex("70 03 e0 26 88 7f e8 f4 02"), hex("70 03 06 71 71 71")}) {
        const auto data = get<dyn::DataCompressed>(dyn::decode_client_pdu(bytes));
        CHECK(data.channel_id == 3);
        CHECK(data.data.size() == bytes.size() - 2);
        CHECK(encoded(data) == bytes);
    }
}

TEST_CASE("[MS-RDPEDYC] 4.4.1 DVC Close, in both directions")
{
    const auto bytes = hex("40 03");
    CHECK(get<dyn::Close>(dyn::decode_client_pdu(bytes)).channel_id == 3);
    CHECK(get<dyn::Close>(dyn::decode_server_pdu(bytes)).channel_id == 3);
    CHECK(encoded(dyn::Close{3}) == bytes);
}

TEST_CASE("ChannelId and Length use the smallest field ([MS-RDPEDYC] 2.2)")
{
    CHECK(dyn::size_code(0) == 0);
    CHECK(dyn::size_code(0xFF) == 0);
    CHECK(dyn::size_code(0x100) == 1);
    CHECK(dyn::size_code(0xFFFF) == 1);
    CHECK(dyn::size_code(0x10000) == 2);
    CHECK(encoded(dyn::Close{0x1234}) == hex("41 34 12"));
    CHECK(encoded(dyn::Close{0x12345678}) == hex("42 78 56 34 12"));
    CHECK(encoded(dyn::DataFirst{1, 0x10, hex("aa")}) == hex("20 01 10 aa"));
    CHECK(encoded(dyn::DataFirst{1, 0x1234, hex("aa")}) == hex("24 01 34 12 aa"));
    CHECK(encoded(dyn::DataFirst{0x100, 0x123456, hex("aa")}) == hex("29 00 01 56 34 12 00 aa"));

    // Wider fields than necessary decode too.
    CHECK(get<dyn::Data>(dyn::decode_client_pdu(hex("32 03 00 00 00 aa"))).channel_id == 3);
    CHECK(get<dyn::DataFirst>(dyn::decode_client_pdu(hex("29 03 00 02 00 00 00 aa"))).length == 2);
}

TEST_CASE("Soft-Sync Request and Response ([MS-RDPEDYC] 2.2.5)")
{
    const auto request_bytes = hex("80 00 16 00 00 00 03 00 01 00 01 00 00 00 02 00 03 00 00 00 05 00 00 00");
    const auto request = get<dyn::SoftSyncRequest>(dyn::decode_server_pdu(request_bytes));
    CHECK(request.flags == (dyn::soft_sync_flag::tcp_flushed | dyn::soft_sync_flag::channel_list_present));
    CHECK(request.number_of_tunnels == 1);
    REQUIRE(request.channel_lists.size() == 1);
    CHECK(request.channel_lists[0] ==
          dyn::SoftSyncChannelList{dyn::tunnel_type::udp_fec_reliable, std::vector<std::uint32_t>{3, 5}});
    CHECK(encoded(request) == request_bytes);

    const auto flushed_only = hex("80 00 08 00 00 00 01 00 00 00");
    CHECK(get<dyn::SoftSyncRequest>(dyn::decode_server_pdu(flushed_only)) == dyn::SoftSyncRequest{});
    CHECK(encoded(dyn::SoftSyncRequest{}) == flushed_only);

    const auto response_bytes = hex("90 00 02 00 00 00 01 00 00 00 03 00 00 00");
    const auto response = get<dyn::SoftSyncResponse>(dyn::decode_client_pdu(response_bytes));
    CHECK(response.tunnels == std::vector<std::uint32_t>{1, 3});
    CHECK(encoded(response) == response_bytes);
}

TEST_CASE("Malformed drdynvc PDUs are rejected")
{
    CHECK(dyn::decode_client_pdu({}).error().code == Errc::truncated);
    // cbId 3 is invalid.
    CHECK(dyn::decode_client_pdu(hex("33 03 aa")).error().code == Errc::invalid_value);
    // Len 3 is invalid.
    CHECK(dyn::decode_client_pdu(hex("2c 03 aa")).error().code == Errc::invalid_value);
    // Unknown commands, and commands that only the other side sends.
    CHECK(dyn::decode_client_pdu(hex("00 00")).error().code == Errc::invalid_value);
    CHECK(dyn::decode_client_pdu(hex("a0 00")).error().code == Errc::invalid_value);
    CHECK(dyn::decode_client_pdu(hex("80 00 08 00 00 00 01 00 00 00")).error().code == Errc::invalid_value);
    CHECK(dyn::decode_server_pdu(hex("90 00 00 00 00 00")).error().code == Errc::invalid_value);
    // Truncated and oversized fixed PDUs.
    CHECK(dyn::decode_client_pdu(hex("50 00 03")).error().code == Errc::truncated);
    CHECK(dyn::decode_client_pdu(hex("50 00 03 00 00")).error().code == Errc::trailing_data);
    CHECK(dyn::decode_client_pdu(hex("10 03 00 00 00")).error().code == Errc::truncated);
    CHECK(dyn::decode_client_pdu(hex("10 03 00 00 00 00 00")).error().code == Errc::trailing_data);
    CHECK(dyn::decode_client_pdu(hex("40")).error().code == Errc::truncated);
    CHECK(dyn::decode_client_pdu(hex("40 03 00")).error().code == Errc::trailing_data);
    CHECK(dyn::decode_client_pdu(hex("41 03")).error().code == Errc::truncated);
    // Data First holding more than its Length.
    CHECK(dyn::decode_client_pdu(hex("20 03 01 aa bb")).error().code == Errc::invalid_length);
    // Capabilities Request with an unknown version.
    CHECK(dyn::decode_server_pdu(hex("50 00 04 00")).error().code == Errc::invalid_value);
    CHECK(dyn::decode_server_pdu(hex("50 00 02 00 01 00")).error().code == Errc::truncated);
    // Channel names.
    CHECK(dyn::decode_server_pdu(hex("10 03 74 65")).error().code == Errc::truncated);
    CHECK(dyn::decode_server_pdu(hex("10 03 00")).error().code == Errc::invalid_value);
    CHECK(dyn::decode_server_pdu(hex("10 03 61 00 00")).error().code == Errc::trailing_data);
    CHECK(dyn::decode_server_pdu(concat(concat(hex("10 03"), filled(257, 0x61)), hex("00"))).error().code ==
          Errc::limit_exceeded);
    CHECK(get<dyn::CreateRequest>(dyn::decode_server_pdu(concat(concat(hex("10 03"), filled(256, 0x61)), hex("00"))))
              .name.size() == 256);
    // Soft-Sync counts that disagree with the PDU.
    CHECK(dyn::decode_server_pdu(hex("80 00 09 00 00 00 01 00 00 00")).error().code == Errc::invalid_length);
    CHECK(dyn::decode_server_pdu(hex("80 00 0e 00 00 00 03 00 01 00 01 00 00 00 05 00")).error().code ==
          Errc::truncated);
    CHECK(dyn::decode_client_pdu(hex("90 00 02 00 00 00 01 00 00 00")).error().code == Errc::invalid_length);
    CHECK(dyn::decode_client_pdu(hex("90 00 ff ff ff ff")).error().code == Errc::invalid_length);
}

TEST_CASE("Data PDUs never exceed 1600 bytes and reassemble ([MS-RDPEDYC] 2.2.3)")
{
    for (const std::uint32_t id : {std::uint32_t{1}, std::uint32_t{0x100}, std::uint32_t{0x10000}}) {
        for (const std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{1590}, std::size_t{1591},
                                       std::size_t{1596}, std::size_t{1597}, std::size_t{3195}, std::size_t{100000}}) {
            Bytes message(size);
            for (std::size_t i = 0; i < size; ++i) {
                message[i] = static_cast<std::byte>(i & 0xFFU);
            }
            const auto pdus = dyn::encode_data(id, message);
            dyn::MessageReassembler reassembler(size);
            std::optional<Bytes> result;
            for (const auto& pdu : pdus) {
                CHECK(pdu.size() <= dyn::max_pdu_size);
                REQUIRE_FALSE(result.has_value());
                const auto decoded = dyn::decode_client_pdu(pdu);
                REQUIRE(decoded.has_value());
                if (const auto* first = std::get_if<dyn::DataFirst>(&*decoded)) {
                    CHECK(first->channel_id == id);
                    result = reassembler.first(first->length, first->data).value();
                } else {
                    const auto& data = std::get<dyn::Data>(*decoded);
                    CHECK(data.channel_id == id);
                    result = reassembler.next(data.data).value();
                }
            }
            REQUIRE(result.has_value());
            CHECK(*result == message);
            // A single DYNVC_DATA up to 1590 bytes, Data First above.
            CHECK((size <= 1590) == std::holds_alternative<dyn::Data>(dyn::decode_client_pdu(pdus[0]).value()));
        }
    }
    // 1591 bytes fit in one Data First that carries the whole message.
    const auto pdus = dyn::encode_data(1, filled(1591));
    REQUIRE(pdus.size() == 1);
    CHECK(get<dyn::DataFirst>(dyn::decode_client_pdu(pdus[0])).data.size() == 1591);
}

TEST_CASE("Message reassembly rules ([MS-RDPEDYC] 3.1.5.2.3)")
{
    dyn::MessageReassembler reassembler(10);
    CHECK(reassembler.next(hex("aa")).value() == hex("aa"));
    CHECK(reassembler.first(1, hex("aa")).value() == hex("aa"));
    CHECK(reassembler.first(0, {}).value() == Bytes{});
    CHECK(reassembler.first(11, hex("aa")).error().code == Errc::limit_exceeded);
    CHECK(reassembler.next(Bytes(11)).error().code == Errc::limit_exceeded);
    CHECK(reassembler.first(1, hex("aa bb")).error().code == Errc::invalid_length);

    REQUIRE_FALSE(reassembler.first(4, hex("aa")).value().has_value());
    CHECK(reassembler.first(4, hex("aa")).error().code == Errc::invalid_value);
    CHECK_FALSE(reassembler.in_progress());

    REQUIRE_FALSE(reassembler.first(4, hex("aa")).value().has_value());
    CHECK(reassembler.next(hex("bb cc dd ee")).error().code == Errc::invalid_length);
    CHECK_FALSE(reassembler.in_progress());

    REQUIRE_FALSE(reassembler.first(4, hex("aa")).value().has_value());
    CHECK_FALSE(reassembler.next(hex("bb")).value().has_value());
    CHECK(reassembler.next(hex("cc dd")).value() == hex("aa bb cc dd"));
}
