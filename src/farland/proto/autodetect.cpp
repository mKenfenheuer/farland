// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/proto/autodetect.hpp>

namespace farland::proto::autodetect {

namespace {

// headerLength per message: the bytes up to the payload (or to the end).
constexpr std::uint8_t short_header = 0x06;
constexpr std::uint8_t payload_header = 0x08;
constexpr std::uint8_t result_header = 0x0E;
constexpr std::uint8_t netchar_all_header = 0x12;

struct Header {
    std::uint8_t length = 0;
    std::uint8_t type_id = 0;
    std::uint16_t sequence = 0;
    std::uint16_t type = 0;  ///< requestType or responseType
};

Result<Header> read_header(Reader& r, std::uint8_t expected_type_id)
{
    const std::size_t start = r.offset();
    Header h;
    FARLAND_TRY(h.length, r.u8());
    FARLAND_TRY(h.type_id, r.u8());
    FARLAND_TRY(h.sequence, r.u16le());
    FARLAND_TRY(h.type, r.u16le());
    if (h.type_id != expected_type_id) {
        return fail(Errc::invalid_value, "auto-detect headerTypeId does not match the direction", start);
    }
    return h;
}

Result<void> expect_length(const Header& h, std::uint8_t expected, std::size_t offset)
{
    if (h.length != expected) {
        return fail(Errc::invalid_length, "auto-detect headerLength does not match the message type", offset);
    }
    return {};
}

void write_header(Writer& w, std::uint8_t length, std::uint8_t type_id, std::uint16_t sequence, std::uint16_t type)
{
    w.u8(length);
    w.u8(type_id);
    w.u16le(sequence);
    w.u16le(type);
}

/// payloadLength and payload of RDP_BW_PAYLOAD and the connect-time RDP_BW_STOP.
Result<std::span<const std::byte>> read_payload(Reader& r)
{
    FARLAND_TRY(const std::uint16_t length, r.u16le());
    return r.bytes(length);
}

void write_payload(Writer& w, std::span<const std::byte> payload)
{
    FARLAND_ASSERT(payload.size() <= 0xFFFF);
    w.u16le(static_cast<std::uint16_t>(payload.size()));
    w.bytes(payload);
}

}  // namespace

std::uint16_t request_type_of(const NetworkCharacteristicsResult& result) noexcept
{
    if (result.base_rtt_ms && result.bandwidth_kbps) {
        return request_type::netchar_all;
    }
    return result.bandwidth_kbps ? request_type::netchar_bandwidth_average_rtt
                                 : request_type::netchar_base_rtt_average_rtt;
}

Result<Request> decode_request(Reader& r)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const Header h, read_header(r, type_id_request));
    switch (h.type) {
    case request_type::rtt_continuous:
    case request_type::rtt_connect_time:
        FARLAND_TRY_VOID(expect_length(h, short_header, start));
        return RttRequest{h.sequence, h.type};
    case request_type::bw_start_continuous:
    case request_type::bw_start_lossy_udp:
    case request_type::bw_start_connect_time:
        FARLAND_TRY_VOID(expect_length(h, short_header, start));
        return BandwidthStart{h.sequence, h.type};
    case request_type::bw_payload: {
        FARLAND_TRY_VOID(expect_length(h, payload_header, start));
        FARLAND_TRY(const auto payload, read_payload(r));
        return BandwidthPayload{h.sequence, payload};
    }
    case request_type::bw_stop_connect_time: {
        FARLAND_TRY_VOID(expect_length(h, payload_header, start));
        const std::size_t length_offset = r.offset();
        FARLAND_TRY(const auto payload, read_payload(r));
        if (payload.empty()) {
            return fail(Errc::invalid_length, "connect-time Bandwidth Measure Stop without payload", length_offset);
        }
        return BandwidthStop{h.sequence, h.type, payload};
    }
    case request_type::bw_stop_continuous:
    case request_type::bw_stop_lossy_udp:
        FARLAND_TRY_VOID(expect_length(h, short_header, start));
        return BandwidthStop{h.sequence, h.type, {}};
    case request_type::netchar_base_rtt_average_rtt:
    case request_type::netchar_bandwidth_average_rtt:
    case request_type::netchar_all: {
        FARLAND_TRY_VOID(
            expect_length(h, h.type == request_type::netchar_all ? netchar_all_header : result_header, start));
        NetworkCharacteristicsResult result;
        result.sequence = h.sequence;
        if (h.type != request_type::netchar_bandwidth_average_rtt) {
            FARLAND_TRY(result.base_rtt_ms, r.u32le());
        }
        if (h.type != request_type::netchar_base_rtt_average_rtt) {
            FARLAND_TRY(result.bandwidth_kbps, r.u32le());
        }
        FARLAND_TRY(result.average_rtt_ms, r.u32le());
        return result;
    }
    default:
        return fail(Errc::unsupported, "unknown auto-detect requestType", start + 4);
    }
}

void encode(Writer& w, const Request& request)
{
    std::visit(
        [&w](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, RttRequest> || std::is_same_v<T, BandwidthStart>) {
                write_header(w, short_header, type_id_request, m.sequence, m.request_type);
            } else if constexpr (std::is_same_v<T, BandwidthPayload>) {
                write_header(w, payload_header, type_id_request, m.sequence, request_type::bw_payload);
                write_payload(w, m.payload);
            } else if constexpr (std::is_same_v<T, BandwidthStop>) {
                if (m.request_type == request_type::bw_stop_connect_time) {
                    FARLAND_ASSERT(!m.payload.empty());
                    write_header(w, payload_header, type_id_request, m.sequence, m.request_type);
                    write_payload(w, m.payload);
                } else {
                    FARLAND_ASSERT(m.payload.empty());
                    write_header(w, short_header, type_id_request, m.sequence, m.request_type);
                }
            } else {
                static_assert(std::is_same_v<T, NetworkCharacteristicsResult>);
                FARLAND_ASSERT(m.base_rtt_ms || m.bandwidth_kbps);
                const std::uint16_t type = request_type_of(m);
                write_header(w, type == request_type::netchar_all ? netchar_all_header : result_header, type_id_request,
                             m.sequence, type);
                if (m.base_rtt_ms) {
                    w.u32le(*m.base_rtt_ms);
                }
                if (m.bandwidth_kbps) {
                    w.u32le(*m.bandwidth_kbps);
                }
                w.u32le(m.average_rtt_ms);
            }
        },
        request);
}

Result<Response> decode_response(Reader& r)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const Header h, read_header(r, type_id_response));
    switch (h.type) {
    case response_type::rtt:
        FARLAND_TRY_VOID(expect_length(h, short_header, start));
        return RttResponse{h.sequence};
    case response_type::bw_results_connect_time:
    case response_type::bw_results_continuous: {
        FARLAND_TRY_VOID(expect_length(h, result_header, start));
        BandwidthResults results;
        results.sequence = h.sequence;
        results.response_type = h.type;
        FARLAND_TRY(results.time_delta_ms, r.u32le());
        FARLAND_TRY(results.byte_count, r.u32le());
        return results;
    }
    case response_type::netchar_sync: {
        FARLAND_TRY_VOID(expect_length(h, result_header, start));
        NetworkCharacteristicsSync sync;
        sync.sequence = h.sequence;
        FARLAND_TRY(sync.bandwidth_kbps, r.u32le());
        FARLAND_TRY(sync.rtt_ms, r.u32le());
        return sync;
    }
    default:
        return fail(Errc::unsupported, "unknown auto-detect responseType", start + 4);
    }
}

void encode(Writer& w, const Response& response)
{
    std::visit(
        [&w](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, RttResponse>) {
                write_header(w, short_header, type_id_response, m.sequence, response_type::rtt);
            } else if constexpr (std::is_same_v<T, BandwidthResults>) {
                write_header(w, result_header, type_id_response, m.sequence, m.response_type);
                w.u32le(m.time_delta_ms);
                w.u32le(m.byte_count);
            } else {
                static_assert(std::is_same_v<T, NetworkCharacteristicsSync>);
                write_header(w, result_header, type_id_response, m.sequence, response_type::netchar_sync);
                w.u32le(m.bandwidth_kbps);
                w.u32le(m.rtt_ms);
            }
        },
        response);
}

Result<Heartbeat> decode_heartbeat(Reader& r)
{
    FARLAND_TRY_VOID(r.skip(1));  // reserved
    Heartbeat h;
    FARLAND_TRY(h.period_seconds, r.u8());
    FARLAND_TRY(h.warning_count, r.u8());
    FARLAND_TRY(h.reconnect_count, r.u8());
    return h;
}

void encode(Writer& w, const Heartbeat& heartbeat)
{
    w.u8(0);  // reserved
    w.u8(heartbeat.period_seconds);
    w.u8(heartbeat.warning_count);
    w.u8(heartbeat.reconnect_count);
}

}  // namespace farland::proto::autodetect
