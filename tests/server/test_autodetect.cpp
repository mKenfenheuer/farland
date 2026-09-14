// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The auto-detect schedule and estimates ([MS-RDPBCGR] 3.3.5.14), driven
// with made-up times.

#include <farland/server/autodetect.hpp>

#include <catch2/catch_test_macros.hpp>

namespace ad = farland::proto::autodetect;
using farland::server::AutoDetect;
using namespace std::chrono_literals;

namespace {

const AutoDetect::Clock::time_point t0 = AutoDetect::Clock::time_point{} + 1h;

}  // namespace

TEST_CASE("Connect-time detection: an RTT probe, then a payload burst between Start and Stop")
{
    AutoDetect::Config config;
    config.connect_time_bytes = 40'000;
    AutoDetect detect(config);
    const auto requests = detect.start_connect_time(t0);

    REQUIRE(requests.size() >= 4);
    const auto rtt = std::get<ad::RttRequest>(requests[0]);
    CHECK(rtt.request_type == ad::request_type::rtt_connect_time);
    const auto start = std::get<ad::BandwidthStart>(requests[1]);
    CHECK(start.request_type == ad::request_type::bw_start_connect_time);
    std::size_t bytes = 0;
    for (std::size_t i = 2; i + 1 < requests.size(); ++i) {
        const auto& payload = std::get<ad::BandwidthPayload>(requests[i]);
        CHECK(payload.sequence == start.sequence);
        CHECK(payload.payload.size() <= ad::max_payload_size);
        bytes += payload.payload.size();
    }
    const auto& stop = std::get<ad::BandwidthStop>(requests.back());
    CHECK(stop.request_type == ad::request_type::bw_stop_connect_time);
    CHECK(stop.sequence == start.sequence);
    CHECK_FALSE(stop.payload.empty());
    CHECK(bytes + stop.payload.size() == 40'000);

    CHECK_FALSE(detect.connect_time_complete(t0 + 100ms));
    detect.on_response(ad::RttResponse{rtt.sequence}, t0 + 30ms);
    CHECK_FALSE(detect.connect_time_complete(t0 + 100ms));
    // 40 000 bytes in 32 ms: 10 000 kbit/s.
    detect.on_response(ad::BandwidthResults{start.sequence, ad::response_type::bw_results_connect_time, 32, 40'000},
                       t0 + 90ms);
    CHECK(detect.connect_time_complete(t0 + 90ms));

    const auto& e = detect.estimate();
    CHECK(e.rtt == 30ms);
    CHECK(e.base_rtt == 30ms);
    CHECK(e.bandwidth_kbps == 10'000U);

    const auto result = detect.network_characteristics_result();
    REQUIRE(result.has_value());
    CHECK(result->base_rtt_ms == 30U);
    CHECK(result->bandwidth_kbps == 10'000U);
    CHECK(result->average_rtt_ms == 30);
    CHECK(ad::request_type_of(*result) == ad::request_type::netchar_all);
}

TEST_CASE("Connect-time detection gives up after its timeout and reports nothing it did not measure")
{
    AutoDetect detect;
    static_cast<void>(detect.start_connect_time(t0));
    CHECK_FALSE(detect.connect_time_complete(t0 + 1s));
    CHECK(detect.connect_time_complete(t0 + 2s));
    CHECK_FALSE(detect.network_characteristics_result().has_value());
}

TEST_CASE("[MS-RDPBCGR] 3.3.5.14: a Network Characteristics Sync replaces the connect-time measurement")
{
    AutoDetect detect;
    static_cast<void>(detect.start_connect_time(t0));
    detect.on_response(ad::NetworkCharacteristicsSync{1, 20'000, 15}, t0 + 5ms);
    CHECK(detect.connect_time_complete(t0 + 5ms));
    CHECK(detect.estimate().rtt == 15ms);
    CHECK(detect.estimate().bandwidth_kbps == 20'000U);
}

TEST_CASE("Continuous RTT probes: one a second, smoothed like TCP's SRTT, with jitter and base RTT")
{
    AutoDetect detect;
    const auto first = detect.poll_rtt_request(t0);
    REQUIRE(first.has_value());
    CHECK(first->request_type == ad::request_type::rtt_continuous);
    CHECK_FALSE(detect.poll_rtt_request(t0 + 500ms).has_value());
    CHECK(detect.next_rtt_request() == t0 + 1s);

    detect.on_response(ad::RttResponse{first->sequence}, t0 + 40ms);
    CHECK(detect.estimate().rtt == 40ms);
    CHECK(detect.estimate().jitter == 20ms);

    const auto second = detect.poll_rtt_request(t0 + 1s);
    REQUIRE(second.has_value());
    CHECK(second->sequence != first->sequence);
    detect.on_response(ad::RttResponse{second->sequence}, t0 + 1s + 120ms);
    const auto& e = detect.estimate();
    CHECK(e.rtt == 50ms);     // 7/8 * 40 + 1/8 * 120
    CHECK(e.jitter == 35ms);  // 3/4 * 20 + 1/4 * |40 - 120|
    CHECK(e.last_rtt == 120ms);
    CHECK(e.base_rtt == 40ms);
    CHECK(e.rtt_samples == 2);

    // An answer to nothing, or a repeated one, changes nothing.
    detect.on_response(ad::RttResponse{second->sequence}, t0 + 2s);
    detect.on_response(ad::RttResponse{0x7777}, t0 + 2s);
    CHECK(detect.estimate().rtt_samples == 2);
}

TEST_CASE("An unanswered probe shows how late the client is; a silent client is no longer probed")
{
    AutoDetect answered;
    const auto probe = answered.poll_rtt_request(t0);
    answered.on_response(ad::RttResponse{probe->sequence}, t0 + 20ms);
    REQUIRE(answered.poll_rtt_request(t0 + 1s).has_value());
    static_cast<void>(answered.poll_rtt_request(t0 + 3s));
    CHECK(answered.estimate().unanswered == 2s);

    // A client that never answers: five lost probes, then no more.
    AutoDetect silent;
    auto now = t0;
    for (int i = 0; i < 12; ++i, now += 1s) {
        static_cast<void>(silent.poll_rtt_request(now));
    }
    CHECK_FALSE(silent.next_rtt_request().has_value());
    CHECK_FALSE(silent.poll_rtt_request(now + 10s).has_value());
    CHECK(silent.estimate().unanswered == 0s);  // it never answered, so it is not "late"
}

TEST_CASE("Continuous bandwidth: large bursts only, not too often, results in kbit/s")
{
    AutoDetect detect;
    CHECK_FALSE(detect.bandwidth_measurement_due(t0, 1000));
    REQUIRE(detect.bandwidth_measurement_due(t0, 100'000));
    const auto start = detect.start_bandwidth_measurement(t0);
    CHECK(start.request_type == ad::request_type::bw_start_continuous);
    const auto stop = detect.stop_bandwidth_measurement();
    CHECK(stop.request_type == ad::request_type::bw_stop_continuous);
    CHECK(stop.sequence == start.sequence);
    CHECK(stop.payload.empty());

    // Not again until the interval passed and the results arrived.
    CHECK_FALSE(detect.bandwidth_measurement_due(t0 + 1s, 100'000));
    CHECK_FALSE(detect.bandwidth_measurement_due(t0 + 3s, 100'000));
    // 100 000 bytes in 80 ms: 10 000 kbit/s.
    detect.on_response(ad::BandwidthResults{start.sequence, ad::response_type::bw_results_continuous, 80, 100'000},
                       t0 + 3s);
    CHECK(detect.estimate().bandwidth_kbps == 10'000U);
    CHECK(detect.bandwidth_measurement_due(t0 + 3s, 100'000));

    // Falls fast, rises slowly; a result below the burst size says nothing.
    const auto again = detect.start_bandwidth_measurement(t0 + 3s);
    detect.on_response(ad::BandwidthResults{again.sequence, ad::response_type::bw_results_continuous, 400, 100'000},
                       t0 + 4s);
    CHECK(detect.estimate().last_bandwidth_kbps == 2'000U);
    CHECK(detect.estimate().bandwidth_kbps == 6'000U);
    const auto third = detect.start_bandwidth_measurement(t0 + 6s);
    detect.on_response(ad::BandwidthResults{third.sequence, ad::response_type::bw_results_continuous, 1, 100}, t0 + 6s);
    CHECK(detect.estimate().bandwidth_samples == 2);
    const auto fourth = detect.start_bandwidth_measurement(t0 + 9s);
    detect.on_response(ad::BandwidthResults{fourth.sequence, ad::response_type::bw_results_continuous, 40, 100'000},
                       t0 + 9s);
    CHECK(detect.estimate().bandwidth_kbps == 9'500U);  // 3/4 * 6000 + 1/4 * 20 000

    // Results for another measurement are ignored.
    detect.on_response(ad::BandwidthResults{0x5555, ad::response_type::bw_results_continuous, 1, 100'000}, t0 + 10s);
    CHECK(detect.estimate().bandwidth_samples == 3);
}
