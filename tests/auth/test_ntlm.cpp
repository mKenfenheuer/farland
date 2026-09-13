// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/legacy_crypto.hpp>
#include <farland/auth/ntlm.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/text.hpp>
#include <farland/base/writer.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ntlm = farland::auth::ntlm;
namespace legacy = farland::auth::legacy;
using farland::Errc;
using farland::SecretString;
using farland::test::ascii;
using farland::test::hex;
using ntlm::ChannelBindingPolicy;

namespace {

template <std::size_t N>
std::array<std::byte, N> array_of(std::string_view text)
{
    const auto bytes = hex(text);
    REQUIRE(bytes.size() == N);
    std::array<std::byte, N> out{};
    std::ranges::copy(bytes, out.begin());
    return out;
}

std::vector<std::byte> bytes_of(std::span<const std::byte> data)
{
    return {data.begin(), data.end()};
}

std::vector<std::byte> utf16(std::string_view text)
{
    return farland::utf8_to_utf16le(text);
}

std::vector<std::byte> concat(std::span<const std::byte> a, std::span<const std::byte> b)
{
    std::vector<std::byte> out(a.begin(), a.end());
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

std::array<std::byte, 16> filled(std::uint8_t value)
{
    std::array<std::byte, 16> out{};
    out.fill(std::byte{value});
    return out;
}

std::uint32_t sequence_of(std::span<const std::byte> signature)
{
    farland::Reader r(signature.subspan(12, 4));
    return r.u32le().value();
}

// [MS-NLMP] 4.2.1 Common Values.
constexpr std::string_view server_challenge_hex = "01 23 45 67 89 ab cd ef";
constexpr std::string_view client_challenge_hex = "aa aa aa aa aa aa aa aa";
constexpr std::string_view random_session_key_hex = "55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55";

// [MS-NLMP] 4.2.4.3 Messages.
constexpr std::string_view spec_challenge = R"(
    4e 54 4c 4d 53 53 50 00 02 00 00 00 0c 00 0c 00
    38 00 00 00 33 82 8a e2 01 23 45 67 89 ab cd ef
    00 00 00 00 00 00 00 00 24 00 24 00 44 00 00 00
    06 00 70 17 00 00 00 0f 53 00 65 00 72 00 76 00
    65 00 72 00 02 00 0c 00 44 00 6f 00 6d 00 61 00
    69 00 6e 00 01 00 0c 00 53 00 65 00 72 00 76 00
    65 00 72 00 00 00 00 00)";
constexpr std::string_view spec_authenticate = R"(
    4e 54 4c 4d 53 53 50 00 03 00 00 00 18 00 18 00
    6c 00 00 00 54 00 54 00 84 00 00 00 0c 00 0c 00
    48 00 00 00 08 00 08 00 54 00 00 00 10 00 10 00
    5c 00 00 00 10 00 10 00 d8 00 00 00 35 82 88 e2
    05 01 28 0a 00 00 00 0f 44 00 6f 00 6d 00 61 00
    69 00 6e 00 55 00 73 00 65 00 72 00 43 00 4f 00
    4d 00 50 00 55 00 54 00 45 00 52 00 86 c3 50 97
    ac 9c ec 10 25 54 76 4a 57 cc cc 19 aa aa aa aa
    aa aa aa aa 68 cd 0a b8 51 e5 1c 96 aa bc 92 7b
    eb ef 6a 1c 01 01 00 00 00 00 00 00 00 00 00 00
    00 00 00 00 aa aa aa aa aa aa aa aa 00 00 00 00
    02 00 0c 00 44 00 6f 00 6d 00 61 00 69 00 6e 00
    01 00 0c 00 53 00 65 00 72 00 76 00 65 00 72 00
    00 00 00 00 00 00 00 00 c5 da d2 54 4f c9 79 90
    94 ce 1c e9 0b c9 d0 3e)";
// [MS-NLMP] 4.2.4.1.3 temp: bytes 0x94..0xd7 of the AUTHENTICATE_MESSAGE.
constexpr std::string_view spec_temp = R"(
    01 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00
    aa aa aa aa aa aa aa aa 00 00 00 00
    02 00 0c 00 44 00 6f 00 6d 00 61 00 69 00 6e 00
    01 00 0c 00 53 00 65 00 72 00 76 00 65 00 72 00
    00 00 00 00 00 00 00 00)";
constexpr std::string_view spec_lmv2_response =
    "86 c3 50 97 ac 9c ec 10 25 54 76 4a 57 cc cc 19 aa aa aa aa aa aa aa aa";

// [MS-NLMP] 4.2.4.4 GSS_WrapEx Examples (client to server, SeqNum 0).
constexpr std::string_view spec_plaintext = "50 00 6c 00 61 00 69 00 6e 00 74 00 65 00 78 00 74 00";
constexpr std::string_view spec_sealed = "54 e5 01 65 bf 19 36 dc 99 60 20 c1 81 1b 0f 06 fb 5f";
constexpr std::string_view spec_signature = "01 00 00 00 7f b3 8e c5 c5 5d 49 76 00 00 00 00";

std::optional<farland::auth::NtHash> lookup(std::string_view user, std::string_view /*domain*/)
{
    if (user == "User") {
        return ntlm::nt_hash("Password");
    }
    return std::nullopt;
}

ntlm::InitiatorConfig client_config(std::string password = "Password")
{
    return ntlm::InitiatorConfig{
        .user = "User",
        .domain = "Domain",
        .password = SecretString(std::move(password)),
        .workstation = "COMPUTER",
        .channel_bindings = std::nullopt,
        .target_name = std::nullopt,
        .client_challenge = std::nullopt,
        .timestamp = std::nullopt,
        .random_session_key = std::nullopt,
    };
}

ntlm::AcceptorConfig server_config(farland::auth::NtlmVerifier& verifier)
{
    return ntlm::AcceptorConfig{
        .verifier = verifier,
        .netbios_domain = "WORKGROUP",
        .netbios_computer = "FARLAND",
        .dns_domain = "",
        .dns_computer = "",
        .channel_bindings = std::nullopt,
        .channel_binding_policy = ChannelBindingPolicy::verify_if_present,
        .server_challenge = std::nullopt,
        .timestamp = std::nullopt,
    };
}

struct Exchange {
    std::vector<std::byte> negotiate;
    std::vector<std::byte> challenge;
    std::vector<std::byte> authenticate;
};

/// NEGOTIATE and CHALLENGE through both contexts; the AUTHENTICATE is
/// returned without handing it to the acceptor.
Exchange start(ntlm::Initiator& client, ntlm::Acceptor& server)
{
    const auto negotiate = client.step({});
    REQUIRE(negotiate.has_value());
    CHECK_FALSE(negotiate->complete);
    const auto challenge = server.step(negotiate->token);
    REQUIRE(challenge.has_value());
    CHECK_FALSE(challenge->complete);
    const auto authenticate = client.step(challenge->token);
    REQUIRE(authenticate.has_value());
    CHECK(authenticate->complete);
    return {negotiate->token, challenge->token, authenticate->token};
}

void establish(ntlm::Initiator& client, ntlm::Acceptor& server)
{
    const auto exchange = start(client, server);
    const auto done = server.step(exchange.authenticate);
    REQUIRE(done.has_value());
    CHECK(done->complete);
    CHECK(done->token.empty());
}

/// Re-encodes the AUTHENTICATE after `edit`, which then carries a stale MIC.
template <class Edit>
std::vector<std::byte> edited(std::span<const std::byte> authenticate, Edit edit)
{
    auto message = ntlm::decode_authenticate(authenticate);
    REQUIRE(message.has_value());
    edit(*message);
    return ntlm::encode(*message);
}

}  // namespace

// ---------------------------------------------------------------------------
// Primitives

TEST_CASE("MD4 matches the RFC 1320 test suite")
{
    const std::array<std::pair<std::string_view, std::string_view>, 7> vectors{{
        {"", "31d6cfe0d16ae931b73c59d7e0c089c0"},
        {"a", "bde52cb31de33e46245e05fbdbd6fb24"},
        {"abc", "a448017aaf21d8525fc10ae87aa6729d"},
        {"message digest", "d9130a8164549fe818874806e1c7014b"},
        {"abcdefghijklmnopqrstuvwxyz", "d79e1c308aa5bbcdeea8ed63df412da9"},
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "043f8582f241db351ce627e153e7f0e4"},
        {"12345678901234567890123456789012345678901234567890123456789012345678901234567890",
         "e33b4ddc9c38f2199c3e7b164fcc0536"},
    }};
    for (const auto& [input, digest] : vectors) {
        CAPTURE(input);
        CHECK(bytes_of(legacy::md4(ascii(input))) == hex(digest));
    }
}

TEST_CASE("RC4 matches RFC 6229 and keeps its state across calls")
{
    struct Vector {
        std::string_view key;
        std::array<std::pair<std::size_t, std::string_view>, 6> stream;
    };
    const std::array<Vector, 2> vectors{{
        {"0102030405",
         {{{0, "b2396305f03dc027ccc3524a0a1118a8"},
           {16, "6982944f18fc82d589c403a47a0d0919"},
           {240, "28cb1132c96ce286421dcaadb8b69eae"},
           {256, "1cfcf62b03eddb641d77dfcf7f8d8c93"},
           {1008, "45129048e6a0ed0b56b490338f078da5"},
           {1024, "30abbcc7c20b01609f23ee2d5f6bb7df"}}}},
        {"0102030405060708090a0b0c0d0e0f10",
         {{{0, "9ac7cc9a609d1ef7b2932899cde41b97"},
           {16, "5248c4959014126a6e8a84f11d1a9e1c"},
           {240, "065902e4b620f6cc36c8589f66432f2b"},
           {256, "d39d566bc6bce3010768151549f3873f"},
           {1008, "e7a72574f8782ae26aabcf9ebcd66065"},
           {1024, "bdf0324e6083dcc6d3cedd3ca8c53c16"}}}},
    }};
    for (const auto& vector : vectors) {
        CAPTURE(vector.key);
        legacy::Rc4 rc4(hex(vector.key));
        std::vector<std::byte> keystream(1040);
        // Odd-sized chunks: the keystream must continue across calls.
        for (std::size_t offset = 0; offset < keystream.size(); offset += 7) {
            rc4.apply(std::span(keystream).subspan(offset, std::min<std::size_t>(7, keystream.size() - offset)));
        }
        for (const auto& [offset, expected] : vector.stream) {
            CAPTURE(offset);
            CHECK(bytes_of(std::span(keystream).subspan(offset, 16)) == hex(expected));
        }
    }
}

TEST_CASE("MD5 and HMAC-MD5 match RFC 1321 and RFC 2104")
{
    CHECK(bytes_of(legacy::md5({})) == hex("d41d8cd98f00b204e9800998ecf8427e"));
    CHECK(bytes_of(legacy::md5({ascii("abc")})) == hex("900150983cd24fb0d6963f7d28e17f72"));
    CHECK(bytes_of(legacy::md5({ascii("a"), ascii(""), ascii("bc")})) == hex("900150983cd24fb0d6963f7d28e17f72"));

    CHECK(bytes_of(legacy::hmac_md5(filled(0x0b), {ascii("Hi There")})) == hex("9294727a3638bb1c13f48ef8158bfc9d"));
    CHECK(bytes_of(legacy::hmac_md5(ascii("Jefe"), {ascii("what do ya "), ascii("want for nothing?")})) ==
          hex("750c783e6ab0b503eaa86e310a5db738"));
}

TEST_CASE("The NT hash is MD4 of the UTF-16LE password")
{
    // FreeRDP TestSspiNTLM: the empty password.
    CHECK(bytes_of(ntlm::nt_hash("")) == hex("31d6cfe0d16ae931b73c59d7e0c089c0"));
    // [MS-NLMP] 4.2.1 Passwd as UTF-16LE.
    CHECK(ntlm::nt_hash("Password") == legacy::md4(hex("50 00 61 00 73 00 73 00 77 00 6f 00 72 00 64 00")));
}

TEST_CASE("channel_bindings_hash follows RFC 5929 tls-server-end-point")
{
    // MD5(5 zero-length/zero-type u32 fields + length, "tls-server-end-point:" + SHA-256(cert)),
    // computed independently with Python's hashlib.
    CHECK(bytes_of(ntlm::channel_bindings_hash(ascii("farland test certificate"))) ==
          hex("52508a73de1bcb6b7f5b462b341d334a"));
}

// ---------------------------------------------------------------------------
// [MS-NLMP] 4.2.4 NTLMv2 Authentication vectors

TEST_CASE("[MS-NLMP] 4.2.4.1 and 4.2.4.2: NTLMv2 keys and responses")
{
    const auto server_challenge = array_of<8>(server_challenge_hex);
    const auto client_challenge = array_of<8>(client_challenge_hex);
    const auto random_session_key = array_of<16>(random_session_key_hex);
    constexpr std::uint32_t challenge_flags = 0xe28a8233U;  // 4.2.4

    // 4.2.4.1.1 NTOWFv2("Password", "User", "Domain")
    const auto response_key = ntlm::ntowf_v2(ntlm::nt_hash("Password"), "User", "Domain");
    CHECK(bytes_of(response_key) == hex("0c 86 8a 40 3b fd 7a 93 a3 00 1e f2 2e f0 2e 3f"));

    // 4.2.4.1.3 temp
    const std::vector<ntlm::AvPair> pairs{{2, utf16("Domain")}, {1, utf16("Server")}};
    const auto temp = ntlm::ntlmv2_temp(0, client_challenge, pairs);
    CHECK(temp == hex(spec_temp));

    // 4.2.4.2.2 NTLMv2 Response (NTProofStr)
    const auto proof = ntlm::nt_proof_str(response_key, server_challenge, temp);
    CHECK(bytes_of(proof) == hex("68 cd 0a b8 51 e5 1c 96 aa bc 92 7b eb ef 6a 1c"));

    // 4.2.4.1.2 Session Base Key
    const auto base = ntlm::session_base_key(response_key, proof);
    CHECK(bytes_of(base) == hex("8d e4 0c ca db c1 4a 82 f1 5c b0 ad 0d e9 5c a3"));

    // 4.2.4.2.1 LMv2 Response
    CHECK(bytes_of(ntlm::lmv2_response(response_key, server_challenge, client_challenge)) == hex(spec_lmv2_response));

    // 4.2.4.2.3 Encrypted Session Key: RC4(KeyExchangeKey = SessionBaseKey, RandomSessionKey)
    auto encrypted = random_session_key;
    legacy::Rc4(base).apply(encrypted);
    CHECK(bytes_of(encrypted) == hex("c5 da d2 54 4f c9 79 90 94 ce 1c e9 0b c9 d0 3e"));

    // 4.2.4.4 SEALKEY and SIGNKEY, client to server
    CHECK(bytes_of(ntlm::sealing_key(random_session_key, challenge_flags, ntlm::Direction::client_to_server)) ==
          hex("59 f6 00 97 3c c4 96 0a 25 48 0a 7c 19 6e 4c 58"));
    CHECK(bytes_of(ntlm::signing_key(random_session_key, ntlm::Direction::client_to_server)) ==
          hex("47 88 dc 86 1b 47 82 f3 5d 43 fd 98 fe 1a 2d 39"));
}

TEST_CASE("NTOWFv2 uppercases the user name but not the domain")
{
    const auto hash = ntlm::nt_hash("Password");
    CHECK(ntlm::ntowf_v2(hash, "user", "Domain") == ntlm::ntowf_v2(hash, "USER", "Domain"));
    CHECK(ntlm::ntowf_v2(hash, "jürgen", "Domain") == ntlm::ntowf_v2(hash, "JÜRGEN", "Domain"));
    CHECK(ntlm::ntowf_v2(hash, "ołga", "Domain") == ntlm::ntowf_v2(hash, "OŁGA", "Domain"));
    CHECK(ntlm::ntowf_v2(hash, "иван", "Domain") == ntlm::ntowf_v2(hash, "ИВАН", "Domain"));
    CHECK(ntlm::ntowf_v2(hash, "User", "domain") != ntlm::ntowf_v2(hash, "User", "Domain"));
}

TEST_CASE("[MS-NLMP] 4.2.4.3: the CHALLENGE_MESSAGE decodes and re-encodes byte for byte")
{
    const auto bytes = hex(spec_challenge);
    const auto challenge = ntlm::decode_challenge(bytes);
    REQUIRE(challenge.has_value());
    CHECK(challenge->flags == 0xe28a8233U);
    CHECK(bytes_of(challenge->server_challenge) == hex(server_challenge_hex));
    CHECK(challenge->target_name == utf16("Server"));
    REQUIRE(challenge->target_info.size() == 2);
    CHECK(challenge->target_info[0] == ntlm::AvPair{2, utf16("Domain")});
    CHECK(challenge->target_info[1] == ntlm::AvPair{1, utf16("Server")});
    CHECK(challenge->version == ntlm::Version{6, 0, 6000, 15});
    CHECK(ntlm::encode(*challenge) == bytes);
}

TEST_CASE("[MS-NLMP] 4.2.4.3: the AUTHENTICATE_MESSAGE decodes and re-encodes byte for byte")
{
    const auto bytes = hex(spec_authenticate);
    const auto authenticate = ntlm::decode_authenticate(bytes);
    REQUIRE(authenticate.has_value());
    CHECK(authenticate->flags == 0xe2888235U);
    CHECK(authenticate->domain == "Domain");
    CHECK(authenticate->user == "User");
    CHECK(authenticate->workstation == "COMPUTER");
    CHECK(authenticate->lm_challenge_response == hex(spec_lmv2_response));
    CHECK(authenticate->nt_challenge_response ==
          concat(hex("68 cd 0a b8 51 e5 1c 96 aa bc 92 7b eb ef 6a 1c"), hex(spec_temp)));
    CHECK(authenticate->encrypted_random_session_key == hex("c5 da d2 54 4f c9 79 90 94 ce 1c e9 0b c9 d0 3e"));
    CHECK(authenticate->version == ntlm::Version{5, 1, 2600, 15});
    CHECK_FALSE(authenticate->mic.has_value());  // The example predates the MIC.
    CHECK(ntlm::encode(*authenticate) == bytes);
}

TEST_CASE("[MS-NLMP] 4.2.4: the acceptor verifies the example and unseals the 4.2.4.4 message")
{
    ntlm::LocalNtlmVerifier verifier(lookup);
    auto config = server_config(verifier);
    config.server_challenge = array_of<8>(server_challenge_hex);
    ntlm::Acceptor server(std::move(config));
    ntlm::NegotiateMessage negotiate;
    negotiate.flags = 0xe2888235U;
    negotiate.version = ntlm::Version{5, 1, 2600, 15};
    REQUIRE(server.step(ntlm::encode(negotiate)).has_value());

    const auto done = server.step(hex(spec_authenticate));
    REQUIRE(done.has_value());
    CHECK(done->complete);
    CHECK(server.complete());
    CHECK(server.identity().user == "User");
    CHECK(server.identity().domain == "Domain");
    CHECK((server.negotiated_flags() & ntlm::flags::negotiate_key_exch) != 0);
    CHECK_FALSE(server.target_name().has_value());
    CHECK_FALSE(server.channel_bound());

    const auto plaintext = server.unwrap(concat(hex(spec_signature), hex(spec_sealed)));
    REQUIRE(plaintext.has_value());
    CHECK(*plaintext == hex(spec_plaintext));
}

TEST_CASE("[MS-NLMP] 4.2.4.4: the initiator seals the example message")
{
    auto config = client_config();
    config.client_challenge = array_of<8>(client_challenge_hex);
    config.timestamp = 0;
    config.random_session_key = array_of<16>(random_session_key_hex);
    ntlm::Initiator client(std::move(config));
    REQUIRE(client.step({}).has_value());
    const auto authenticate = client.step(hex(spec_challenge));
    REQUIRE(authenticate.has_value());
    CHECK(authenticate->complete);

    const auto decoded = ntlm::decode_authenticate(authenticate->token);
    REQUIRE(decoded.has_value());
    CHECK(decoded->lm_challenge_response == hex(spec_lmv2_response));  // No server timestamp: LMv2.
    CHECK(decoded->mic.has_value());

    ntlm::LocalNtlmVerifier verifier(lookup);
    CHECK(verifier.session_base_key("User", "Domain", array_of<8>(server_challenge_hex), decoded->nt_challenge_response)
              .has_value());

    CHECK(client.wrap(hex(spec_plaintext)) == concat(hex(spec_signature), hex(spec_sealed)));
}

// ---------------------------------------------------------------------------
// Initiator <-> Acceptor

TEST_CASE("Initiator and acceptor authenticate and exchange sealed messages")
{
    ntlm::LocalNtlmVerifier verifier(lookup);
    const auto bindings = ntlm::channel_bindings_hash(ascii("farland test certificate"));
    auto config = client_config();
    config.channel_bindings = bindings;
    config.target_name = "TERMSRV/farland.example";
    ntlm::Initiator client(std::move(config));
    ntlm::Acceptor server({
        .verifier = verifier,
        .netbios_domain = "FARLAND",
        .netbios_computer = "HOST",
        .dns_domain = "farland.example",
        .dns_computer = "host.farland.example",
        .channel_bindings = bindings,
        .channel_binding_policy = ChannelBindingPolicy::require,
        .server_challenge = std::nullopt,
        .timestamp = std::nullopt,
    });
    CHECK(bytes_of(server.mechanism()) == hex("2b 06 01 04 01 82 37 02 02 0a"));
    CHECK(bytes_of(client.mechanism()) == bytes_of(server.mechanism()));

    const auto exchange = start(client, server);
    const auto challenge = ntlm::decode_challenge(exchange.challenge);
    REQUIRE(challenge.has_value());
    const auto dns_computer = ntlm::find_av_pair(challenge->target_info, ntlm::AvId::dns_computer_name);
    REQUIRE(dns_computer.has_value());
    CHECK(bytes_of(*dns_computer) == utf16("host.farland.example"));
    CHECK(ntlm::find_av_pair(challenge->target_info, ntlm::AvId::timestamp).has_value());

    // The server sent MsvAvTimestamp, so the client sends Z(24) as its LM response.
    const auto authenticate = ntlm::decode_authenticate(exchange.authenticate);
    REQUIRE(authenticate.has_value());
    CHECK(authenticate->lm_challenge_response == std::vector<std::byte>(24));

    REQUIRE(server.step(exchange.authenticate).has_value());
    CHECK(server.complete());
    CHECK(client.complete());
    CHECK(server.identity().user == "User");
    CHECK(server.identity().domain == "Domain");
    CHECK(client.identity().user == "User");
    CHECK(server.target_name() == std::optional<std::string>("TERMSRV/farland.example"));
    CHECK(server.channel_bound());
    CHECK(server.negotiated_flags() == client.negotiated_flags());
    CHECK((server.negotiated_flags() & ntlm::flags::negotiate_128) != 0);

    for (int i = 0; i < 5; ++i) {
        const auto message = bytes_of(ascii("message number " + std::string(static_cast<std::size_t>(i), 'x')));
        const auto up = client.wrap(message);
        CHECK(up.size() == message.size() + 16);
        CHECK(sequence_of(up) == static_cast<std::uint32_t>(i));
        const auto received = server.unwrap(up);
        REQUIRE(received.has_value());
        CHECK(*received == message);

        const auto down = server.wrap(message);
        CHECK(sequence_of(down) == static_cast<std::uint32_t>(i));
        CHECK(down != up);  // Different keys per direction.
        const auto back = client.unwrap(down);
        REQUIRE(back.has_value());
        CHECK(*back == message);
    }
    const auto empty = server.unwrap(client.wrap({}));
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

TEST_CASE("get_mic and verify_mic share the sequence numbers with wrap")
{
    ntlm::LocalNtlmVerifier verifier(lookup);
    ntlm::Initiator client(client_config());
    ntlm::Acceptor server(server_config(verifier));
    establish(client, server);
    const auto message = bytes_of(ascii("mechTypes"));

    const auto mic = client.get_mic(message);
    CHECK(mic.size() == 16);
    CHECK(sequence_of(mic) == 0);
    CHECK(server.verify_mic(message, mic).has_value());

    const auto wrapped = client.wrap(message);
    CHECK(sequence_of(wrapped) == 1);
    CHECK(server.unwrap(wrapped).has_value());

    const auto server_mic = server.get_mic(message);
    CHECK(sequence_of(server_mic) == 0);
    CHECK(client.verify_mic(message, server_mic).has_value());

    auto tampered = client.get_mic(message);
    tampered[5] ^= std::byte{1};
    CHECK_FALSE(server.verify_mic(message, tampered).has_value());
    CHECK_FALSE(server.verify_mic(bytes_of(ascii("other")), client.get_mic(message)).has_value());
    const auto short_mic = client.get_mic(message);
    CHECK_FALSE(server.verify_mic(message, std::span(short_mic).first(15)).has_value());

    // Failures consume a sequence number on both sides, so they stay in step.
    CHECK(server.verify_mic(message, client.get_mic(message)).has_value());
}

TEST_CASE("Tampered, replayed and reordered wrapped messages are rejected")
{
    ntlm::LocalNtlmVerifier verifier(lookup);
    ntlm::Initiator client(client_config());
    ntlm::Acceptor server(server_config(verifier));
    establish(client, server);
    const auto message = bytes_of(ascii("TSCredentials"));

    SECTION("signature")
    {
        auto wrapped = client.wrap(message);
        wrapped[6] ^= std::byte{0x80};
        CHECK_FALSE(server.unwrap(wrapped).has_value());
    }
    SECTION("sequence number")
    {
        auto wrapped = client.wrap(message);
        wrapped[12] = std::byte{7};
        CHECK_FALSE(server.unwrap(wrapped).has_value());
    }
    SECTION("ciphertext")
    {
        auto wrapped = client.wrap(message);
        wrapped.back() ^= std::byte{1};
        CHECK_FALSE(server.unwrap(wrapped).has_value());
    }
    SECTION("replay")
    {
        const auto wrapped = client.wrap(message);
        CHECK(server.unwrap(wrapped).has_value());
        CHECK_FALSE(server.unwrap(wrapped).has_value());
    }
    SECTION("reordering")
    {
        const auto first = client.wrap(message);
        const auto second = client.wrap(message);
        CHECK_FALSE(server.unwrap(second).has_value());
        static_cast<void>(first);
    }
    SECTION("truncated signature")
    {
        const auto wrapped = client.wrap({});
        const auto result = server.unwrap(std::span(wrapped).first(15));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Errc::truncated);
    }
}

TEST_CASE("reset_cipher_state restarts RC4 and sequence numbers on both sides")
{
    ntlm::LocalNtlmVerifier verifier(lookup);
    ntlm::Initiator client(client_config());
    ntlm::Acceptor server(server_config(verifier));
    establish(client, server);
    const auto message = bytes_of(ascii("mechTypes"));

    const auto first = client.get_mic(message);
    REQUIRE(server.verify_mic(message, first).has_value());
    REQUIRE(client.verify_mic(message, server.get_mic(message)).has_value());

    client.reset_cipher_state();
    server.reset_cipher_state();
    CHECK(client.get_mic(message) == first);  // Same key stream, same sequence number.
    server.reset_cipher_state();
    client.reset_cipher_state();
    const auto wrapped = client.wrap(message);
    CHECK(sequence_of(wrapped) == 0);
    const auto plaintext = server.unwrap(wrapped);
    REQUIRE(plaintext.has_value());
    CHECK(*plaintext == message);
}

// ---------------------------------------------------------------------------
// Acceptor rejections

TEST_CASE("The acceptor rejects wrong passwords and unknown users")
{
    ntlm::LocalNtlmVerifier verifier(lookup);
    ntlm::Acceptor server(server_config(verifier));

    SECTION("wrong password")
    {
        ntlm::Initiator client(client_config("password"));
        const auto exchange = start(client, server);
        const auto result = server.step(exchange.authenticate);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Errc::invalid_value);
    }
    SECTION("unknown user")
    {
        auto config = client_config();
        config.user = "Nobody";
        ntlm::Initiator client(std::move(config));
        const auto exchange = start(client, server);
        CHECK_FALSE(server.step(exchange.authenticate).has_value());
    }
    CHECK_FALSE(server.complete());
    CHECK(server.identity().user.empty());
    // A failed context stays failed.
    CHECK_FALSE(server.step(hex(spec_authenticate)).has_value());
}

TEST_CASE("The acceptor verifies the MIC")
{
    ntlm::LocalNtlmVerifier verifier(lookup);
    ntlm::Initiator client(client_config());
    ntlm::Acceptor server(server_config(verifier));
    const auto exchange = start(client, server);
    const std::size_t mic_offset = ntlm::authenticate_mic_offset(client.negotiated_flags());
    REQUIRE(mic_offset == 72);

    SECTION("intact")
    {
        CHECK(server.step(exchange.authenticate).has_value());
    }
    SECTION("tampered MIC")
    {
        auto message = exchange.authenticate;
        message[mic_offset + 3] ^= std::byte{1};
        const auto result = server.step(message);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().what == "NTLM MIC does not verify");
    }
    SECTION("tampered header flags")
    {
        auto message = exchange.authenticate;
        message[60] &= ~std::byte{ntlm::flags::negotiate_seal};  // NegotiateFlags, lowest byte
        const auto result = server.step(message);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().what == "NTLM MIC does not verify");
    }
    SECTION("MIC missing although MsvAvFlags announces it")
    {
        const auto message = edited(exchange.authenticate, [](ntlm::AuthenticateMessage& m) { m.mic.reset(); });
        const auto result = server.step(message);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().what == "NTLM MIC announced but missing");
    }
    SECTION("MIC zeroed")
    {
        const auto message =
            edited(exchange.authenticate, [](ntlm::AuthenticateMessage& m) { m.mic = std::array<std::byte, 16>{}; });
        CHECK_FALSE(server.step(message).has_value());
    }
}

TEST_CASE("Channel binding policies")
{
    ntlm::LocalNtlmVerifier verifier(lookup);
    const auto ours = filled(0x11);
    const auto theirs = filled(0x22);

    const auto run = [&](ChannelBindingPolicy policy, std::optional<std::array<std::byte, 16>> server_bindings,
                         std::optional<std::array<std::byte, 16>> client_bindings) {
        auto config = client_config();
        config.channel_bindings = client_bindings;
        ntlm::Initiator client(std::move(config));
        auto settings = server_config(verifier);
        settings.channel_bindings = server_bindings;
        settings.channel_binding_policy = policy;
        ntlm::Acceptor server(std::move(settings));
        const auto exchange = start(client, server);
        const auto result = server.step(exchange.authenticate);
        return std::pair{result.has_value() ? std::string_view{} : result.error().what, server.channel_bound()};
    };

    CHECK(run(ChannelBindingPolicy::require, ours, ours) == std::pair{std::string_view{}, true});
    CHECK(run(ChannelBindingPolicy::require, ours, std::nullopt).first == "NTLM channel bindings missing");
    CHECK(run(ChannelBindingPolicy::require, ours, filled(0)).first == "NTLM channel bindings missing");
    CHECK(run(ChannelBindingPolicy::require, ours, theirs).first == "NTLM channel bindings do not match");
    CHECK(run(ChannelBindingPolicy::verify_if_present, ours, ours) == std::pair{std::string_view{}, true});
    CHECK(run(ChannelBindingPolicy::verify_if_present, ours, std::nullopt) == std::pair{std::string_view{}, false});
    CHECK(run(ChannelBindingPolicy::verify_if_present, ours, theirs).first == "NTLM channel bindings do not match");
    CHECK(run(ChannelBindingPolicy::verify_if_present, std::nullopt, theirs) == std::pair{std::string_view{}, false});
    CHECK(run(ChannelBindingPolicy::ignore, ours, theirs) == std::pair{std::string_view{}, false});
}

TEST_CASE("The acceptor rejects NTLMv1, anonymous logons and missing session security")
{
    ntlm::LocalNtlmVerifier verifier(lookup);
    ntlm::Initiator client(client_config());
    ntlm::Acceptor server(server_config(verifier));
    const auto exchange = start(client, server);

    SECTION("NTLMv1-length response")
    {
        const auto message =
            edited(exchange.authenticate, [](ntlm::AuthenticateMessage& m) { m.nt_challenge_response.resize(24); });
        const auto result = server.step(message);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == Errc::unsupported);
        CHECK(result.error().what == "NTLMv1 response");
    }
    SECTION("anonymous")
    {
        const auto message = edited(exchange.authenticate, [](ntlm::AuthenticateMessage& m) {
            m.user.clear();
            m.nt_challenge_response.clear();
            m.lm_challenge_response.assign(1, std::byte{0});
        });
        const auto result = server.step(message);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().what == "anonymous NTLM logon");
    }
    SECTION("anonymous flag")
    {
        const auto message =
            edited(exchange.authenticate, [](ntlm::AuthenticateMessage& m) { m.flags |= ntlm::flags::anonymous; });
        CHECK_FALSE(server.step(message).has_value());
    }
    SECTION("no extended session security in AUTHENTICATE")
    {
        const auto message = edited(exchange.authenticate, [](ntlm::AuthenticateMessage& m) {
            m.flags &= ~ntlm::flags::negotiate_extended_sessionsecurity;
        });
        CHECK(server.step(message).error().code == Errc::unsupported);
    }
    SECTION("short EncryptedRandomSessionKey")
    {
        const auto message = edited(exchange.authenticate,
                                    [](ntlm::AuthenticateMessage& m) { m.encrypted_random_session_key.resize(8); });
        CHECK_FALSE(server.step(message).has_value());
    }
}

TEST_CASE("The acceptor requires Unicode, NTLM and extended session security in NEGOTIATE")
{
    ntlm::LocalNtlmVerifier verifier(lookup);
    ntlm::Acceptor server(server_config(verifier));
    ntlm::NegotiateMessage negotiate;
    negotiate.flags = ntlm::flags::negotiate_unicode | ntlm::flags::negotiate_ntlm;
    const auto result = server.step(ntlm::encode(negotiate));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Errc::unsupported);
}

TEST_CASE("The initiator requires key exchange from the server")
{
    auto challenge = hex(spec_challenge);
    challenge[23] &= ~std::byte{0x40};  // NTLMSSP_NEGOTIATE_KEY_EXCH (bit 30 of the flags at offset 20)
    ntlm::Initiator client(client_config());
    REQUIRE(client.step({}).has_value());
    const auto result = client.step(challenge);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == Errc::unsupported);
    CHECK_FALSE(client.complete());
}

TEST_CASE("LocalNtlmVerifier::verify_password compares NT hashes")
{
    ntlm::LocalNtlmVerifier verifier(lookup);
    CHECK(verifier.verify_password("User", "Domain", "Password"));
    CHECK_FALSE(verifier.verify_password("User", "Domain", "password"));
    CHECK_FALSE(verifier.verify_password("Nobody", "Domain", "Password"));
}

// ---------------------------------------------------------------------------
// Codec strictness

TEST_CASE("AUTHENTICATE decoding rejects truncated and overlapping payload fields")
{
    const auto good = hex(spec_authenticate);
    REQUIRE(ntlm::decode_authenticate(good).has_value());

    for (std::size_t size = 0; size < good.size(); ++size) {
        CAPTURE(size);
        CHECK_FALSE(ntlm::decode_authenticate(std::span(good).first(size)).has_value());
    }

    auto message = good;
    SECTION("overlapping fields")
    {
        message[40] = std::byte{0x4a};  // UserName offset inside DomainName
        const auto result = ntlm::decode_authenticate(message);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().what == "NTLM payload fields overlap");
    }
    SECTION("field inside the header")
    {
        message[32] = std::byte{0x30};  // DomainName offset
        const auto result = ntlm::decode_authenticate(message);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().what == "NTLM payload field overlaps the header");
    }
    SECTION("field past the end")
    {
        message[52] = std::byte{0x11};  // EncryptedRandomSessionKey length
        const auto result = ntlm::decode_authenticate(message);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().what == "NTLM payload field outside the message");
    }
    SECTION("huge offset")
    {
        message[51] = std::byte{0xff};  // Workstation offset, high byte
        CHECK_FALSE(ntlm::decode_authenticate(message).has_value());
    }
    SECTION("odd-length name")
    {
        message[28] = std::byte{0x0b};  // DomainName length
        CHECK(ntlm::decode_authenticate(message).error().what == "odd-length UTF-16 name");
    }
    SECTION("OEM encoding")
    {
        message[60] &= ~std::byte{0x01};
        CHECK(ntlm::decode_authenticate(message).error().code == Errc::unsupported);
    }
    SECTION("wrong message type")
    {
        CHECK_FALSE(ntlm::decode_challenge(message).has_value());
        message[0] = std::byte{'X'};
        CHECK(ntlm::decode_authenticate(message).error().what == "not an NTLMSSP message");
    }
}

TEST_CASE("CHALLENGE decoding rejects truncation and bad target info")
{
    const auto good = hex(spec_challenge);
    for (std::size_t size = 0; size < good.size(); ++size) {
        CAPTURE(size);
        CHECK_FALSE(ntlm::decode_challenge(std::span(good).first(size)).has_value());
    }
    auto message = good;
    message[0x40] = std::byte{0x05};  // TargetInfo length: cuts the MsvAvEOL
    message[0x28] = std::byte{0x20};
    CHECK_FALSE(ntlm::decode_challenge(message).has_value());
}

TEST_CASE("Names are capped at 256 UTF-16 code units and may not contain NUL")
{
    ntlm::AuthenticateMessage message;
    message.flags = ntlm::flags::negotiate_unicode;
    message.user = std::string(256, 'u');
    CHECK(ntlm::decode_authenticate(ntlm::encode(message)).has_value());
    message.user = std::string(257, 'u');
    CHECK(ntlm::decode_authenticate(ntlm::encode(message)).error().code == Errc::limit_exceeded);
    message.user = std::string("a\0b", 3);
    CHECK(ntlm::decode_authenticate(ntlm::encode(message)).error().what == "NUL in NTLM name");
}

TEST_CASE("AV pair lists need MsvAvEOL, unique ids, sane lengths and a bounded count")
{
    const auto decode = [](std::span<const std::byte> bytes) {
        farland::Reader r(bytes);
        return ntlm::decode_av_pairs(r);
    };
    CHECK(decode(hex("00 00 00 00")).value().empty());
    CHECK(decode(hex("02 00 02 00 41 00")).error().code == Errc::truncated);
    CHECK(decode(hex("02 00 02 00 41 00 02 00 00 00 00 00")).error().what == "duplicate AV pair");
    CHECK(decode(hex("06 00 02 00 02 00 00 00 00 00")).error().what == "AV pair has the wrong length");
    CHECK(decode(hex("00 00 01 00 00")).error().what == "MsvAvEOL has a value");

    farland::Writer w;
    for (std::uint16_t id = 0x100; id < 0x100 + ntlm::max_av_pairs + 1; ++id) {
        w.u16le(id);
        w.u16le(0);
    }
    w.u32le(0);
    CHECK(decode(w.view()).error().code == Errc::limit_exceeded);

    const std::vector<ntlm::AvPair> pairs{{7, std::vector<std::byte>(8)}, {0x1234, hex("01 02 03")}};
    farland::Writer out;
    ntlm::encode_av_pairs(out, pairs);
    CHECK(decode(out.view()).value() == pairs);
}

TEST_CASE("NEGOTIATE_MESSAGE round trip")
{
    const ntlm::NegotiateMessage message{
        .flags = ntlm::flags::negotiate_unicode | ntlm::flags::negotiate_oem_domain_supplied |
                 ntlm::flags::negotiate_version,
        .domain = bytes_of(ascii("DOMAIN")),
        .workstation = bytes_of(ascii("WS")),
        .version = ntlm::Version{},
    };
    const auto bytes = ntlm::encode(message);
    CHECK(bytes.size() == 40 + 8);
    CHECK(ntlm::decode_negotiate(bytes).value() == message);
    CHECK_FALSE(ntlm::decode_negotiate(std::span(bytes).first(31)).has_value());
}
