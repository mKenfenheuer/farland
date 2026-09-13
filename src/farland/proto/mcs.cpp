// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/ber.hpp>
#include <farland/base/per.hpp>
#include <farland/proto/mcs.hpp>

#include <algorithm>
#include <limits>

namespace farland::proto::mcs {

namespace {

constexpr auto rules = ber::Rules::ber;
constexpr ber::Tag connect_initial_tag = ber::application(101);
constexpr ber::Tag connect_response_tag = ber::application(102);

/// DomainMCSPDU CHOICE indices, T.125 section 7 (ASN.1 module), in the top six
/// bits of the first octet.
enum class Choice : std::uint8_t {
    erect_domain_request = 1,
    disconnect_provider_ultimatum = 8,
    attach_user_request = 10,
    attach_user_confirm = 11,
    channel_join_request = 14,
    channel_join_confirm = 15,
    send_data_request = 25,
    send_data_indication = 26,
};

constexpr std::uint8_t optional_field_present = 0x02;
/// dataPriority = high (1), segmentation = begin | end; [MS-RDPBCGR] 2.2.1.5 ff.
constexpr std::uint8_t priority_and_segmentation = 0x70;
constexpr std::uint8_t result_alternatives = 16;

std::uint8_t header(Choice choice, std::uint8_t low_bits = 0)
{
    return static_cast<std::uint8_t>((static_cast<unsigned>(choice) << 2U) | low_bits);
}

/// Domain parameters and IDs are unsigned; mstsc omits the leading zero octet
/// for values with the top bit set, so the sign bit is ignored.
Result<std::uint32_t> read_u32_integer(Reader& r)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const ber::Tlv tlv, ber::expect_tlv(r, ber::tags::integer, rules));
    FARLAND_TRY(const std::uint64_t value, ber::decode_raw_unsigned(tlv));
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        return fail(Errc::limit_exceeded, "MCS domain parameter above 2^32", start);
    }
    return static_cast<std::uint32_t>(value);
}

Result<DomainParameters> read_domain_parameters(Reader& r)
{
    FARLAND_TRY(Reader seq, ber::read_constructed(r, ber::tags::sequence, rules));
    DomainParameters p;
    FARLAND_TRY(p.max_channel_ids, read_u32_integer(seq));
    FARLAND_TRY(p.max_user_ids, read_u32_integer(seq));
    FARLAND_TRY(p.max_token_ids, read_u32_integer(seq));
    FARLAND_TRY(p.num_priorities, read_u32_integer(seq));
    FARLAND_TRY(p.min_throughput, read_u32_integer(seq));
    FARLAND_TRY(p.max_height, read_u32_integer(seq));
    FARLAND_TRY(p.max_mcs_pdu_size, read_u32_integer(seq));
    FARLAND_TRY(p.protocol_version, read_u32_integer(seq));
    FARLAND_TRY_VOID(seq.expect_end("MCS DomainParameters"));
    return p;
}

void write_domain_parameters(Writer& w, const DomainParameters& p)
{
    ber::write_constructed(w, ber::tags::sequence, [&p](Writer& seq) {
        for (const std::uint32_t value : {p.max_channel_ids, p.max_user_ids, p.max_token_ids, p.num_priorities,
                                          p.min_throughput, p.max_height, p.max_mcs_pdu_size, p.protocol_version}) {
            ber::write_unsigned(seq, value);
        }
    });
}

Result<ResultCode> read_result(Reader& r)
{
    FARLAND_TRY(const std::uint8_t value, per::read_enumerated(r, result_alternatives));
    return static_cast<ResultCode>(value);
}

}  // namespace

Result<ConnectInitial> decode_connect_initial(Reader& r)
{
    FARLAND_TRY(Reader body, ber::read_constructed(r, connect_initial_tag, rules));
    ConnectInitial pdu;
    FARLAND_TRY(pdu.calling_domain_selector, ber::read_octet_string(body, rules));
    FARLAND_TRY(pdu.called_domain_selector, ber::read_octet_string(body, rules));
    FARLAND_TRY(pdu.upward_flag, ber::read_boolean(body, rules));
    FARLAND_TRY(pdu.target, read_domain_parameters(body));
    FARLAND_TRY(pdu.minimum, read_domain_parameters(body));
    FARLAND_TRY(pdu.maximum, read_domain_parameters(body));
    FARLAND_TRY(pdu.user_data, ber::read_octet_string(body, rules));
    FARLAND_TRY_VOID(body.expect_end("MCS Connect-Initial"));
    return pdu;
}

void encode_connect_initial(Writer& w, const ConnectInitial& pdu)
{
    ber::write_constructed(w, connect_initial_tag, [&pdu](Writer& body) {
        ber::write_octet_string(body, pdu.calling_domain_selector);
        ber::write_octet_string(body, pdu.called_domain_selector);
        ber::write_boolean(body, pdu.upward_flag);
        write_domain_parameters(body, pdu.target);
        write_domain_parameters(body, pdu.minimum);
        write_domain_parameters(body, pdu.maximum);
        ber::write_octet_string(body, pdu.user_data);
    });
}

Result<ConnectResponse> decode_connect_response(Reader& r)
{
    FARLAND_TRY(Reader body, ber::read_constructed(r, connect_response_tag, rules));
    ConnectResponse pdu;
    const std::size_t result_offset = body.offset();
    FARLAND_TRY(const std::uint64_t result, ber::read_unsigned(body, rules, ber::tags::enumerated));
    if (result >= result_alternatives) {
        return fail(Errc::invalid_value, "MCS Connect-Response result out of range", result_offset);
    }
    pdu.result = static_cast<ResultCode>(result);
    FARLAND_TRY(pdu.called_connect_id, read_u32_integer(body));
    FARLAND_TRY(pdu.parameters, read_domain_parameters(body));
    FARLAND_TRY(pdu.user_data, ber::read_octet_string(body, rules));
    FARLAND_TRY_VOID(body.expect_end("MCS Connect-Response"));
    return pdu;
}

void encode_connect_response(Writer& w, const ConnectResponse& pdu)
{
    ber::write_constructed(w, connect_response_tag, [&pdu](Writer& body) {
        ber::write_unsigned(body, static_cast<std::uint64_t>(pdu.result), ber::tags::enumerated);
        ber::write_unsigned(body, pdu.called_connect_id);
        write_domain_parameters(body, pdu.parameters);
        ber::write_octet_string(body, pdu.user_data);
    });
}

DomainParameters negotiate_domain_parameters(const ConnectInitial& pdu)
{
    // The same merge FreeRDP's server performs (mcs_merge_domain_parameters),
    // which mstsc and Windows App accept: RDP needs at least four channel IDs
    // and three user IDs, one priority, a flat domain and PDUs of 1K to 64K.
    constexpr std::uint32_t min_pdu_size = 1024;
    constexpr std::uint32_t max_pdu_size = 65528;
    const auto& target = pdu.target;
    DomainParameters p;
    p.max_channel_ids = std::max<std::uint32_t>(target.max_channel_ids, 4);
    p.max_user_ids = std::max<std::uint32_t>(target.max_user_ids, 3);
    p.max_token_ids = target.max_token_ids;
    p.num_priorities = 1;
    p.min_throughput = target.min_throughput;
    p.max_height = 1;
    const std::uint32_t pdu_size =
        target.max_mcs_pdu_size >= min_pdu_size ? target.max_mcs_pdu_size : pdu.maximum.max_mcs_pdu_size;
    p.max_mcs_pdu_size = std::clamp(pdu_size, min_pdu_size, max_pdu_size);
    p.protocol_version = 2;
    return p;
}

Result<DomainPdu> decode_domain_pdu(Reader& r)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint8_t first, r.u8());
    const auto choice = static_cast<Choice>(first >> 2U);
    const auto low_bits = static_cast<std::uint8_t>(first & 0x03U);

    DomainPdu pdu;
    switch (choice) {
    case Choice::erect_domain_request: {
        ErectDomainRequest p;
        FARLAND_TRY(p.sub_height, per::read_integer(r));
        FARLAND_TRY(p.sub_interval, per::read_integer(r));
        pdu = p;
        break;
    }
    case Choice::disconnect_provider_ultimatum: {
        // Reason is a 3-bit enumeration spanning the two low bits of the
        // header octet and the top bit of the next one.
        FARLAND_TRY(const std::uint8_t second, r.u8());
        const auto reason = static_cast<unsigned>((low_bits << 1U) | (second >> 7U));
        if (reason > static_cast<unsigned>(DisconnectReason::channel_purged)) {
            return fail(Errc::invalid_value, "unknown Disconnect Provider Ultimatum reason", start);
        }
        pdu = DisconnectProviderUltimatum{static_cast<DisconnectReason>(reason)};
        break;
    }
    case Choice::attach_user_request:
        pdu = AttachUserRequest{};
        break;
    case Choice::attach_user_confirm: {
        AttachUserConfirm p;
        FARLAND_TRY(p.result, read_result(r));
        if ((low_bits & optional_field_present) != 0) {
            FARLAND_TRY(p.initiator, per::read_integer16(r, user_id_base));
        }
        pdu = p;
        break;
    }
    case Choice::channel_join_request: {
        ChannelJoinRequest p;
        FARLAND_TRY(p.initiator, per::read_integer16(r, user_id_base));
        FARLAND_TRY(p.channel_id, per::read_integer16(r, 0));
        pdu = p;
        break;
    }
    case Choice::channel_join_confirm: {
        ChannelJoinConfirm p;
        FARLAND_TRY(p.result, read_result(r));
        FARLAND_TRY(p.initiator, per::read_integer16(r, user_id_base));
        FARLAND_TRY(p.requested, per::read_integer16(r, 0));
        if ((low_bits & optional_field_present) != 0) {
            FARLAND_TRY(p.channel_id, per::read_integer16(r, 0));
        }
        pdu = p;
        break;
    }
    case Choice::send_data_request:
    case Choice::send_data_indication: {
        FARLAND_TRY(const std::uint16_t initiator, per::read_integer16(r, user_id_base));
        FARLAND_TRY(const std::uint16_t channel_id, per::read_integer16(r, 0));
        FARLAND_TRY_VOID(r.skip(1));  // dataPriority and segmentation
        FARLAND_TRY(const std::size_t length, per::read_length(r));
        FARLAND_TRY(const auto data, r.bytes(length));
        if (choice == Choice::send_data_request) {
            pdu = SendDataRequest{initiator, channel_id, data};
        } else {
            pdu = SendDataIndication{initiator, channel_id, data};
        }
        break;
    }
    default:
        return fail(Errc::unsupported, "unsupported MCS domain PDU", start);
    }
    FARLAND_TRY_VOID(r.expect_end("MCS domain PDU"));
    return pdu;
}

void encode(Writer& w, const ErectDomainRequest& pdu)
{
    w.u8(header(Choice::erect_domain_request));
    per::write_integer(w, pdu.sub_height);
    per::write_integer(w, pdu.sub_interval);
}

void encode(Writer& w, const DisconnectProviderUltimatum& pdu)
{
    const auto reason = static_cast<unsigned>(pdu.reason);
    w.u8(header(Choice::disconnect_provider_ultimatum, static_cast<std::uint8_t>(reason >> 1U)));
    w.u8(static_cast<std::uint8_t>((reason & 1U) << 7U));
}

void encode(Writer& w, const AttachUserRequest& /*pdu*/)
{
    w.u8(header(Choice::attach_user_request));
}

void encode(Writer& w, const AttachUserConfirm& pdu)
{
    w.u8(header(Choice::attach_user_confirm, pdu.initiator ? optional_field_present : 0));
    per::write_enumerated(w, static_cast<std::uint8_t>(pdu.result));
    if (pdu.initiator) {
        per::write_integer16(w, *pdu.initiator, user_id_base);
    }
}

void encode(Writer& w, const ChannelJoinRequest& pdu)
{
    w.u8(header(Choice::channel_join_request));
    per::write_integer16(w, pdu.initiator, user_id_base);
    per::write_integer16(w, pdu.channel_id, 0);
}

void encode(Writer& w, const ChannelJoinConfirm& pdu)
{
    w.u8(header(Choice::channel_join_confirm, pdu.channel_id ? optional_field_present : 0));
    per::write_enumerated(w, static_cast<std::uint8_t>(pdu.result));
    per::write_integer16(w, pdu.initiator, user_id_base);
    per::write_integer16(w, pdu.requested, 0);
    if (pdu.channel_id) {
        per::write_integer16(w, *pdu.channel_id, 0);
    }
}

namespace {

void encode_send_data(Writer& w, Choice choice, std::uint16_t initiator, std::uint16_t channel_id,
                      std::span<const std::byte> data)
{
    w.u8(header(choice));
    per::write_integer16(w, initiator, user_id_base);
    per::write_integer16(w, channel_id, 0);
    w.u8(priority_and_segmentation);
    per::write_length(w, data.size());
    w.bytes(data);
}

}  // namespace

void encode(Writer& w, const SendDataRequest& pdu)
{
    encode_send_data(w, Choice::send_data_request, pdu.initiator, pdu.channel_id, pdu.data);
}

void encode(Writer& w, const SendDataIndication& pdu)
{
    encode_send_data(w, Choice::send_data_indication, pdu.initiator, pdu.channel_id, pdu.data);
}

}  // namespace farland::proto::mcs
