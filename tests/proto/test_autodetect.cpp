// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Auto-detect and heartbeat messages, [MS-RDPBCGR] 2.2.14 and 2.2.16. The
// spec has no example bytes; these follow the field tables.

#include <farland/proto/autodetect.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

namespace ad = farland::proto::autodetect;
using farland::Errc;
using farland::Reader;
using farland::Writer;
using namespace farland::test::literals;

namespace {

std::vector<std::byte> encoded(const ad::Request& request)
{
    Writer w;
    ad::encode(w, request);
    return std::move(w).take();
}

std::vector<std::byte> encoded(const ad::Response& response)
{
    Writer w;
    ad::encode(w, response);
    return std::move(w).take();
}

ad::Request request(const std::vector<std::byte>& bytes)
{
    Reader r(bytes);
    auto decoded = ad::decode_request(r);
    REQUIRE(decoded.has_value());
    CHECK(r.empty());
    return *decoded;
}

ad::Response response(const std::vector<std::byte>& bytes)
{
    Reader r(bytes);
    auto decoded = ad::decode_response(r);
    REQUIRE(decoded.has_value());
    CHECK(r.empty());
    return *decoded;
}

Errc request_error(const std::vector<std::byte>& bytes)
{
    Reader r(bytes);
    auto decoded = ad::decode_request(r);
    REQUIRE_FALSE(decoded.has_value());
    return decoded.error().code;
}

Errc response_error(const std::vector<std::byte>& bytes)
{
    Reader r(bytes);
    auto decoded = ad::decode_response(r);
    REQUIRE_FALSE(decoded.has_value());
    return decoded.error().code;
}

}  // namespace

TEST_CASE("[MS-RDPBCGR] 2.2.14.1.1 RTT Measure Request, connect-time and continuous")
{
    const auto connect = "06 00 34 12 01 10"_hex;
    CHECK(encoded(ad::RttRequest{0x1234, ad::request_type::rtt_connect_time}) == connect);
    CHECK(std::get<ad::RttRequest>(request(connect)) == ad::RttRequest{0x1234, ad::request_type::rtt_connect_time});

    const auto continuous = "06 00 01 00 01 00"_hex;
    CHECK(encoded(ad::RttRequest{1, ad::request_type::rtt_continuous}) == continuous);
    CHECK(std::get<ad::RttRequest>(request(continuous)).request_type == ad::request_type::rtt_continuous);
}

TEST_CASE("[MS-RDPBCGR] 2.2.14.1.2 - 2.2.14.1.4 Bandwidth Measure Start, Payload and Stop")
{
    const auto start = "06 00 07 00 14 10"_hex;
    CHECK(encoded(ad::BandwidthStart{7, ad::request_type::bw_start_connect_time}) == start);
    CHECK(std::get<ad::BandwidthStart>(request(start)) ==
          ad::BandwidthStart{7, ad::request_type::bw_start_connect_time});

    const auto body = "aa bb cc"_hex;
    const auto payload = "08 00 07 00 02 00 03 00 aa bb cc"_hex;
    CHECK(encoded(ad::BandwidthPayload{7, body}) == payload);
    const auto decoded_payload = request(payload);
    const auto& p = std::get<ad::BandwidthPayload>(decoded_payload);
    CHECK(p.sequence == 7);
    CHECK(std::vector(p.payload.begin(), p.payload.end()) == body);

    // The connect-time stop carries a payload; the continuous one does not.
    const auto stop = "08 00 07 00 2b 00 03 00 aa bb cc"_hex;
    CHECK(encoded(ad::BandwidthStop{7, ad::request_type::bw_stop_connect_time, body}) == stop);
    const auto decoded_stop = request(stop);
    CHECK(std::get<ad::BandwidthStop>(decoded_stop).payload.size() == 3);

    const auto continuous_stop = "06 00 08 00 29 04"_hex;
    CHECK(encoded(ad::BandwidthStop{8, ad::request_type::bw_stop_continuous, {}}) == continuous_stop);
    CHECK(std::get<ad::BandwidthStop>(request(continuous_stop)).request_type == ad::request_type::bw_stop_continuous);
}

TEST_CASE("[MS-RDPBCGR] 2.2.14.1.5 Network Characteristics Result: the fields decide the requestType")
{
    const ad::NetworkCharacteristicsResult all{9, 12, 50'000, 20};
    const auto all_bytes = "12 00 09 00 c0 08 0c 00 00 00 50 c3 00 00 14 00 00 00"_hex;
    CHECK(encoded(all) == all_bytes);
    CHECK(std::get<ad::NetworkCharacteristicsResult>(request(all_bytes)) == all);

    const ad::NetworkCharacteristicsResult rtt_only{9, 12, std::nullopt, 20};
    const auto rtt_bytes = "0e 00 09 00 40 08 0c 00 00 00 14 00 00 00"_hex;
    CHECK(encoded(rtt_only) == rtt_bytes);
    CHECK(std::get<ad::NetworkCharacteristicsResult>(request(rtt_bytes)) == rtt_only);

    const ad::NetworkCharacteristicsResult bandwidth_only{9, std::nullopt, 50'000, 20};
    const auto bandwidth_bytes = "0e 00 09 00 80 08 50 c3 00 00 14 00 00 00"_hex;
    CHECK(encoded(bandwidth_only) == bandwidth_bytes);
    CHECK(std::get<ad::NetworkCharacteristicsResult>(request(bandwidth_bytes)) == bandwidth_only);
}

TEST_CASE("[MS-RDPBCGR] 2.2.14.2 RTT Measure Response, Bandwidth Measure Results, Network Characteristics Sync")
{
    const auto rtt = "06 01 34 12 00 00"_hex;
    CHECK(encoded(ad::RttResponse{0x1234}) == rtt);
    CHECK(std::get<ad::RttResponse>(response(rtt)) == ad::RttResponse{0x1234});

    const ad::BandwidthResults results{7, ad::response_type::bw_results_connect_time, 25, 65536};
    const auto results_bytes = "0e 01 07 00 03 00 19 00 00 00 00 00 01 00"_hex;
    CHECK(encoded(results) == results_bytes);
    CHECK(std::get<ad::BandwidthResults>(response(results_bytes)) == results);

    const ad::NetworkCharacteristicsSync sync{3, 10'000, 40};
    const auto sync_bytes = "0e 01 03 00 18 00 10 27 00 00 28 00 00 00"_hex;
    CHECK(encoded(sync) == sync_bytes);
    CHECK(std::get<ad::NetworkCharacteristicsSync>(response(sync_bytes)) == sync);
}

TEST_CASE("Malformed auto-detect messages are errors")
{
    // Truncated headers and bodies.
    CHECK(request_error("06 00 01"_hex) == Errc::truncated);
    CHECK(response_error("0e 01 07 00 03 00 19 00"_hex) == Errc::truncated);
    CHECK(request_error("08 00 07 00 02 00 05 00 aa bb"_hex) == Errc::truncated);

    // A request where a response belongs, and the other way round.
    CHECK(request_error("06 01 01 00 01 00"_hex) == Errc::invalid_value);
    CHECK(response_error("06 00 01 00 00 00"_hex) == Errc::invalid_value);

    // headerLength that does not match the type.
    CHECK(request_error("08 00 01 00 01 00"_hex) == Errc::invalid_length);
    CHECK(request_error("06 00 07 00 2b 00 01 00 aa"_hex) == Errc::invalid_length);
    CHECK(request_error("0e 00 09 00 c0 08 0c 00 00 00 50 c3 00 00 14 00 00 00"_hex) == Errc::invalid_length);
    CHECK(response_error("06 01 07 00 0b 00 19 00 00 00 00 00 01 00"_hex) == Errc::invalid_length);

    // The connect-time stop must carry a payload.
    CHECK(request_error("08 00 07 00 2b 00 00 00"_hex) == Errc::invalid_length);

    // Unknown types.
    CHECK(request_error("06 00 01 00 77 00"_hex) == Errc::unsupported);
    CHECK(response_error("06 01 01 00 77 00"_hex) == Errc::unsupported);
}

TEST_CASE("[MS-RDPBCGR] 2.2.16.1 Heartbeat PDU fields")
{
    const auto bytes = "00 05 03 05"_hex;
    Writer w;
    ad::encode(w, ad::Heartbeat{5, 3, 5});
    CHECK(std::move(w).take() == bytes);
    Reader r(bytes);
    CHECK(ad::decode_heartbeat(r).value() == ad::Heartbeat{5, 3, 5});
    const auto truncated = "00 05"_hex;
    Reader short_input(truncated);
    CHECK_FALSE(ad::decode_heartbeat(short_input).has_value());
}
