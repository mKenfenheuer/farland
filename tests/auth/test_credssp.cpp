// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/credssp.hpp>
#include <farland/auth/spnego.hpp>
#include <farland/base/text.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace auth = farland::auth;
namespace credssp = farland::auth::credssp;
namespace spnego = farland::auth::spnego;
using Bytes = std::vector<std::byte>;
using Status = auth::NlaAcceptor::Status;
using farland::Errc;
using farland::Result;
using farland::SecretString;
using farland::test::hex;

namespace {

Bytes to_bytes(std::string_view text)
{
    Bytes out;
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

std::string to_text(std::span<const std::byte> bytes)
{
    std::string out;
    for (const std::byte b : bytes) {
        out.push_back(std::to_integer<char>(b));
    }
    return out;
}

Bytes oid_bytes(std::span<const std::byte> oid)
{
    return {oid.begin(), oid.end()};
}

/// 1.3.6.1.4.1.99999.1: a stand-in for a mechanism farland lacks (Kerberos, NEGOEX).
const Bytes& other_oid()
{
    static const Bytes oid = hex("2b 06 01 04 01 86 8d 1f 01");
    return oid;
}

/// Shaped like a DER RSAPublicKey; only its bytes matter.
Bytes make_public_key(std::uint8_t seed)
{
    Bytes key = hex("30 81 89 02 81 81 00");
    for (unsigned i = 0; i < 128; ++i) {
        key.push_back(static_cast<std::byte>((i * 7U + seed) & 0xFFU));
    }
    key.insert(key.end(), {std::byte{0x02}, std::byte{0x03}, std::byte{0x01}, std::byte{0x00}, std::byte{0x01}});
    return key;
}

const Bytes& server_key()
{
    static const Bytes key = make_public_key(1);
    return key;
}

credssp::Nonce test_nonce()
{
    credssp::Nonce nonce{};
    for (std::size_t i = 0; i < nonce.size(); ++i) {
        nonce[i] = static_cast<std::byte>(i);
    }
    return nonce;
}

auth::PasswordCredentials alice()
{
    return auth::PasswordCredentials{.domain = "FARLAND", .user = "alice", .password = SecretString("Secret1!")};
}

// A mock mechanism ---------------------------------------------------------

constexpr std::uint64_t fnv_offset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t fnv_prime = 0x100000001b3ULL;

std::uint64_t fnv(std::uint64_t hash, std::span<const std::byte> data)
{
    for (const std::byte b : data) {
        hash ^= std::to_integer<std::uint64_t>(b);
        hash *= fnv_prime;
    }
    return hash;
}

std::uint64_t fnv(std::uint64_t hash, std::uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) {
        hash ^= (value >> (8U * i)) & 0xFFU;
        hash *= fnv_prime;
    }
    return hash;
}

void put_le(Bytes& out, std::uint64_t value, unsigned size)
{
    for (unsigned i = 0; i < size; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
    }
}

std::uint64_t get_le(std::span<const std::byte> bytes)
{
    std::uint64_t value = 0;
    for (std::size_t i = bytes.size(); i-- > 0;) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(bytes[i]);
    }
    return value;
}

using Log = std::vector<std::string>;

/// Three legs shaped like NTLM (NEGOTIATE, CHALLENGE, AUTHENTICATE, with the
/// "NTLMSSP\0" signature for the NTLM OID), a password-derived key, and a
/// keyed, sequence-numbered wrap and MIC. A wrong password, a flipped bit, a
/// replay or a reordered call makes the peer's check fail. The optional log
/// records the order of calls.
class MockMechanism final : public auth::SecurityContext {
public:
    enum class Role : std::uint8_t { initiator, acceptor };

    MockMechanism(Role role, Bytes oid, std::string password, auth::Identity identity,
                  std::shared_ptr<Log> log = nullptr)
        : role_(role), oid_(std::move(oid)), password_(std::move(password)), identity_(std::move(identity)),
          log_(std::move(log))
    {
    }

    [[nodiscard]] std::span<const std::byte> mechanism() const noexcept override { return oid_; }
    [[nodiscard]] bool complete() const noexcept override { return complete_; }
    [[nodiscard]] const auth::Identity& identity() const noexcept override { return identity_; }

    [[nodiscard]] Result<auth::Step> step(std::span<const std::byte> input) override
    {
        record("step");
        return role_ == Role::initiator ? initiator_step(input) : acceptor_step(input);
    }

    [[nodiscard]] Bytes wrap(std::span<const std::byte> plaintext) override
    {
        record("wrap");
        const std::uint32_t seq = send_seq_++;
        Bytes out;
        put_le(out, seq, 4);
        put_le(out, mac(send_direction(), seq, plaintext), 8);
        const Bytes sealed = keystream(send_direction(), seq, plaintext);
        out.insert(out.end(), sealed.begin(), sealed.end());
        return out;
    }

    [[nodiscard]] Result<Bytes> unwrap(std::span<const std::byte> wrapped) override
    {
        record("unwrap");
        if (!complete_ || wrapped.size() < header_size) {
            return farland::fail(Errc::invalid_value, "mock: cannot unwrap");
        }
        const auto seq = static_cast<std::uint32_t>(get_le(wrapped.first(4)));
        if (seq != recv_seq_) {
            return farland::fail(Errc::invalid_value, "mock: sequence number out of order");
        }
        ++recv_seq_;
        Bytes plaintext = keystream(recv_direction(), seq, wrapped.subspan(header_size));
        if (get_le(wrapped.subspan(4, 8)) != mac(recv_direction(), seq, plaintext)) {
            return farland::fail(Errc::invalid_value, "mock: wrap checksum mismatch");
        }
        return plaintext;
    }

    [[nodiscard]] Bytes get_mic(std::span<const std::byte> message) override
    {
        record("get_mic");
        const std::uint32_t seq = send_seq_++;
        Bytes out;
        put_le(out, seq, 4);
        put_le(out, mac(send_direction() + mic_domain, seq, message), 8);
        return out;
    }

    [[nodiscard]] Result<void> verify_mic(std::span<const std::byte> message, std::span<const std::byte> mic) override
    {
        record("verify_mic");
        if (!complete_ || mic.size() != header_size) {
            return farland::fail(Errc::invalid_value, "mock: malformed MIC");
        }
        const auto seq = static_cast<std::uint32_t>(get_le(mic.first(4)));
        if (seq != recv_seq_) {
            return farland::fail(Errc::invalid_value, "mock: MIC sequence number out of order");
        }
        ++recv_seq_;
        if (get_le(mic.subspan(4, 8)) != mac(recv_direction() + mic_domain, seq, message)) {
            return farland::fail(Errc::invalid_value, "mock: MIC mismatch");
        }
        return {};
    }

    /// Like NTLM after the mechListMIC: both directions start over at sequence number 0.
    void reset_cipher_state() override
    {
        record("reset");
        send_seq_ = 0;
        recv_seq_ = 0;
    }

    /// The receive direction only, as a Windows client does after verifying
    /// the acceptor's mechListMIC ([MS-SPNG] 3.3.5.1 keeps a handle per direction).
    void reset_receive() { recv_seq_ = 0; }

private:
    static constexpr std::size_t header_size = 12;
    static constexpr std::uint64_t mic_domain = 2;

    void record(std::string_view call)
    {
        if (log_) {
            log_->emplace_back(call);
        }
    }

    [[nodiscard]] Bytes message(char type, std::span<const std::byte> body) const
    {
        Bytes out = prefix();
        out.push_back(static_cast<std::byte>(type));
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }

    [[nodiscard]] Bytes prefix() const
    {
        return std::ranges::equal(oid_, spnego::ntlm_oid) ? to_bytes(std::string_view("NTLMSSP\0", 8))
                                                          : to_bytes("MOCKMECH");
    }

    [[nodiscard]] std::optional<std::span<const std::byte>> body(std::span<const std::byte> token, char type) const
    {
        const Bytes expected = prefix();
        if (token.size() <= expected.size() || !std::ranges::equal(token.first(expected.size()), expected) ||
            token[expected.size()] != static_cast<std::byte>(type)) {
            return std::nullopt;
        }
        return token.subspan(expected.size() + 1);
    }

    [[nodiscard]] std::uint64_t derive_key(std::span<const std::byte> challenge) const
    {
        return fnv(fnv(fnv_offset, to_bytes(password_)), challenge);
    }

    Result<auth::Step> initiator_step(std::span<const std::byte> input)
    {
        if (state_ == 0 && input.empty()) {
            state_ = 1;
            return auth::Step{message('N', {}), false};
        }
        if (state_ == 1) {
            const auto challenge = body(input, 'C');
            if (!challenge || challenge->size() != 8) {
                return farland::fail(Errc::invalid_value, "mock: expected a CHALLENGE");
            }
            key_ = derive_key(*challenge);
            Bytes proof;
            put_le(proof, fnv(key_, to_bytes("proof")), 8);
            const Bytes user = to_bytes(identity_.user);
            proof.insert(proof.end(), user.begin(), user.end());
            state_ = 2;
            complete_ = true;
            return auth::Step{message('A', proof), true};
        }
        return farland::fail(Errc::invalid_value, "mock: unexpected initiator input");
    }

    Result<auth::Step> acceptor_step(std::span<const std::byte> input)
    {
        if (state_ == 0) {
            if (!body(input, 'N')) {
                return farland::fail(Errc::invalid_value, "mock: expected a NEGOTIATE");
            }
            challenge_.clear();
            put_le(challenge_, fnv(fnv_offset, to_bytes(password_)) ^ 0x1234U, 8);
            state_ = 1;
            return auth::Step{message('C', challenge_), false};
        }
        if (state_ == 1) {
            const auto rest = body(input, 'A');
            if (!rest || rest->size() < 8) {
                return farland::fail(Errc::invalid_value, "mock: expected an AUTHENTICATE");
            }
            key_ = derive_key(challenge_);
            if (get_le(rest->first(8)) != fnv(key_, to_bytes("proof"))) {
                return farland::fail(Errc::invalid_value, "mock: wrong password");
            }
            identity_.user = to_text(rest->subspan(8));
            state_ = 2;
            complete_ = true;
            return auth::Step{{}, true};
        }
        return farland::fail(Errc::invalid_value, "mock: unexpected acceptor input");
    }

    [[nodiscard]] std::uint64_t send_direction() const { return role_ == Role::initiator ? 0 : 1; }
    [[nodiscard]] std::uint64_t recv_direction() const { return role_ == Role::initiator ? 1 : 0; }

    [[nodiscard]] std::uint64_t mac(std::uint64_t direction, std::uint32_t seq, std::span<const std::byte> data) const
    {
        return fnv(fnv(fnv(key_, direction), seq), data);
    }

    [[nodiscard]] Bytes keystream(std::uint64_t direction, std::uint32_t seq, std::span<const std::byte> data) const
    {
        std::uint64_t state = fnv(fnv(key_ ^ 0x5555U, direction), seq) | 1U;
        Bytes out;
        for (const std::byte b : data) {
            state ^= state << 13U;
            state ^= state >> 7U;
            state ^= state << 17U;
            out.push_back(b ^ static_cast<std::byte>(state & 0xFFU));
        }
        return out;
    }

    Role role_;
    Bytes oid_;
    std::string password_;
    auth::Identity identity_;
    std::shared_ptr<Log> log_;
    int state_ = 0;
    bool complete_ = false;
    Bytes challenge_;
    std::uint64_t key_ = 0;
    std::uint32_t send_seq_ = 0;
    std::uint32_t recv_seq_ = 0;
};

std::unique_ptr<MockMechanism> client_mechanism(std::string password = "Secret1!", std::shared_ptr<Log> log = nullptr)
{
    return std::make_unique<MockMechanism>(MockMechanism::Role::initiator, oid_bytes(spnego::ntlm_oid),
                                           std::move(password), auth::Identity{"alice", "FARLAND"}, std::move(log));
}

credssp::AcceptorConfig acceptor_config(std::string password = "Secret1!", std::shared_ptr<Log> log = nullptr)
{
    credssp::AcceptorConfig config;
    config.server_public_key = server_key();
    config.make_mechanism = [password = std::move(password), log = std::move(log)](
                                std::span<const std::byte> oid) -> std::unique_ptr<auth::SecurityContext> {
        if (!std::ranges::equal(oid, spnego::ntlm_oid)) {
            return nullptr;
        }
        return std::make_unique<MockMechanism>(MockMechanism::Role::acceptor, oid_bytes(spnego::ntlm_oid), password,
                                               auth::Identity{"", "FARLAND"}, log);
    };
    return config;
}

credssp::TsRequest decode(std::span<const std::byte> message)
{
    auto request = credssp::decode_ts_request(message);
    REQUIRE(request.has_value());
    return *request;
}

spnego::NegTokenResp neg_token_resp(const credssp::TsRequest& request)
{
    REQUIRE(request.nego_tokens.size() == 1);
    const auto token = spnego::decode(request.nego_tokens.front());
    REQUIRE(token.has_value());
    const auto* resp = std::get_if<spnego::NegTokenResp>(&*token);
    REQUIRE(resp != nullptr);
    return *resp;
}

/// Feeds one TSRequest to the acceptor and decodes its answer.
credssp::TsRequest exchange(credssp::Acceptor& acceptor, const credssp::TsRequest& request)
{
    acceptor.receive(credssp::encode(request));
    const Bytes answer = acceptor.take_output();
    INFO(acceptor.failure_reason());
    REQUIRE_FALSE(answer.empty());
    return decode(answer);
}

bool sends_error_code(std::uint32_t version)
{
    return version == 3 || version == 4 || version == 6;
}

/// frame_ts_request, which must not fail.
std::optional<std::size_t> frame(std::span<const std::byte> stream)
{
    const auto size = credssp::frame_ts_request(stream);
    REQUIRE(size.has_value());
    return *size;
}

/// Rewrites TSRequests in flight.
std::function<void(Bytes&)> rewrite(std::function<void(credssp::TsRequest&)> edit)
{
    return [edit = std::move(edit)](Bytes& message) {
        auto request = decode(message);
        edit(request);
        message = credssp::encode(request);
    };
}

// An Initiator talking to an Acceptor -------------------------------------------

struct Options {
    std::uint32_t client_version = 6;
    std::uint32_t server_max_version = 6;
    bool spnego = true;
    bool other_mechanism_first = false;
    std::string client_password = "Secret1!";
    std::string server_password = "Secret1!";
    Bytes client_key = server_key();
    std::function<bool(const auth::PasswordCredentials&, const auth::Identity&, std::span<const std::byte>)>
        accept_credentials;
};

class Harness {
public:
    explicit Harness(Options options)
        : options_(std::move(options)), acceptor_(make_acceptor_config()), initiator_(make_initiator_config())
    {
    }

    /// Relays messages until neither side has anything to say.
    void run(const std::function<void(Bytes&)>& to_server = {}, const std::function<void(Bytes&)>& to_client = {})
    {
        for (int round = 0; round < 16; ++round) {
            Bytes up = initiator_.take_output();
            if (!up.empty()) {
                if (to_server) {
                    to_server(up);
                }
                client_messages.push_back(up);
                acceptor_.receive(up);
            }
            Bytes down = acceptor_.take_output();
            if (!down.empty()) {
                if (to_client) {
                    to_client(down);
                }
                server_messages.push_back(down);
                initiator_.receive(down);
            }
            if (up.empty() && down.empty()) {
                return;
            }
        }
        FAIL("the handshake does not settle");
    }

    credssp::Acceptor& acceptor() { return acceptor_; }
    credssp::Initiator& initiator() { return initiator_; }

    /// Checks that the acceptor failed and, where the version allows, told the client.
    void require_rejected()
    {
        INFO(acceptor_.failure_reason());
        CHECK(acceptor_.status() == Status::failed);
        CHECK_FALSE(acceptor_.failure_reason().empty());
        CHECK_FALSE(acceptor_.take_credentials().has_value());
        REQUIRE_FALSE(server_messages.empty());
        const auto last = decode(server_messages.back());
        if (sends_error_code(options_.client_version)) {
            CHECK(last.error_code == credssp::status_logon_failure);
            CHECK(initiator_.status() == Status::failed);
            CHECK(initiator_.server_error_code() == credssp::status_logon_failure);
        } else {
            CHECK_FALSE(last.error_code.has_value());
        }
    }

    std::shared_ptr<Log> acceptor_log = std::make_shared<Log>();
    std::shared_ptr<Log> initiator_log = std::make_shared<Log>();
    std::vector<Bytes> client_messages;
    std::vector<Bytes> server_messages;

private:
    credssp::AcceptorConfig make_acceptor_config()
    {
        auto config = acceptor_config(options_.server_password, acceptor_log);
        config.max_version = options_.server_max_version;
        config.accept_credentials = options_.accept_credentials;
        return config;
    }

    credssp::InitiatorConfig make_initiator_config()
    {
        credssp::InitiatorConfig config;
        config.mechanism = client_mechanism(options_.client_password, initiator_log);
        config.use_spnego = options_.spnego;
        config.server_public_key = options_.client_key;
        config.credentials = alice();
        config.version = options_.client_version;
        config.client_nonce = test_nonce();
        if (options_.other_mechanism_first) {
            config.preferred_mech_types = {other_oid()};
        }
        return config;
    }

    Options options_;
    credssp::Acceptor acceptor_;
    credssp::Initiator initiator_;
};

/// A raw-NTLM client driven by hand up to the point where it delegates credentials.
void handshake_until_credentials(credssp::Acceptor& acceptor, MockMechanism& client)
{
    auto negotiate = client.step({});
    REQUIRE(negotiate.has_value());
    credssp::TsRequest first;
    first.nego_tokens.push_back(negotiate->token);
    const auto challenge = exchange(acceptor, first);
    REQUIRE(challenge.nego_tokens.size() == 1);

    auto authenticate = client.step(challenge.nego_tokens.front());
    REQUIRE(authenticate.has_value());
    credssp::TsRequest second;
    second.nego_tokens.push_back(authenticate->token);
    second.client_nonce = test_nonce();
    second.pub_key_auth = client.wrap(credssp::client_to_server_hash(test_nonce(), server_key()));
    const auto answer = exchange(acceptor, second);
    REQUIRE(answer.pub_key_auth.has_value());
    REQUIRE(client.unwrap(*answer.pub_key_auth).has_value());
}

}  // namespace

// Codecs -------------------------------------------------------------------

TEST_CASE("TSRequest with a negoToken, hand-encoded ([MS-CSSP] 2.2.1, 2.2.1.1)")
{
    const auto der = hex("30 12 a0 03 02 01 06"           // version 6
                         "a1 0b 30 09 30 07 a0 05 04 03"  // negoTokens: SEQUENCE OF SEQUENCE { [0] OCTET STRING }
                         "01 02 03");
    const auto request = credssp::decode_ts_request(der);
    REQUIRE(request.has_value());
    CHECK(request->version == 6);
    CHECK(request->nego_tokens == std::vector<Bytes>{hex("01 02 03")});
    CHECK_FALSE(request->auth_info.has_value());
    CHECK_FALSE(request->pub_key_auth.has_value());
    CHECK_FALSE(request->error_code.has_value());
    CHECK_FALSE(request->client_nonce.has_value());
    CHECK(credssp::encode(*request) == der);
}

TEST_CASE("TSRequest errorCode is a signed 32-bit NTSTATUS ([MS-CSSP] 2.2.1)")
{
    credssp::TsRequest request;
    request.error_code = credssp::status_logon_failure;
    const auto four_bytes = hex("30 0d a0 03 02 01 06 a4 06 02 04 c0 00 00 6d");
    CHECK(credssp::encode(request) == four_bytes);
    CHECK(credssp::decode_ts_request(four_bytes) == request);

    const auto five_bytes = hex("30 0e a0 03 02 01 06 a4 07 02 05 00 c0 00 00 6d");
    CHECK(credssp::decode_ts_request(five_bytes) == request);

    const auto too_large = credssp::decode_ts_request(hex("30 0e a0 03 02 01 06 a4 07 02 05 01 00 00 00 00"));
    REQUIRE_FALSE(too_large.has_value());
    CHECK(too_large.error().code == Errc::invalid_value);
}

TEST_CASE("TSRequest with every field round-trips ([MS-CSSP] 2.2.1)")
{
    credssp::TsRequest request;
    request.version = 5;
    request.nego_tokens = {hex("01"), Bytes(200, std::byte{0x22})};
    request.auth_info = hex("aa bb");
    request.pub_key_auth = Bytes(300, std::byte{0x33});
    request.error_code = 0x80090308;
    request.client_nonce = test_nonce();
    const auto encoded = credssp::encode(request);
    CHECK(credssp::decode_ts_request(encoded) == request);
    CHECK(frame(encoded) == std::optional<std::size_t>(encoded.size()));
}

TEST_CASE("TSRequest decoding rejects malformed input")
{
    const auto error = [](std::string_view text) {
        const auto result = credssp::decode_ts_request(hex(text));
        REQUIRE_FALSE(result.has_value());
        return result.error().code;
    };
    CHECK(error("30 0a a0 03 02 01 06 a5 03 04 01 00") == Errc::invalid_length);                // 1-byte clientNonce
    CHECK(error("30 0f a0 03 02 01 06 a3 03 04 01 aa a2 03 04 01 bb") == Errc::trailing_data);  // [3] before [2]
    CHECK(error("30 0a a0 03 02 01 06 a6 03 02 01 00") == Errc::trailing_data);                 // unknown [6]
    CHECK(error("30 05 a1 03 30 01 00") == Errc::invalid_value);                                // no version
    CHECK(error("30 05 a0 03 02 01 ff") == Errc::invalid_value);                                // negative version
    CHECK(error("30 09 a0 03 02 01 06 a1 02 30 00") == Errc::invalid_length);                   // empty negoTokens
    CHECK(error("30 05 a0 03 02 01 06 00") == Errc::trailing_data);                             // bytes after it
    CHECK(error("30 81 05 a0 03 02 01 06") == Errc::invalid_length);                            // non-minimal length
    CHECK(error("30 07 a0 05 02 03 00 00 06") == Errc::invalid_value);                          // non-minimal INTEGER
}

TEST_CASE("TSRequest framing waits for the header and caps the size at 64 KiB")
{
    CHECK(frame({}) == std::optional<std::size_t>());
    CHECK(frame(hex("30")) == std::optional<std::size_t>());
    CHECK(frame(hex("30 82 01")) == std::optional<std::size_t>());
    CHECK(frame(hex("30 05 a0")) == std::optional<std::size_t>(7));
    CHECK(frame(hex("30 82 ff fc")) == std::optional<std::size_t>(65536));

    const auto too_large = credssp::frame_ts_request(hex("30 82 ff fd"));
    REQUIRE_FALSE(too_large.has_value());
    CHECK(too_large.error().code == Errc::limit_exceeded);
    const auto not_a_sequence = credssp::frame_ts_request(hex("31 00"));
    REQUIRE_FALSE(not_a_sequence.has_value());
    CHECK(not_a_sequence.error().code == Errc::invalid_value);
}

TEST_CASE("TSCredentials with TSPasswordCreds, hand-encoded ([MS-CSSP] 2.2.1.2, 2.2.1.2.1)")
{
    const auto der = hex("30 1b a0 03 02 01 01 a1 14 04 12"  // credType 1, credentials
                         "30 10 a0 02 04 00"                 // domainName ""
                         "a1 04 04 02 75 00"                 // userName "u"
                         "a2 04 04 02 70 00");               // password "p"
    const auto credentials = credssp::decode_ts_credentials(der);
    REQUIRE(credentials.has_value());
    const auto& password = std::get<credssp::TsPasswordCreds>(credentials->credentials);
    const auto converted = credssp::to_password_credentials(password);
    CHECK(converted.domain.empty());
    CHECK(converted.user == "u");
    CHECK(converted.password.view() == "p");
    CHECK(credssp::encode(*credentials) == der);
}

TEST_CASE("Password credentials convert between UTF-8 and UTF-16LE")
{
    const auth::PasswordCredentials original{
        .domain = "M\xc3\x9cNCHEN", .user = "j\xc3\xbcrgen", .password = SecretString("p\xc3\xa4sswort")};
    const auto encoded = credssp::encode(credssp::to_ts_credentials(original));
    const auto decoded = credssp::decode_ts_credentials(encoded);
    REQUIRE(decoded.has_value());
    const auto back = credssp::to_password_credentials(std::get<credssp::TsPasswordCreds>(decoded->credentials));
    CHECK(back.domain == original.domain);
    CHECK(back.user == original.user);
    CHECK(back.password.view() == original.password.view());
}

TEST_CASE("TSSmartCardCreds and TSRemoteGuardCreds are checked and kept raw ([MS-CSSP] 2.2.1.2.2, 2.2.1.2.3)")
{
    SECTION("smart card")
    {
        const auto der = hex("30 1a a0 03 02 01 02 a1 13 04 11"
                             "30 0f a0 04 04 02 31 00"       // pin "1"
                             "a1 07 30 05 a0 03 02 01 01");  // cspData { keySpec 1 }
        const auto credentials = credssp::decode_ts_credentials(der);
        REQUIRE(credentials.has_value());
        const auto& smart_card = std::get<credssp::TsSmartCardCreds>(credentials->credentials);
        CHECK(std::ranges::equal(smart_card.der.view(), std::span(der).subspan(11)));
        CHECK(credssp::encode(*credentials) == der);
    }
    SECTION("smart card without cspData")
    {
        const auto credentials = credssp::decode_ts_credentials(hex("30 11 a0 03 02 01 02 a1 0a 04 08"
                                                                    "30 06 a0 04 04 02 31 00"));
        CHECK_FALSE(credentials.has_value());
    }
    SECTION("remote guard")
    {
        const auto der = hex("30 1b a0 03 02 01 06 a1 14 04 12"
                             "30 10 a0 0e 30 0c"        // logonCred
                             "a0 06 04 04 4b 00 52 00"  // packageName "KR"
                             "a1 02 04 00");            // credBuffer
        const auto credentials = credssp::decode_ts_credentials(der);
        REQUIRE(credentials.has_value());
        CHECK(std::holds_alternative<credssp::TsRemoteGuardCreds>(credentials->credentials));
        CHECK(credssp::encode(*credentials) == der);
    }
    SECTION("unknown credType")
    {
        const auto credentials = credssp::decode_ts_credentials(hex("30 0a a0 03 02 01 03 a1 03 04 01 00"));
        REQUIRE_FALSE(credentials.has_value());
        CHECK(credentials.error().code == Errc::unsupported);
    }
}

TEST_CASE("Public key binding hashes ([MS-CSSP] 3.1.5)")
{
    const auto key = to_bytes("KEY");
    const auto client = credssp::client_to_server_hash(test_nonce(), key);
    const auto server = credssp::server_to_client_hash(test_nonce(), key);
    // Computed independently with Python's hashlib.
    CHECK(std::ranges::equal(client, hex("08376256112fb8dca0623f0150e885931162e9a53db5add81a0424731ecda3ec")));
    CHECK(std::ranges::equal(server, hex("4a0320a9bf5de2c9132f600a3d1a093bafbe5f2d8fab71b64efd6a051d29575d")));
}

// Handshakes -------------------------------------------------------------------

TEST_CASE("CredSSP handshake completes for versions 2 to 6 ([MS-CSSP] 3.1.5)")
{
    const std::uint32_t version = GENERATE(2U, 3U, 4U, 5U, 6U);
    const int mode = GENERATE(0, 1, 2);  // raw NTLM, SPNEGO, SPNEGO with the mechanism not first
    CAPTURE(version, mode);
    Options options;
    options.client_version = version;
    options.spnego = mode != 0;
    options.other_mechanism_first = mode == 2;
    Harness h(std::move(options));
    h.run();

    INFO(h.acceptor().failure_reason());
    INFO(h.initiator().failure_reason());
    REQUIRE(h.acceptor().status() == Status::succeeded);
    REQUIRE(h.initiator().status() == Status::succeeded);
    CHECK(h.acceptor().version() == version);
    CHECK(h.initiator().version() == version);
    CHECK(h.acceptor().used_spnego() == (mode != 0));
    CHECK(h.acceptor().identity().user == "alice");
    CHECK(h.acceptor().identity().domain == "FARLAND");
    CHECK(h.acceptor().failure_reason().empty());
    CHECK(h.acceptor().take_remaining_input().empty());

    const auto credentials = h.acceptor().take_credentials();
    REQUIRE(credentials.has_value());
    CHECK(credentials->domain == "FARLAND");
    CHECK(credentials->user == "alice");
    CHECK(credentials->password.view() == "Secret1!");
    CHECK_FALSE(h.acceptor().take_credentials().has_value());

    // [MS-CSSP] 3.1.5 step 3: the last token travels with pubKeyAuth (and the v5+ nonce).
    REQUIRE(h.client_messages.size() >= 3);
    const auto with_key = decode(h.client_messages.at(h.client_messages.size() - 2));
    CHECK(with_key.nego_tokens.size() == (mode == 2 ? 0U : 1U));
    CHECK(with_key.pub_key_auth.has_value());
    CHECK(with_key.client_nonce.has_value() == (version >= 5));
    const auto last = decode(h.client_messages.back());
    CHECK(last.auth_info.has_value());
    CHECK_FALSE(last.pub_key_auth.has_value());

    const auto answer = decode(h.server_messages.back());
    CHECK(answer.pub_key_auth.has_value());
    CHECK(answer.version == version);

    if (mode == 2) {
        // Init, NEGOTIATE, AUTHENTICATE + MIC, pubKeyAuth, authInfo: pubKeyAuth
        // waits for the acceptor's MIC, because reset_cipher_state() restarts
        // both directions and must precede the first wrap.
        CHECK(h.client_messages.size() == 5);
        const auto first_reply = neg_token_resp(decode(h.server_messages.front()));
        CHECK(first_reply.neg_state == spnego::NegState::request_mic);
        CHECK(first_reply.supported_mech == oid_bytes(spnego::ntlm_oid));
        CHECK_FALSE(first_reply.response_token.has_value());
        const auto last_token = decode(h.client_messages.at(2));
        CHECK_FALSE(last_token.pub_key_auth.has_value());
        CHECK(neg_token_resp(last_token).mech_list_mic.has_value());
        const auto final_reply = neg_token_resp(decode(h.server_messages.at(2)));
        CHECK(final_reply.neg_state == spnego::NegState::accept_completed);
        CHECK(final_reply.mech_list_mic.has_value());
        CHECK(answer.nego_tokens.empty());
        CHECK(*h.acceptor_log == Log{"step", "step", "verify_mic", "get_mic", "reset", "unwrap", "wrap", "unwrap"});
        CHECK(*h.initiator_log ==
              Log{"step", "step", "get_mic", "reset", "verify_mic", "reset", "wrap", "unwrap", "wrap"});
    } else {
        CHECK(h.client_messages.size() == 3);
        CHECK(*h.acceptor_log == Log{"step", "step", "unwrap", "wrap", "unwrap"});
        CHECK(*h.initiator_log == Log{"step", "step", "wrap", "unwrap", "wrap"});
    }
    if (mode == 1) {
        const auto first_reply = neg_token_resp(decode(h.server_messages.front()));
        CHECK(first_reply.neg_state == spnego::NegState::accept_incomplete);
        CHECK(first_reply.supported_mech == oid_bytes(spnego::ntlm_oid));
        CHECK(first_reply.response_token.has_value());
        const auto final_reply = neg_token_resp(answer);
        CHECK(final_reply.neg_state == spnego::NegState::accept_completed);
        CHECK_FALSE(final_reply.mech_list_mic.has_value());
    }
}

TEST_CASE("The version in use is the lower of client and server ([MS-CSSP] 2.2.1)")
{
    const std::uint32_t server_max = GENERATE(2U, 4U, 5U);
    Options options;
    options.server_max_version = server_max;
    Harness h(std::move(options));
    h.run();
    INFO(h.acceptor().failure_reason());
    REQUIRE(h.acceptor().status() == Status::succeeded);
    REQUIRE(h.initiator().status() == Status::succeeded);
    CHECK(h.acceptor().version() == server_max);
    CHECK(h.initiator().version() == server_max);
}

TEST_CASE("A tampered pubKeyAuth, a wrong nonce or a wrong public key fail the handshake ([MS-CSSP] 3.1.5)")
{
    const std::uint32_t version = GENERATE(2U, 3U, 4U, 5U, 6U);
    const bool use_spnego = GENERATE(false, true);
    CAPTURE(version, use_spnego);
    Options options;
    options.client_version = version;
    options.spnego = use_spnego;

    SECTION("tampered client pubKeyAuth")
    {
        Harness h(options);
        h.run(rewrite([](credssp::TsRequest& request) {
            if (request.pub_key_auth) {
                request.pub_key_auth->back() ^= std::byte{0x01};
            }
        }));
        h.require_rejected();
    }
    SECTION("wrong clientNonce")
    {
        if (version >= 5) {
            Harness h(options);
            h.run(rewrite([](credssp::TsRequest& request) {
                if (request.pub_key_auth && request.client_nonce) {
                    request.client_nonce->front() ^= std::byte{0x01};
                }
            }));
            h.require_rejected();
            CHECK(h.acceptor().failure_reason().find("TLS public key") != std::string_view::npos);
        }
    }
    SECTION("client binds to another public key")
    {
        options.client_key = make_public_key(2);
        Harness h(options);
        h.run();
        h.require_rejected();
    }
    SECTION("tampered server pubKeyAuth")
    {
        Harness h(options);
        h.run({}, rewrite([](credssp::TsRequest& request) {
                  if (request.pub_key_auth) {
                      request.pub_key_auth->back() ^= std::byte{0x01};
                  }
              }));
        CHECK(h.initiator().status() == Status::failed);
        CHECK(h.acceptor().status() == Status::in_progress);
        CHECK_FALSE(h.acceptor().take_credentials().has_value());
    }
    SECTION("server answers with the client's own pubKeyAuth (replay)")
    {
        Bytes client_pub_key_auth;
        Harness h(options);
        h.run(
            [&](Bytes& message) {
                const auto request = decode(message);
                if (request.pub_key_auth) {
                    client_pub_key_auth = *request.pub_key_auth;
                }
            },
            rewrite([&](credssp::TsRequest& request) {
                if (request.pub_key_auth) {
                    request.pub_key_auth = client_pub_key_auth;
                }
            }));
        CHECK(h.initiator().status() == Status::failed);
    }
}

TEST_CASE("A mechanism failure fails the handshake with errorCode ([MS-CSSP] 3.1.5 step 2)")
{
    const std::uint32_t version = GENERATE(2U, 3U, 4U, 5U, 6U);
    const bool use_spnego = GENERATE(false, true);
    CAPTURE(version, use_spnego);
    Options options;
    options.client_version = version;
    options.spnego = use_spnego;
    options.client_password = "wrong";
    Harness h(std::move(options));
    h.run();
    h.require_rejected();
    CHECK(h.acceptor().failure_reason().find("wrong password") != std::string_view::npos);
}

TEST_CASE("Rejected delegated credentials fail the handshake")
{
    std::string seen_user;
    Options options;
    options.accept_credentials = [&](const auth::PasswordCredentials& credentials, const auth::Identity& identity,
                                    std::span<const std::byte> mech_oid) {
        seen_user = identity.user + "/" + credentials.user;
        // The mechanism is named too, so a policy can tell NTLM from
        // Kerberos: here it is always NTLM.
        CHECK(std::ranges::equal(mech_oid, auth::spnego::ntlm_oid));
        return false;
    };
    Harness h(std::move(options));
    h.run();
    CHECK(seen_user == "alice/alice");
    CHECK(h.acceptor().status() == Status::failed);
    CHECK_FALSE(h.acceptor().take_credentials().has_value());
    REQUIRE_FALSE(h.server_messages.empty());
    CHECK(decode(h.server_messages.back()).error_code == credssp::status_logon_failure);
    // The initiator was done once it sent authInfo; the errorCode is left for the next layer.
    CHECK(h.initiator().status() == Status::succeeded);
    CHECK_FALSE(h.initiator().take_remaining_input().empty());
}

TEST_CASE("SPNEGO acceptor restarts the client on NTLM when the optimistic token is for another mechanism "
          "(RFC 4178 3.2, 5)")
{
    credssp::Acceptor acceptor(acceptor_config());
    MockMechanism client(MockMechanism::Role::initiator, oid_bytes(spnego::ntlm_oid), "Secret1!", {"alice", "FARLAND"});

    const auto init =
        spnego::make_neg_token_init({other_oid(), oid_bytes(spnego::ntlm_oid)}, to_bytes("MOCKMECH optimistic"));
    credssp::TsRequest first;
    first.nego_tokens.push_back(spnego::encode(init));
    auto reply = neg_token_resp(exchange(acceptor, first));
    CHECK(reply.neg_state == spnego::NegState::request_mic);
    CHECK(reply.supported_mech == oid_bytes(spnego::ntlm_oid));
    CHECK_FALSE(reply.response_token.has_value());
    CHECK_FALSE(reply.mech_list_mic.has_value());

    auto negotiate = client.step({});
    REQUIRE(negotiate.has_value());
    spnego::NegTokenResp second_token;
    second_token.response_token = negotiate->token;
    credssp::TsRequest second;
    second.nego_tokens.push_back(spnego::encode(second_token));
    reply = neg_token_resp(exchange(acceptor, second));
    CHECK(reply.neg_state == spnego::NegState::accept_incomplete);
    CHECK_FALSE(reply.supported_mech.has_value());
    REQUIRE(reply.response_token.has_value());

    auto authenticate = client.step(*reply.response_token);
    REQUIRE(authenticate.has_value());
    REQUIRE(authenticate->complete);
    spnego::NegTokenResp third_token;
    third_token.response_token = authenticate->token;
    credssp::TsRequest third;
    third.client_nonce = test_nonce();

    SECTION("with the client's mechListMIC and pubKeyAuth in one TSRequest, as Windows sends them")
    {
        // A Windows client keeps its RC4 handle per direction ([MS-SPNG]
        // 3.3.5.1): it restarts sending after its MIC and receiving after the
        // acceptor's, so it can send pubKeyAuth along with its last token.
        third_token.mech_list_mic = client.get_mic(init.mech_types_der);
        client.reset_cipher_state();
        third.nego_tokens.push_back(spnego::encode(third_token));
        third.pub_key_auth = client.wrap(credssp::client_to_server_hash(test_nonce(), server_key()));
        const auto answer = exchange(acceptor, third);
        reply = neg_token_resp(answer);
        CHECK(reply.neg_state == spnego::NegState::accept_completed);
        REQUIRE(reply.mech_list_mic.has_value());
        CHECK(client.verify_mic(init.mech_types_der, *reply.mech_list_mic).has_value());
        client.reset_receive();
        REQUIRE(answer.pub_key_auth.has_value());
        const auto server_hash = client.unwrap(*answer.pub_key_auth);
        REQUIRE(server_hash.has_value());
        CHECK(std::ranges::equal(*server_hash, credssp::server_to_client_hash(test_nonce(), server_key())));

        credssp::TsRequest fourth;
        fourth.auth_info = client.wrap(credssp::encode(credssp::to_ts_credentials(alice())));
        acceptor.receive(credssp::encode(fourth));
        INFO(acceptor.failure_reason());
        CHECK(acceptor.status() == Status::succeeded);
        CHECK(acceptor.take_output().empty());
    }
    SECTION("without the client's mechListMIC the acceptor fails")
    {
        third.nego_tokens.push_back(spnego::encode(third_token));
        third.pub_key_auth = client.wrap(credssp::client_to_server_hash(test_nonce(), server_key()));
        acceptor.receive(credssp::encode(third));
        CHECK(acceptor.status() == Status::failed);
        CHECK(acceptor.failure_reason().find("mechListMIC") != std::string_view::npos);
        CHECK(decode(acceptor.take_output()).error_code == credssp::status_logon_failure);
    }
    SECTION("a tampered mechListMIC fails")
    {
        auto mic = client.get_mic(init.mech_types_der);
        mic.back() ^= std::byte{0x80};
        third_token.mech_list_mic = mic;
        third.nego_tokens.push_back(spnego::encode(third_token));
        third.pub_key_auth = client.wrap(credssp::client_to_server_hash(test_nonce(), server_key()));
        acceptor.receive(credssp::encode(third));
        CHECK(acceptor.status() == Status::failed);
        CHECK(decode(acceptor.take_output()).error_code == credssp::status_logon_failure);
    }
    SECTION("a MIC over different mechTypes fails (downgrade)")
    {
        const std::vector<spnego::Oid> ntlm_only{oid_bytes(spnego::ntlm_oid)};
        third_token.mech_list_mic = client.get_mic(spnego::encode_mech_types(ntlm_only));
        third.nego_tokens.push_back(spnego::encode(third_token));
        third.pub_key_auth = client.wrap(credssp::client_to_server_hash(test_nonce(), server_key()));
        acceptor.receive(credssp::encode(third));
        CHECK(acceptor.status() == Status::failed);
    }
}

TEST_CASE("Smart-card and Remote Credential Guard credentials are rejected ([MS-CSSP] 2.2.1.2)")
{
    credssp::Acceptor acceptor(acceptor_config());
    MockMechanism client(MockMechanism::Role::initiator, oid_bytes(spnego::ntlm_oid), "Secret1!", {"alice", "FARLAND"});
    handshake_until_credentials(acceptor, client);

    credssp::TsRequest last;
    std::string_view expected;
    SECTION("smart card")
    {
        last.auth_info = client.wrap(hex("30 1a a0 03 02 01 02 a1 13 04 11 30 0f a0 04 04 02 31 00"
                                         "a1 07 30 05 a0 03 02 01 01"));
        expected = "smart-card";
    }
    SECTION("remote guard")
    {
        last.auth_info = client.wrap(hex("30 1b a0 03 02 01 06 a1 14 04 12 30 10 a0 0e 30 0c"
                                         "a0 06 04 04 4b 00 52 00 a1 02 04 00"));
        expected = "Remote Credential Guard";
    }
    acceptor.receive(credssp::encode(last));
    CHECK(acceptor.status() == Status::failed);
    CHECK(acceptor.failure_reason().find(expected) != std::string_view::npos);
    CHECK_FALSE(acceptor.take_credentials().has_value());
    CHECK(decode(acceptor.take_output()).error_code == credssp::status_logon_failure);
}

TEST_CASE("Bytes after the last TSRequest come out of take_remaining_input()")
{
    const auto rdp = hex("03 00 00 0b 06 e0 00 00 00 00 00");
    Harness h(Options{});
    h.run([&](Bytes& message) {
        if (decode(message).auth_info) {
            message.insert(message.end(), rdp.begin(), rdp.end());
        }
    });
    REQUIRE(h.acceptor().status() == Status::succeeded);
    CHECK(h.acceptor().take_remaining_input() == rdp);
    CHECK(h.acceptor().take_remaining_input().empty());
    h.acceptor().receive(hex("aa bb"));
    CHECK(h.acceptor().take_remaining_input() == hex("aa bb"));
    CHECK(h.acceptor().take_output().empty());
}

TEST_CASE("TSRequests delivered one byte at a time")
{
    const bool use_spnego = GENERATE(false, true);
    auto initiator_config = credssp::InitiatorConfig{};
    initiator_config.mechanism = client_mechanism();
    initiator_config.use_spnego = use_spnego;
    initiator_config.server_public_key = server_key();
    initiator_config.credentials = alice();
    credssp::Initiator initiator(std::move(initiator_config));
    credssp::Acceptor acceptor(acceptor_config());

    for (int round = 0; round < 8; ++round) {
        const Bytes up = initiator.take_output();
        for (std::size_t i = 0; i < up.size(); ++i) {
            acceptor.receive(std::span(up).subspan(i, 1));
        }
        const Bytes down = acceptor.take_output();
        for (std::size_t i = 0; i < down.size(); ++i) {
            initiator.receive(std::span(down).subspan(i, 1));
        }
    }
    INFO(acceptor.failure_reason());
    CHECK(acceptor.status() == Status::succeeded);
    CHECK(initiator.status() == Status::succeeded);
}

TEST_CASE("The acceptor fails on protocol violations")
{
    credssp::Acceptor acceptor(acceptor_config());
    SECTION("not a TSRequest: no errorCode, the version is unknown")
    {
        acceptor.receive(hex("04 03 01 02 03"));
        CHECK(acceptor.status() == Status::failed);
        CHECK(acceptor.take_output().empty());
    }
    SECTION("a TSRequest above 64 KiB")
    {
        acceptor.receive(hex("30 83 01 00 00"));
        CHECK(acceptor.status() == Status::failed);
    }
    SECTION("CredSSP version 1")
    {
        credssp::TsRequest request;
        request.version = 1;
        request.nego_tokens.push_back(to_bytes(std::string_view("NTLMSSP\0N", 9)));
        acceptor.receive(credssp::encode(request));
        CHECK(acceptor.status() == Status::failed);
        CHECK(acceptor.take_output().empty());
    }
    SECTION("the client changes its version")
    {
        MockMechanism client(MockMechanism::Role::initiator, oid_bytes(spnego::ntlm_oid), "Secret1!",
                             {"alice", "FARLAND"});
        credssp::TsRequest first;
        first.nego_tokens.push_back(client.step({})->token);
        const auto challenge = exchange(acceptor, first);
        credssp::TsRequest second;
        second.version = 4;
        second.nego_tokens.push_back(client.step(challenge.nego_tokens.front())->token);
        acceptor.receive(credssp::encode(second));
        CHECK(acceptor.status() == Status::failed);
        CHECK(decode(acceptor.take_output()).error_code == credssp::status_logon_failure);
    }
    SECTION("the client sends an errorCode")
    {
        credssp::TsRequest request;
        request.error_code = credssp::status_logon_failure;
        acceptor.receive(credssp::encode(request));
        CHECK(acceptor.status() == Status::failed);
    }
    SECTION("no mechanism in mechTypes that farland supports")
    {
        credssp::TsRequest request;
        request.nego_tokens.push_back(spnego::encode(spnego::make_neg_token_init({other_oid()})));
        acceptor.receive(credssp::encode(request));
        CHECK(acceptor.status() == Status::failed);
        CHECK(decode(acceptor.take_output()).error_code == credssp::status_logon_failure);
    }
    SECTION("a first token that is neither NTLM nor SPNEGO")
    {
        credssp::TsRequest request;
        request.nego_tokens.push_back(hex("01 02 03"));
        acceptor.receive(credssp::encode(request));
        CHECK(acceptor.status() == Status::failed);
    }
    SECTION("authInfo before pubKeyAuth")
    {
        credssp::TsRequest request;
        request.nego_tokens.push_back(to_bytes(std::string_view("NTLMSSP\0N", 9)));
        request.auth_info = hex("00");
        acceptor.receive(credssp::encode(request));
        CHECK(acceptor.status() == Status::failed);
    }
    CHECK_FALSE(acceptor.take_credentials().has_value());
    acceptor.receive(hex("30 00"));
    CHECK(acceptor.take_remaining_input().empty());
}

TEST_CASE("A raw NTLM token without an NTLM mechanism fails")
{
    auto config = acceptor_config();
    config.make_mechanism = [](std::span<const std::byte>) -> std::unique_ptr<auth::SecurityContext> {
        return nullptr;
    };
    credssp::Acceptor acceptor(std::move(config));
    credssp::TsRequest request;
    request.nego_tokens.push_back(to_bytes(std::string_view("NTLMSSP\0N", 9)));
    acceptor.receive(credssp::encode(request));
    CHECK(acceptor.status() == Status::failed);
    CHECK(acceptor.failure_reason().find("NTLM is not available") != std::string_view::npos);
    CHECK(decode(acceptor.take_output()).error_code == credssp::status_logon_failure);
}
