// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/server/autodetect.hpp>

#include <algorithm>
#include <limits>

namespace farland::server {

namespace {

namespace ad = proto::autodetect;
constexpr std::string_view log_component = "server.autodetect";

/// Payload bytes: the spec asks for random data; any pattern that does not
/// compress serves, and a fixed one keeps the server free of an entropy source.
std::vector<std::byte> make_payload(std::size_t size)
{
    std::vector<std::byte> payload(size);
    std::uint32_t state = 0x9E3779B9U;
    for (auto& b : payload) {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        b = static_cast<std::byte>(state & 0xFFU);
    }
    return payload;
}

std::uint32_t to_ms(std::chrono::steady_clock::duration d)
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(ms, 0, std::numeric_limits<std::uint32_t>::max()));
}

}  // namespace

AutoDetect::AutoDetect(Config config) : config_(config)
{
    config_.base_rtt_window = std::max<std::size_t>(config_.base_rtt_window, 1);
    config_.max_unanswered_probes = std::max(config_.max_unanswered_probes, 1U);
}

// Connect-time --------------------------------------------------------------------

std::vector<ad::Request> AutoDetect::start_connect_time(Clock::time_point now)
{
    std::vector<ad::Request> requests;
    connect_time_started_ = now;

    // The RTT probe goes first, so that the burst does not queue in front of it.
    const std::uint16_t rtt = next_sequence();
    probes_.push_back({rtt, now});
    requests.emplace_back(ad::RttRequest{rtt, ad::request_type::rtt_connect_time});

    // [MS-RDPBCGR] 3.2.5.14: the client times Start to Stop and counts the
    // payload bytes, the Stop's included.
    const std::uint16_t bandwidth = next_sequence();
    bandwidth_sequence_ = bandwidth;
    bandwidth_response_type_ = ad::response_type::bw_results_connect_time;
    payload_ = make_payload(std::max<std::size_t>(config_.connect_time_bytes, 1));
    requests.emplace_back(ad::BandwidthStart{bandwidth, ad::request_type::bw_start_connect_time});
    std::span<const std::byte> rest(payload_);
    while (rest.size() > ad::max_payload_size) {
        requests.emplace_back(ad::BandwidthPayload{bandwidth, rest.first(ad::max_payload_size)});
        rest = rest.subspan(ad::max_payload_size);
    }
    requests.emplace_back(ad::BandwidthStop{bandwidth, ad::request_type::bw_stop_connect_time, rest});
    return requests;
}

bool AutoDetect::connect_time_complete(Clock::time_point now) const noexcept
{
    if (!connect_time_started_) {
        return true;
    }
    return (connect_time_rtt_ && connect_time_bandwidth_) ||
           now - *connect_time_started_ >= config_.connect_time_timeout;
}

std::optional<ad::NetworkCharacteristicsResult> AutoDetect::network_characteristics_result()
{
    if (!estimate_.rtt && !estimate_.bandwidth_kbps) {
        return std::nullopt;
    }
    ad::NetworkCharacteristicsResult result;
    result.sequence = next_sequence();
    if (estimate_.base_rtt) {
        result.base_rtt_ms = to_ms(*estimate_.base_rtt);
    }
    result.bandwidth_kbps = estimate_.bandwidth_kbps;
    result.average_rtt_ms = estimate_.rtt ? to_ms(*estimate_.rtt) : 0;
    if (!result.base_rtt_ms && !result.bandwidth_kbps) {
        return std::nullopt;
    }
    return result;
}

// Continuous ----------------------------------------------------------------------

void AutoDetect::expire_probes(Clock::time_point now)
{
    while (!probes_.empty() && now - probes_.front().sent >= config_.rtt_timeout) {
        probes_.pop_front();
        ++probes_lost_;
    }
    // Some clients advertise auto-detect and still never answer an RTT probe
    // after the connection sequence; stop asking them.
    if (probing_ && estimate_.rtt_samples == 0 && probes_lost_ >= config_.max_unanswered_probes) {
        log::info(log_component, "the client does not answer RTT probes; continuous RTT detection stops");
        probing_ = false;
        probes_.clear();
    }
    // Only a client that answers at all can be late; the age of its oldest
    // unanswered probe is a lower bound of the current RTT.
    estimate_.unanswered =
        (!probes_.empty() && estimate_.rtt_samples > 0) ? now - probes_.front().sent : Clock::duration::zero();
}

std::optional<ad::RttRequest> AutoDetect::poll_rtt_request(Clock::time_point now)
{
    expire_probes(now);
    const auto next = next_rtt_request();
    if (!next || now < *next) {
        return std::nullopt;
    }
    last_probe_ = now;
    const std::uint16_t sequence = next_sequence();
    probes_.push_back({sequence, now});
    return ad::RttRequest{sequence, ad::request_type::rtt_continuous};
}

std::optional<AutoDetect::Clock::time_point> AutoDetect::next_rtt_request() const noexcept
{
    if (!probing_) {
        return std::nullopt;
    }
    if (!last_probe_) {
        return Clock::time_point::min();
    }
    return *last_probe_ + config_.rtt_interval;
}

bool AutoDetect::bandwidth_measurement_due(Clock::time_point now, std::size_t burst_bytes) const noexcept
{
    if (burst_bytes < config_.min_burst_bytes) {
        return false;
    }
    if (!last_bandwidth_measurement_) {
        return true;
    }
    // Results still outstanding: wait for them, but not forever.
    const auto since = now - *last_bandwidth_measurement_;
    return since >= config_.bandwidth_interval && (!bandwidth_sequence_ || since >= config_.rtt_timeout);
}

ad::BandwidthStart AutoDetect::start_bandwidth_measurement(Clock::time_point now)
{
    last_bandwidth_measurement_ = now;
    bandwidth_sequence_ = next_sequence();
    bandwidth_response_type_ = ad::response_type::bw_results_continuous;
    return {*bandwidth_sequence_, ad::request_type::bw_start_continuous};
}

ad::BandwidthStop AutoDetect::stop_bandwidth_measurement() const noexcept
{
    return {bandwidth_sequence_.value_or(0), ad::request_type::bw_stop_continuous, {}};
}

// Responses -----------------------------------------------------------------------

void AutoDetect::on_response(const ad::Response& response, Clock::time_point now)
{
    if (const auto* rtt = std::get_if<ad::RttResponse>(&response)) {
        const auto found = std::ranges::find(probes_, rtt->sequence, &Probe::sequence);
        if (found == probes_.end()) {
            log::debug(log_component, "RTT response {} matches no probe", rtt->sequence);
            return;
        }
        const bool connect_time = connect_time_started_ && !connect_time_rtt_;
        add_rtt_sample(now - found->sent);
        // Probes answered out of order are rare; older ones are lost.
        probes_.erase(probes_.begin(), std::next(found));
        connect_time_rtt_ = connect_time_rtt_ || connect_time;
        expire_probes(now);
        return;
    }
    if (const auto* results = std::get_if<ad::BandwidthResults>(&response)) {
        if (!bandwidth_sequence_ || results->sequence != *bandwidth_sequence_ ||
            results->response_type != bandwidth_response_type_) {
            log::debug(log_component, "bandwidth results {} match no measurement", results->sequence);
            return;
        }
        bandwidth_sequence_.reset();
        if (results->response_type == ad::response_type::bw_results_connect_time) {
            connect_time_bandwidth_ = true;
        } else if (results->byte_count < config_.min_burst_bytes / 2) {
            return;  // too little arrived in between to say anything
        }
        // [MS-RDPBCGR] 3.3.5.14: kbit/s = byteCount * 8 / timeDelta. A burst that
        // took under a millisecond counts as one millisecond.
        const std::uint64_t kbps = std::uint64_t{results->byte_count} * 8U / std::max(results->time_delta_ms, 1U);
        add_bandwidth_sample(static_cast<std::uint32_t>(std::min<std::uint64_t>(kbps, 0xFFFFFFFFU)));
        return;
    }
    const auto& sync = std::get<ad::NetworkCharacteristicsSync>(response);
    // [MS-RDPBCGR] 3.3.5.14 step 4: a reconnecting client's earlier results
    // replace the running connect-time measurement.
    if (connect_time_started_ && !(connect_time_rtt_ && connect_time_bandwidth_)) {
        probes_.clear();
        bandwidth_sequence_.reset();
        connect_time_rtt_ = connect_time_bandwidth_ = true;
    }
    add_rtt_sample(std::chrono::milliseconds(sync.rtt_ms));
    add_bandwidth_sample(sync.bandwidth_kbps);
}

void AutoDetect::add_rtt_sample(Clock::duration sample)
{
    sample = std::max(sample, Clock::duration::zero());
    auto& e = estimate_;
    if (!e.rtt) {
        e.rtt = sample;
        e.jitter = sample / 2;
    } else {
        const auto deviation = sample > *e.rtt ? sample - *e.rtt : *e.rtt - sample;
        e.jitter = (e.jitter * 3 + deviation) / 4;
        e.rtt = (*e.rtt * 7 + sample) / 8;
    }
    e.last_rtt = sample;
    recent_rtt_.push_back(sample);
    if (recent_rtt_.size() > config_.base_rtt_window) {
        recent_rtt_.pop_front();
    }
    e.base_rtt = *std::ranges::min_element(recent_rtt_);
    ++e.rtt_samples;
}

void AutoDetect::add_bandwidth_sample(std::uint32_t kbps)
{
    auto& e = estimate_;
    e.last_bandwidth_kbps = kbps;
    if (!e.bandwidth_kbps) {
        e.bandwidth_kbps = kbps;
    } else if (kbps < *e.bandwidth_kbps) {
        e.bandwidth_kbps = static_cast<std::uint32_t>((std::uint64_t{*e.bandwidth_kbps} + kbps) / 2);
    } else {
        e.bandwidth_kbps = static_cast<std::uint32_t>((std::uint64_t{*e.bandwidth_kbps} * 3 + kbps) / 4);
    }
    ++e.bandwidth_samples;
}

}  // namespace farland::server
