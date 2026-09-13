// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/spnego.hpp>
#include <farland/base/ber.hpp>

#include <algorithm>
#include <string_view>
#include <utility>

namespace farland::auth::spnego {

namespace {

constexpr auto der = ber::Rules::der;

/// InitialContextToken ::= [APPLICATION 0] IMPLICIT SEQUENCE (RFC 2743 3.1).
constexpr ber::Tag initial_context_token = ber::application(0);

bool next_is(const Reader& r, ber::Tag tag)
{
    if (r.empty()) {
        return false;
    }
    const auto next = ber::peek_tag(r, der);
    return next.has_value() && *next == tag;
}

/// X.690 8.19.2: every subidentifier is minimal and the last one is complete.
Result<void> check_oid(std::span<const std::byte> oid, std::size_t offset)
{
    if (oid.empty()) {
        return fail(Errc::invalid_length, "OBJECT IDENTIFIER has no content octets", offset);
    }
    if (oid.size() > max_oid_size) {
        return fail(Errc::limit_exceeded, "OBJECT IDENTIFIER too long", offset);
    }
    bool at_start = true;
    for (const std::byte octet : oid) {
        const auto value = std::to_integer<unsigned>(octet);
        if (at_start && value == 0x80U) {
            return fail(Errc::invalid_value, "OBJECT IDENTIFIER subidentifier has a leading 0x80 octet", offset);
        }
        at_start = (value & 0x80U) == 0;
    }
    if (!at_start) {
        return fail(Errc::invalid_value, "OBJECT IDENTIFIER ends inside a subidentifier", offset);
    }
    return {};
}

Result<Oid> read_oid(Reader& r)
{
    FARLAND_TRY(const ber::Tlv tlv, ber::expect_tlv(r, ber::tags::object_identifier, der));
    FARLAND_TRY_VOID(check_oid(tlv.value, tlv.value_offset));
    return Oid(tlv.value.begin(), tlv.value.end());
}

/// The contents of an explicitly tagged field `[number]`.
Result<Reader> read_field(Reader& r, std::uint32_t number)
{
    return ber::read_constructed(r, ber::context(number), der);
}

/// `[number] OCTET STRING`, at most `limit` bytes.
Result<Bytes> read_octets_field(Reader& r, std::uint32_t number, std::size_t limit)
{
    FARLAND_TRY(Reader field, read_field(r, number));
    FARLAND_TRY(const ber::Tlv tlv, ber::expect_tlv(field, ber::tags::octet_string, der));
    FARLAND_TRY_VOID(field.expect_end("explicitly tagged OCTET STRING"));
    if (tlv.value.size() > limit) {
        return fail(Errc::limit_exceeded, "SPNEGO token field too large", tlv.value_offset);
    }
    return Bytes(tlv.value.begin(), tlv.value.end());
}

void write_octets_field(Writer& w, std::uint32_t number, std::span<const std::byte> value)
{
    ber::write_constructed(w, ber::context(number), [&](Writer& field) { ber::write_octet_string(field, value); });
}

// RFC 4178 4.2.1 NegTokenInit, and [MS-SPNG] 2.2.1 NegTokenInit2:
//   mechTypes [0] MechTypeList, reqFlags [1] ContextFlags OPTIONAL,
//   mechToken [2] OCTET STRING OPTIONAL,
//   mechListMIC [3] OCTET STRING OPTIONAL           (RFC 4178)
//   negHints [3] NegHints OPTIONAL, mechListMIC [4] (NegTokenInit2)
Result<NegTokenInit> decode_init(Reader& choice)
{
    FARLAND_TRY(Reader seq, ber::read_constructed(choice, ber::tags::sequence, der));
    FARLAND_TRY_VOID(choice.expect_end("NegTokenInit"));
    NegTokenInit out;

    FARLAND_TRY(const ber::Tlv types_field, ber::expect_tlv(seq, ber::context(0), der));
    Reader types_reader = types_field.reader();
    FARLAND_TRY(const ber::Tlv list, ber::expect_tlv(types_reader, ber::tags::sequence, der));
    FARLAND_TRY_VOID(types_reader.expect_end("mechTypes"));
    out.mech_types_der.assign(types_field.value.begin(), types_field.value.end());
    Reader oids = list.reader();
    while (!oids.empty()) {
        if (out.mech_types.size() == max_mech_types) {
            return fail(Errc::limit_exceeded, "too many entries in mechTypes", oids.offset());
        }
        FARLAND_TRY(Oid oid, read_oid(oids));
        out.mech_types.push_back(std::move(oid));
    }
    if (out.mech_types.empty()) {
        return fail(Errc::invalid_length, "mechTypes is empty", list.value_offset);
    }

    if (next_is(seq, ber::context(1))) {
        // The acceptor ignores reqFlags (RFC 4178 4.2.1).
        FARLAND_TRY(Reader flags, read_field(seq, 1));
        FARLAND_TRY_VOID(ber::expect_tlv(flags, ber::tags::bit_string, der));
        FARLAND_TRY_VOID(flags.expect_end("reqFlags"));
    }
    if (next_is(seq, ber::context(2))) {
        FARLAND_TRY(out.mech_token, read_octets_field(seq, 2, max_token_size));
    }
    if (next_is(seq, ber::context(3))) {
        FARLAND_TRY(Reader field, read_field(seq, 3));
        FARLAND_TRY(const ber::Tlv inner, ber::read_tlv(field, der));
        FARLAND_TRY_VOID(field.expect_end("NegTokenInit field [3]"));
        if (inner.tag == ber::tags::octet_string) {
            if (inner.value.size() > max_mic_size) {
                return fail(Errc::limit_exceeded, "mechListMIC too large", inner.value_offset);
            }
            out.mech_list_mic = Bytes(inner.value.begin(), inner.value.end());
        } else if (inner.tag != ber::tags::sequence) {
            return fail(Errc::invalid_value, "NegTokenInit field [3] is neither mechListMIC nor negHints",
                        inner.value_offset);
        }
    }
    if (!out.mech_list_mic && next_is(seq, ber::context(4))) {
        FARLAND_TRY(out.mech_list_mic, read_octets_field(seq, 4, max_mic_size));
    }
    FARLAND_TRY_VOID(seq.expect_end("NegTokenInit"));
    return out;
}

// RFC 4178 4.2.2 NegTokenResp:
//   negState [0] ENUMERATED OPTIONAL, supportedMech [1] MechType OPTIONAL,
//   responseToken [2] OCTET STRING OPTIONAL, mechListMIC [3] OCTET STRING OPTIONAL
Result<NegTokenResp> decode_resp(Reader& choice)
{
    FARLAND_TRY(Reader seq, ber::read_constructed(choice, ber::tags::sequence, der));
    FARLAND_TRY_VOID(choice.expect_end("NegTokenResp"));
    NegTokenResp out;

    if (next_is(seq, ber::context(0))) {
        FARLAND_TRY(Reader field, read_field(seq, 0));
        const std::size_t offset = field.offset();
        FARLAND_TRY(const std::int64_t state, ber::read_integer(field, der, ber::tags::enumerated));
        FARLAND_TRY_VOID(field.expect_end("negState"));
        if (state < 0 || state > static_cast<std::int64_t>(NegState::request_mic)) {
            return fail(Errc::invalid_value, "unknown negState", offset);
        }
        out.neg_state = static_cast<NegState>(state);
    }
    if (next_is(seq, ber::context(1))) {
        FARLAND_TRY(Reader field, read_field(seq, 1));
        FARLAND_TRY(out.supported_mech, read_oid(field));
        FARLAND_TRY_VOID(field.expect_end("supportedMech"));
    }
    if (next_is(seq, ber::context(2))) {
        FARLAND_TRY(out.response_token, read_octets_field(seq, 2, max_token_size));
    }
    if (next_is(seq, ber::context(3))) {
        FARLAND_TRY(out.mech_list_mic, read_octets_field(seq, 3, max_mic_size));
    }
    FARLAND_TRY_VOID(seq.expect_end("NegTokenResp"));
    return out;
}

}  // namespace

Bytes encode_mech_types(std::span<const Oid> mech_types)
{
    Writer w;
    ber::write_constructed(w, ber::tags::sequence, [&](Writer& list) {
        for (const Oid& oid : mech_types) {
            ber::write_tlv(list, ber::tags::object_identifier, oid);
        }
    });
    return std::move(w).take();
}

NegTokenInit make_neg_token_init(std::vector<Oid> mech_types, std::optional<Bytes> mech_token)
{
    NegTokenInit token;
    token.mech_types_der = encode_mech_types(mech_types);
    token.mech_types = std::move(mech_types);
    token.mech_token = std::move(mech_token);
    return token;
}

Bytes encode(const NegTokenInit& token)
{
    Writer w;
    ber::write_constructed(w, initial_context_token, [&](Writer& app) {
        ber::write_tlv(app, ber::tags::object_identifier, spnego_oid);
        ber::write_constructed(app, ber::context(0), [&](Writer& choice) {
            ber::write_constructed(choice, ber::tags::sequence, [&](Writer& seq) {
                // [0] is explicit, so its contents are the MechTypeList TLV as is.
                ber::write_tlv(seq, ber::context(0), token.mech_types_der);
                if (token.mech_token) {
                    write_octets_field(seq, 2, *token.mech_token);
                }
                if (token.mech_list_mic) {
                    write_octets_field(seq, 3, *token.mech_list_mic);
                }
            });
        });
    });
    return std::move(w).take();
}

Bytes encode(const NegTokenResp& token)
{
    Writer w;
    ber::write_constructed(w, ber::context(1), [&](Writer& choice) {
        ber::write_constructed(choice, ber::tags::sequence, [&](Writer& seq) {
            if (token.neg_state) {
                ber::write_constructed(seq, ber::context(0), [&](Writer& field) {
                    ber::write_integer(field, static_cast<std::int64_t>(*token.neg_state), ber::tags::enumerated);
                });
            }
            if (token.supported_mech) {
                ber::write_constructed(seq, ber::context(1), [&](Writer& field) {
                    ber::write_tlv(field, ber::tags::object_identifier, *token.supported_mech);
                });
            }
            if (token.response_token) {
                write_octets_field(seq, 2, *token.response_token);
            }
            if (token.mech_list_mic) {
                write_octets_field(seq, 3, *token.mech_list_mic);
            }
        });
    });
    return std::move(w).take();
}

Result<NegotiationToken> decode(std::span<const std::byte> token)
{
    Reader r(token);
    FARLAND_TRY(const ber::Tag tag, ber::peek_tag(r, der));
    if (tag == initial_context_token) {
        FARLAND_TRY(Reader inner, ber::read_constructed(r, initial_context_token, der));
        FARLAND_TRY_VOID(r.expect_end("InitialContextToken"));
        const std::size_t oid_offset = inner.offset();
        FARLAND_TRY(const Oid mech, read_oid(inner));
        if (!std::ranges::equal(mech, spnego_oid)) {
            return fail(Errc::unsupported, "InitialContextToken is not for SPNEGO", oid_offset);
        }
        FARLAND_TRY(Reader choice, read_field(inner, 0));
        FARLAND_TRY_VOID(inner.expect_end("InitialContextToken"));
        FARLAND_TRY(NegTokenInit init, decode_init(choice));
        return NegotiationToken{std::move(init)};
    }
    FARLAND_TRY(Reader choice, read_field(r, 1));
    FARLAND_TRY_VOID(r.expect_end("NegotiationToken"));
    FARLAND_TRY(NegTokenResp resp, decode_resp(choice));
    return NegotiationToken{std::move(resp)};
}

bool is_raw_ntlm(std::span<const std::byte> token) noexcept
{
    constexpr std::string_view signature{"NTLMSSP\0", 8};
    return token.size() >= signature.size() &&
           std::ranges::equal(token.first(signature.size()), signature,
                              [](std::byte b, char c) { return std::to_integer<char>(b) == c; });
}

}  // namespace farland::auth::spnego
