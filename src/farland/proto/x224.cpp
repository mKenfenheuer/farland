// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/proto/x224.hpp>

#include <algorithm>
#include <string_view>

namespace farland::proto {

namespace {

constexpr std::uint8_t tpkt_version = 3;
constexpr std::uint8_t x224_class_0 = 0;
constexpr std::uint8_t data_eot = 0x80;
constexpr std::uint8_t fixed_cr_cc_header = 6;  // code, DST-REF, SRC-REF, class
constexpr std::uint16_t server_src_ref = 0x1234;

constexpr std::uint8_t type_rdp_neg_req = 0x01;
constexpr std::uint8_t type_rdp_neg_rsp = 0x02;
constexpr std::uint8_t type_rdp_neg_failure = 0x03;
constexpr std::uint8_t type_rdp_correlation_info = 0x06;
constexpr std::uint16_t neg_structure_length = 8;
constexpr std::uint16_t correlation_info_length = 36;

constexpr std::string_view cookie_prefix = "Cookie: mstshash=";

std::span<const std::byte> as_bytes(std::string_view text)
{
    return std::as_bytes(std::span(text));
}

/// Position of the CRLF that ends a token, or npos.
std::size_t find_crlf(std::span<const std::byte> data)
{
    for (std::size_t i = 0; i + 1 < data.size(); ++i) {
        if (data[i] == std::byte{'\r'} && data[i + 1] == std::byte{'\n'}) {
            return i;
        }
    }
    return std::string_view::npos;
}

bool starts_with(std::span<const std::byte> data, std::string_view prefix)
{
    return data.size() >= prefix.size() && std::ranges::equal(data.first(prefix.size()), as_bytes(prefix));
}

/// Reads LI and the fixed part of a CR or CC TPDU, returning a reader over the
/// variable part. [X.224] 13.3 / 13.4; LI counts every octet after itself.
Result<Reader> read_cr_cc_header(Reader& tpdu, TpduCode expected)
{
    const std::size_t start = tpdu.offset();
    FARLAND_TRY(const std::uint8_t li, tpdu.u8());
    if (li < fixed_cr_cc_header || li != tpdu.remaining()) {
        return fail(Errc::invalid_length, "X.224 length indicator does not match the TPDU", start);
    }
    FARLAND_TRY(const std::uint8_t code, tpdu.u8());
    if (code != static_cast<std::uint8_t>(expected)) {
        return fail(Errc::invalid_value, "unexpected X.224 TPDU code", start + 1);
    }
    FARLAND_TRY_VOID(tpdu.skip(4));  // DST-REF, SRC-REF: not meaningful for RDP
    FARLAND_TRY(const std::uint8_t cls, tpdu.u8());
    if (cls != x224_class_0) {
        return fail(Errc::unsupported, "X.224 class other than 0", start + 6);
    }
    return tpdu.sub(tpdu.remaining());
}

void write_cr_cc_header(Writer& w, TpduCode code, std::size_t variable_length, std::uint16_t src_ref)
{
    FARLAND_ASSERT(fixed_cr_cc_header + variable_length <= 0xFE);
    w.u8(static_cast<std::uint8_t>(fixed_cr_cc_header + variable_length));
    w.u8(static_cast<std::uint8_t>(code));
    w.u16be(0);  // DST-REF
    w.u16be(src_ref);
    w.u8(x224_class_0);
}

}  // namespace

std::size_t begin_tpkt(Writer& w)
{
    const std::size_t start = w.size();
    w.u8(tpkt_version);
    w.u8(0);
    w.u16be(0);  // length, patched by end_tpkt
    return start;
}

void end_tpkt(Writer& w, std::size_t start)
{
    const std::size_t length = w.size() - start;
    FARLAND_ASSERT(length >= tpkt_header_size && length <= max_tpkt_size);
    w.patch_u16be(start + 2, static_cast<std::uint16_t>(length));
}

Result<Reader> read_tpkt(Reader& r)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint8_t version, r.u8());
    if (version != tpkt_version) {
        return fail(Errc::invalid_value, "TPKT version is not 3", start);
    }
    FARLAND_TRY_VOID(r.skip(1));  // reserved
    FARLAND_TRY(const std::uint16_t length, r.u16be());
    if (length < tpkt_header_size + 3) {
        return fail(Errc::invalid_length, "TPKT shorter than the smallest X.224 TPDU", start);
    }
    return r.sub(length - tpkt_header_size);
}

Result<TpduCode> peek_tpdu_code(const Reader& tpdu)
{
    Reader copy = tpdu;
    FARLAND_TRY_VOID(copy.skip(1));  // LI
    FARLAND_TRY(const std::uint8_t code, copy.u8());
    switch (code & 0xF0U) {
    case static_cast<std::uint8_t>(TpduCode::connection_request):
    case static_cast<std::uint8_t>(TpduCode::connection_confirm):
    case static_cast<std::uint8_t>(TpduCode::disconnect_request):
    case static_cast<std::uint8_t>(TpduCode::data):
        return static_cast<TpduCode>(code & 0xF0U);
    default:
        return fail(Errc::unsupported, "unknown X.224 TPDU code", tpdu.offset() + 1);
    }
}

Result<ConnectionRequest> decode_connection_request(Reader& tpdu)
{
    FARLAND_TRY(Reader variable, read_cr_cc_header(tpdu, TpduCode::connection_request));
    ConnectionRequest request;

    // Optional cookie or routing token, terminated by CRLF. [MS-RDPBCGR] 2.2.1.1:
    // the two are mutually exclusive and both precede RDP_NEG_REQ.
    if (!variable.empty() && variable.peek_u8().value() != type_rdp_neg_req) {
        const auto rest = variable.rest();
        const std::size_t end = find_crlf(rest);
        if (end == std::string_view::npos) {
            return fail(Errc::invalid_value, "X.224 cookie is not terminated by CRLF", variable.offset());
        }
        const auto token = rest.first(end);
        if (starts_with(token, cookie_prefix)) {
            const auto value = token.subspan(cookie_prefix.size());
            request.cookie.assign(value.size(), '\0');
            std::ranges::transform(value, request.cookie.begin(),
                                   [](std::byte b) { return static_cast<char>(std::to_integer<unsigned char>(b)); });
        } else {
            request.routing_token.assign(token.begin(), token.end());
        }
        FARLAND_TRY_VOID(variable.skip(end + 2));
    }

    if (!variable.empty()) {
        const std::size_t start = variable.offset();
        FARLAND_TRY(const std::uint8_t type, variable.u8());
        if (type != type_rdp_neg_req) {
            return fail(Errc::invalid_value, "expected RDP_NEG_REQ", start);
        }
        FARLAND_TRY(const std::uint8_t flags, variable.u8());
        FARLAND_TRY(const std::uint16_t length, variable.u16le());
        if (length != neg_structure_length) {
            return fail(Errc::invalid_length, "RDP_NEG_REQ length is not 8", start + 2);
        }
        FARLAND_TRY(const std::uint32_t requested, variable.u32le());
        request.negotiation = ConnectionRequest::Negotiation{flags, requested};

        // RDP_NEG_CORRELATION_INFO, [MS-RDPBCGR] 2.2.1.1.2.
        if ((flags & neg_req_flags::correlation_info_present) != 0) {
            const std::size_t info_start = variable.offset();
            FARLAND_TRY(const std::uint8_t info_type, variable.u8());
            FARLAND_TRY_VOID(variable.skip(1));  // flags
            FARLAND_TRY(const std::uint16_t info_length, variable.u16le());
            if (info_type != type_rdp_correlation_info || info_length != correlation_info_length) {
                return fail(Errc::invalid_value, "malformed RDP_NEG_CORRELATION_INFO", info_start);
            }
            FARLAND_TRY(const auto id, variable.bytes(16));
            std::array<std::byte, 16> correlation{};
            std::ranges::copy(id, correlation.begin());
            request.correlation_id = correlation;
            FARLAND_TRY_VOID(variable.skip(16));  // reserved
        }
    }
    FARLAND_TRY_VOID(variable.expect_end("X.224 Connection Request"));
    return request;
}

void encode_connection_request(Writer& w, const ConnectionRequest& request)
{
    Writer variable;
    if (!request.cookie.empty()) {
        variable.bytes(as_bytes(cookie_prefix));
        variable.bytes(as_bytes(request.cookie));
        variable.bytes(as_bytes("\r\n"));
    } else if (!request.routing_token.empty()) {
        variable.bytes(request.routing_token);
        variable.bytes(as_bytes("\r\n"));
    }
    if (request.negotiation) {
        const bool correlation = request.correlation_id.has_value();
        variable.u8(type_rdp_neg_req);
        variable.u8(static_cast<std::uint8_t>(request.negotiation->flags |
                                              (correlation ? neg_req_flags::correlation_info_present : 0U)));
        variable.u16le(neg_structure_length);
        variable.u32le(request.negotiation->requested_protocols);
        if (correlation) {
            variable.u8(type_rdp_correlation_info);
            variable.u8(0);
            variable.u16le(correlation_info_length);
            variable.bytes(*request.correlation_id);
            variable.zeros(16);
        }
    }
    const std::size_t start = begin_tpkt(w);
    write_cr_cc_header(w, TpduCode::connection_request, variable.size(), 0);
    w.bytes(variable.view());
    end_tpkt(w, start);
}

Result<ConnectionConfirm> decode_connection_confirm(Reader& tpdu)
{
    FARLAND_TRY(Reader variable, read_cr_cc_header(tpdu, TpduCode::connection_confirm));
    ConnectionConfirm confirm;
    if (variable.empty()) {
        return confirm;
    }
    const std::size_t start = variable.offset();
    FARLAND_TRY(const std::uint8_t type, variable.u8());
    FARLAND_TRY(const std::uint8_t flags, variable.u8());
    FARLAND_TRY(const std::uint16_t length, variable.u16le());
    if (length != neg_structure_length) {
        return fail(Errc::invalid_length, "RDP_NEG_RSP length is not 8", start + 2);
    }
    FARLAND_TRY(const std::uint32_t value, variable.u32le());
    if (type == type_rdp_neg_rsp) {
        confirm.result = NegotiationResponse{flags, value};
    } else if (type == type_rdp_neg_failure) {
        confirm.result = static_cast<NegotiationFailureCode>(value);
    } else {
        return fail(Errc::invalid_value, "expected RDP_NEG_RSP or RDP_NEG_FAILURE", start);
    }
    FARLAND_TRY_VOID(variable.expect_end("X.224 Connection Confirm"));
    return confirm;
}

void encode_connection_confirm(Writer& w, const ConnectionConfirm& confirm)
{
    const std::size_t start = begin_tpkt(w);
    const bool has_negotiation = !std::holds_alternative<std::monostate>(confirm.result);
    write_cr_cc_header(w, TpduCode::connection_confirm, has_negotiation ? neg_structure_length : 0, server_src_ref);
    if (const auto* response = std::get_if<NegotiationResponse>(&confirm.result)) {
        w.u8(type_rdp_neg_rsp);
        w.u8(response->flags);
        w.u16le(neg_structure_length);
        w.u32le(response->selected_protocol);
    } else if (const auto* failure = std::get_if<NegotiationFailureCode>(&confirm.result)) {
        w.u8(type_rdp_neg_failure);
        w.u8(0);
        w.u16le(neg_structure_length);
        w.u32le(static_cast<std::uint32_t>(*failure));
    }
    end_tpkt(w, start);
}

Result<Reader> decode_data_tpdu(Reader& tpdu)
{
    const std::size_t start = tpdu.offset();
    FARLAND_TRY(const std::uint8_t li, tpdu.u8());
    FARLAND_TRY(const std::uint8_t code, tpdu.u8());
    FARLAND_TRY(const std::uint8_t eot, tpdu.u8());
    if (li != 2 || code != static_cast<std::uint8_t>(TpduCode::data) || eot != data_eot) {
        return fail(Errc::invalid_value, "malformed X.224 Data TPDU header", start);
    }
    return tpdu.sub(tpdu.remaining());
}

std::size_t begin_data_tpdu(Writer& w)
{
    const std::size_t start = begin_tpkt(w);
    w.u8(2);
    w.u8(static_cast<std::uint8_t>(TpduCode::data));
    w.u8(data_eot);
    return start;
}

}  // namespace farland::proto
