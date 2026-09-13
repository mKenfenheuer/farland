// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/spnego.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace spnego = farland::auth::spnego;
using farland::Errc;
using farland::test::hex;

namespace {

spnego::Oid oid(std::span<const std::byte> value)
{
    return {value.begin(), value.end()};
}

/// MechTypeList as Windows orders it: MS Kerberos, Kerberos, NTLM.
const std::vector<std::byte>& windows_mech_types_der()
{
    static const auto der = hex("30 22"
                                "06 09 2a 86 48 82 f7 12 01 02 02"
                                "06 09 2a 86 48 86 f7 12 01 02 02"
                                "06 0a 2b 06 01 04 01 82 37 02 02 0a");
    return der;
}

Errc decode_error(std::string_view hex_text)
{
    const auto result = spnego::decode(hex(hex_text));
    REQUIRE_FALSE(result.has_value());
    return result.error().code;
}

}  // namespace

TEST_CASE("NegTokenInit with Kerberos first and an optimistic token (RFC 4178 4.2.1)")
{
    const auto token = hex("60 3a 06 06 2b 06 01 05 05 02"  // InitialContextToken, SPNEGO OID
                           "a0 30 30 2e"                    // [0] NegTokenInit SEQUENCE
                           "a0 24 30 22"                    // mechTypes
                           "06 09 2a 86 48 82 f7 12 01 02 02"
                           "06 09 2a 86 48 86 f7 12 01 02 02"
                           "06 0a 2b 06 01 04 01 82 37 02 02 0a"
                           "a2 06 04 04 de ad be ef");  // mechToken
    const auto decoded = spnego::decode(token);
    REQUIRE(decoded.has_value());
    const auto* init = std::get_if<spnego::NegTokenInit>(&*decoded);
    REQUIRE(init != nullptr);
    CHECK(init->mech_types ==
          std::vector<spnego::Oid>{oid(spnego::ms_kerberos_oid), oid(spnego::kerberos_oid), oid(spnego::ntlm_oid)});
    CHECK(init->mech_types_der == windows_mech_types_der());
    CHECK(init->mech_token == hex("de ad be ef"));
    CHECK_FALSE(init->mech_list_mic.has_value());

    CHECK(spnego::encode(*init) == token);
    CHECK(spnego::make_neg_token_init(init->mech_types, hex("de ad be ef")) == *init);
}

TEST_CASE("NegTokenInit2 negHints are skipped ([MS-SPNG] 2.2.1)")
{
    const auto decoded = spnego::decode(hex("60 25 06 06 2b 06 01 05 05 02 a0 1b 30 19"
                                            "a0 0e 30 0c 06 0a 2b 06 01 04 01 82 37 02 02 0a"
                                            "a3 07 30 05 a0 03 1b 01 78"));  // negHints { hintName "x" }
    REQUIRE(decoded.has_value());
    const auto& init = std::get<spnego::NegTokenInit>(*decoded);
    CHECK(init.mech_types == std::vector<spnego::Oid>{oid(spnego::ntlm_oid)});
    CHECK_FALSE(init.mech_token.has_value());
    CHECK_FALSE(init.mech_list_mic.has_value());
}

TEST_CASE("NegTokenResp with negState, supportedMech and responseToken (RFC 4178 4.2.2)")
{
    const auto token = hex("a1 1b 30 19"
                           "a0 03 0a 01 01"                             // negState accept-incomplete
                           "a1 0c 06 0a 2b 06 01 04 01 82 37 02 02 0a"  // supportedMech NTLM
                           "a2 04 04 02 01 02");                        // responseToken
    const auto decoded = spnego::decode(token);
    REQUIRE(decoded.has_value());
    const auto& resp = std::get<spnego::NegTokenResp>(*decoded);
    CHECK(resp.neg_state == spnego::NegState::accept_incomplete);
    CHECK(resp.supported_mech == oid(spnego::ntlm_oid));
    CHECK(resp.response_token == hex("01 02"));
    CHECK_FALSE(resp.mech_list_mic.has_value());
    CHECK(spnego::encode(resp) == token);
}

TEST_CASE("NegTokenResp with accept-completed and a mechListMIC (RFC 4178 4.2.2)")
{
    const auto token = hex("a1 0e 30 0c a0 03 0a 01 00 a3 05 04 03 aa bb cc");
    const auto decoded = spnego::decode(token);
    REQUIRE(decoded.has_value());
    const auto& resp = std::get<spnego::NegTokenResp>(*decoded);
    CHECK(resp.neg_state == spnego::NegState::accept_completed);
    CHECK_FALSE(resp.supported_mech.has_value());
    CHECK_FALSE(resp.response_token.has_value());
    CHECK(resp.mech_list_mic == hex("aa bb cc"));
    CHECK(spnego::encode(resp) == token);
}

TEST_CASE("SPNEGO tokens round-trip")
{
    spnego::NegTokenResp resp;
    resp.neg_state = spnego::NegState::request_mic;
    resp.supported_mech = oid(spnego::kerberos_oid);
    resp.response_token = std::vector<std::byte>(300, std::byte{0x5a});
    resp.mech_list_mic = hex("01 00 00 00 11 22 33 44 55 66 77 88 00 00 00 00");
    const auto decoded_resp = spnego::decode(spnego::encode(resp));
    REQUIRE(decoded_resp.has_value());
    CHECK(std::get<spnego::NegTokenResp>(*decoded_resp) == resp);

    auto init = spnego::make_neg_token_init({oid(spnego::ntlm_oid)}, hex("4e 54 4c 4d 53 53 50 00 01"));
    init.mech_list_mic = hex("01 02");
    const auto decoded_init = spnego::decode(spnego::encode(init));
    REQUIRE(decoded_init.has_value());
    CHECK(std::get<spnego::NegTokenInit>(*decoded_init) == init);

    CHECK(spnego::encode(spnego::NegTokenResp{}) == hex("a1 02 30 00"));
}

TEST_CASE("encode_mech_types produces the DER the mechListMIC covers (RFC 4178 5)")
{
    const std::vector mech_types{oid(spnego::ms_kerberos_oid), oid(spnego::kerberos_oid), oid(spnego::ntlm_oid)};
    CHECK(spnego::encode_mech_types(mech_types) == windows_mech_types_der());
}

TEST_CASE("SPNEGO decoding rejects malformed tokens")
{
    SECTION("an InitialContextToken for another mechanism")
    {
        CHECK(decode_error("60 0b 06 09 2a 86 48 86 f7 12 01 02 02") == Errc::unsupported);
    }
    SECTION("an unknown negState")
    {
        CHECK(decode_error("a1 07 30 05 a0 03 0a 01 04") == Errc::invalid_value);
    }
    SECTION("an OBJECT IDENTIFIER that ends inside a subidentifier")
    {
        CHECK(decode_error("a1 08 30 06 a1 04 06 02 2b 86") == Errc::invalid_value);
    }
    SECTION("an empty mechTypes")
    {
        CHECK(decode_error("60 10 06 06 2b 06 01 05 05 02 a0 06 30 04 a0 02 30 00") == Errc::invalid_length);
    }
    SECTION("fields out of order")
    {
        CHECK(decode_error("a1 0b 30 09 a2 02 04 00 a0 03 0a 01 00") == Errc::trailing_data);
    }
    SECTION("trailing bytes after the token")
    {
        CHECK(decode_error("a1 02 30 00 00") == Errc::trailing_data);
    }
    SECTION("a non-minimal DER length")
    {
        CHECK(decode_error("a1 81 02 30 00") == Errc::invalid_length);
    }
    SECTION("a NegTokenInit without the InitialContextToken wrapper")
    {
        CHECK(decode_error("a0 0e 30 0c a0 0a 30 08 06 06 2b 06 01 05 05 02") == Errc::invalid_value);
    }
    SECTION("a truncated token")
    {
        CHECK(decode_error("a1 1b 30 19 a0 03") == Errc::truncated);
    }
}

TEST_CASE("Raw NTLM tokens are recognised by their signature ([MS-NLMP] 2.2.1)")
{
    CHECK(spnego::is_raw_ntlm(hex("4e 54 4c 4d 53 53 50 00 01 00 00 00")));
    CHECK(spnego::is_raw_ntlm(hex("4e 54 4c 4d 53 53 50 00")));
    CHECK_FALSE(spnego::is_raw_ntlm(hex("4e 54 4c 4d 53 53 50")));
    CHECK_FALSE(spnego::is_raw_ntlm(hex("4e 54 4c 4d 53 53 50 01 01")));
    CHECK_FALSE(spnego::is_raw_ntlm(hex("60 3a 06 06 2b 06 01 05 05 02")));
}
