// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/credssp.hpp>
#include <farland/auth/openssl.hpp>
#include <farland/base/assert.hpp>
#include <farland/base/ber.hpp>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <bit>
#include <format>
#include <limits>
#include <utility>

namespace farland::auth::credssp {

namespace {

constexpr auto der = ber::Rules::der;

// [MS-CSSP] 3.1.5 steps 3 and 4. std::to_array keeps the terminating NUL,
// which the hash includes.
constexpr auto client_server_magic = std::to_array("CredSSP Client-To-Server Binding Hash");
constexpr auto server_client_magic = std::to_array("CredSSP Server-To-Client Binding Hash");

using MdCtx = std::unique_ptr<EVP_MD_CTX, ossl::Deleter<&EVP_MD_CTX_free>>;

Hash binding_hash(std::span<const char> magic, const Nonce& nonce, std::span<const std::byte> key)
{
    const MdCtx context(EVP_MD_CTX_new());
    std::array<unsigned char, std::tuple_size_v<Hash>> digest{};
    unsigned int size = 0;
    const bool ok = context && EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1 &&
                    EVP_DigestUpdate(context.get(), magic.data(), magic.size()) == 1 &&
                    EVP_DigestUpdate(context.get(), nonce.data(), nonce.size()) == 1 &&
                    EVP_DigestUpdate(context.get(), key.data(), key.size()) == 1 &&
                    EVP_DigestFinal_ex(context.get(), digest.data(), &size) == 1;
    FARLAND_ASSERT(ok && size == digest.size());
    return std::bit_cast<Hash>(digest);
}

Nonce random_nonce()
{
    std::array<unsigned char, nonce_size> bytes{};
    const int rc = RAND_bytes(bytes.data(), static_cast<int>(bytes.size()));
    FARLAND_ASSERT(rc == 1);
    return std::bit_cast<Nonce>(bytes);
}

bool equal_constant_time(std::span<const std::byte> a, std::span<const std::byte> b)
{
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

/// [MS-CSSP] 3.1.5 step 4, v2-4: "adds 1 to the first byte" of the public key.
Bytes public_key_plus_one(std::span<const std::byte> key)
{
    Bytes out(key.begin(), key.end());
    FARLAND_ASSERT(!out.empty());
    out.front() = static_cast<std::byte>(std::to_integer<unsigned>(out.front()) + 1U);
    return out;
}

std::unexpected<std::string> reject(std::string reason)
{
    return std::unexpected(std::move(reason));
}

std::string describe(std::string_view what, const Error& error)
{
    return std::string(what) + ": " + error.message();
}

// DER helpers --------------------------------------------------------------

bool next_is(const Reader& r, ber::Tag tag)
{
    if (r.empty()) {
        return false;
    }
    const auto next = ber::peek_tag(r, der);
    return next.has_value() && *next == tag;
}

Result<Reader> read_field(Reader& r, std::uint32_t number)
{
    return ber::read_constructed(r, ber::context(number), der);
}

/// `[number] OCTET STRING` of at most `limit` bytes.
Result<Bytes> read_octets_field(Reader& r, std::uint32_t number, std::size_t limit = max_field_size)
{
    FARLAND_TRY(Reader field, read_field(r, number));
    FARLAND_TRY(const ber::Tlv tlv, ber::expect_tlv(field, ber::tags::octet_string, der));
    FARLAND_TRY_VOID(field.expect_end("explicitly tagged OCTET STRING"));
    if (tlv.value.size() > limit) {
        return fail(Errc::limit_exceeded, "CredSSP field too large", tlv.value_offset);
    }
    return Bytes(tlv.value.begin(), tlv.value.end());
}

template <class Body>
void write_field(Writer& w, std::uint32_t number, Body&& body)
{
    ber::write_constructed(w, ber::context(number), std::forward<Body>(body));
}

void write_octets_field(Writer& w, std::uint32_t number, std::span<const std::byte> value)
{
    write_field(w, number, [&](Writer& field) { ber::write_octet_string(field, value); });
}

// TSCredentials -----------------------------------------------------------

// [MS-CSSP] 2.2.1.2.1 TSPasswordCreds ::= SEQUENCE {
//   domainName [0] OCTET STRING, userName [1] OCTET STRING, password [2] OCTET STRING }
Result<TsPasswordCreds> decode_password_creds(Reader& r)
{
    FARLAND_TRY(Reader seq, ber::read_constructed(r, ber::tags::sequence, der));
    FARLAND_TRY_VOID(r.expect_end("TSPasswordCreds"));
    TsPasswordCreds out;
    FARLAND_TRY(out.domain_name, read_octets_field(seq, 0));
    FARLAND_TRY(out.user_name, read_octets_field(seq, 1));
    FARLAND_TRY(Bytes password, read_octets_field(seq, 2));
    out.password = SecretBytes(std::move(password));
    FARLAND_TRY_VOID(seq.expect_end("TSPasswordCreds"));
    return out;
}

// [MS-CSSP] 2.2.1.2.2 TSSmartCardCreds ::= SEQUENCE {
//   pin [0] OCTET STRING, cspData [1] TSCspDataDetail,
//   userHint [2] OCTET STRING OPTIONAL, domainHint [3] OCTET STRING OPTIONAL }
// [MS-CSSP] 2.2.1.2.2.1 TSCspDataDetail ::= SEQUENCE {
//   keySpec [0] INTEGER, cardName [1] .. cspName [4] OCTET STRING OPTIONAL }
Result<void> check_smart_card_creds(Reader& r)
{
    FARLAND_TRY(Reader seq, ber::read_constructed(r, ber::tags::sequence, der));
    FARLAND_TRY_VOID(r.expect_end("TSSmartCardCreds"));
    FARLAND_TRY_VOID(read_octets_field(seq, 0));
    FARLAND_TRY(Reader csp_field, read_field(seq, 1));
    FARLAND_TRY(Reader csp, ber::read_constructed(csp_field, ber::tags::sequence, der));
    FARLAND_TRY_VOID(csp_field.expect_end("cspData"));
    FARLAND_TRY(Reader key_spec, read_field(csp, 0));
    FARLAND_TRY_VOID(ber::read_integer(key_spec, der));
    FARLAND_TRY_VOID(key_spec.expect_end("keySpec"));
    for (std::uint32_t number = 1; number <= 4; ++number) {
        if (next_is(csp, ber::context(number))) {
            FARLAND_TRY_VOID(read_octets_field(csp, number));
        }
    }
    FARLAND_TRY_VOID(csp.expect_end("TSCspDataDetail"));
    for (std::uint32_t number = 2; number <= 3; ++number) {
        if (next_is(seq, ber::context(number))) {
            FARLAND_TRY_VOID(read_octets_field(seq, number));
        }
    }
    FARLAND_TRY_VOID(seq.expect_end("TSSmartCardCreds"));
    return {};
}

// [MS-CSSP] 2.2.1.2.3.1 TSRemoteGuardPackageCred ::= SEQUENCE {
//   packageName [0] OCTET STRING, credBuffer [1] OCTET STRING }
Result<void> check_package_cred(Reader& r)
{
    FARLAND_TRY(Reader seq, ber::read_constructed(r, ber::tags::sequence, der));
    FARLAND_TRY_VOID(read_octets_field(seq, 0));
    FARLAND_TRY_VOID(read_octets_field(seq, 1));
    FARLAND_TRY_VOID(seq.expect_end("TSRemoteGuardPackageCred"));
    return {};
}

// [MS-CSSP] 2.2.1.2.3 TSRemoteGuardCreds ::= SEQUENCE {
//   logonCred [0] TSRemoteGuardPackageCred,
//   supplementalCreds [1] SEQUENCE OF TSRemoteGuardPackageCred OPTIONAL }
Result<void> check_remote_guard_creds(Reader& r)
{
    FARLAND_TRY(Reader seq, ber::read_constructed(r, ber::tags::sequence, der));
    FARLAND_TRY_VOID(r.expect_end("TSRemoteGuardCreds"));
    FARLAND_TRY(Reader logon, read_field(seq, 0));
    FARLAND_TRY_VOID(check_package_cred(logon));
    FARLAND_TRY_VOID(logon.expect_end("logonCred"));
    if (next_is(seq, ber::context(1))) {
        FARLAND_TRY(Reader field, read_field(seq, 1));
        FARLAND_TRY(Reader list, ber::read_constructed(field, ber::tags::sequence, der));
        FARLAND_TRY_VOID(field.expect_end("supplementalCreds"));
        for (std::size_t count = 0; !list.empty(); ++count) {
            if (count == max_supplemental_creds) {
                return fail(Errc::limit_exceeded, "too many supplementalCreds", list.offset());
            }
            FARLAND_TRY_VOID(check_package_cred(list));
        }
    }
    FARLAND_TRY_VOID(seq.expect_end("TSRemoteGuardCreds"));
    return {};
}

enum class CredType : std::uint8_t { password = 1, smart_card = 2, remote_guard = 6 };

}  // namespace

// TSRequest ---------------------------------------------------------------

// [MS-CSSP] 2.2.1 TSRequest ::= SEQUENCE {
//   version [0] INTEGER, negoTokens [1] NegoData OPTIONAL,
//   authInfo [2] OCTET STRING OPTIONAL, pubKeyAuth [3] OCTET STRING OPTIONAL,
//   errorCode [4] INTEGER OPTIONAL, clientNonce [5] OCTET STRING OPTIONAL }
// [MS-CSSP] 2.2.1.1 NegoData ::= SEQUENCE OF SEQUENCE { negoToken [0] OCTET STRING }
Bytes encode(const TsRequest& request)
{
    Writer w;
    ber::write_constructed(w, ber::tags::sequence, [&](Writer& seq) {
        write_field(seq, 0, [&](Writer& field) { ber::write_unsigned(field, request.version); });
        if (!request.nego_tokens.empty()) {
            write_field(seq, 1, [&](Writer& field) {
                ber::write_constructed(field, ber::tags::sequence, [&](Writer& list) {
                    for (const Bytes& token : request.nego_tokens) {
                        ber::write_constructed(list, ber::tags::sequence,
                                               [&](Writer& data) { write_octets_field(data, 0, token); });
                    }
                });
            });
        }
        if (request.auth_info) {
            write_octets_field(seq, 2, *request.auth_info);
        }
        if (request.pub_key_auth) {
            write_octets_field(seq, 3, *request.pub_key_auth);
        }
        if (request.error_code) {
            // As a signed 32-bit value, the way Windows and FreeRDP encode NTSTATUS.
            const auto code = static_cast<std::int32_t>(*request.error_code);
            write_field(seq, 4, [&](Writer& field) { ber::write_integer(field, code); });
        }
        if (request.client_nonce) {
            write_octets_field(seq, 5, *request.client_nonce);
        }
    });
    return std::move(w).take();
}

Result<TsRequest> decode_ts_request(std::span<const std::byte> bytes)
{
    if (bytes.size() > max_ts_request_size) {
        return fail(Errc::limit_exceeded, "TSRequest larger than 64 KiB");
    }
    Reader r(bytes);
    FARLAND_TRY(Reader seq, ber::read_constructed(r, ber::tags::sequence, der));
    FARLAND_TRY_VOID(r.expect_end("TSRequest"));
    TsRequest out;

    FARLAND_TRY(Reader version_field, read_field(seq, 0));
    const std::size_t version_offset = version_field.offset();
    FARLAND_TRY(const std::uint64_t version, ber::read_unsigned(version_field, der));
    FARLAND_TRY_VOID(version_field.expect_end("version"));
    if (version > std::numeric_limits<std::uint32_t>::max()) {
        return fail(Errc::invalid_value, "TSRequest version out of range", version_offset);
    }
    out.version = static_cast<std::uint32_t>(version);

    if (next_is(seq, ber::context(1))) {
        FARLAND_TRY(Reader field, read_field(seq, 1));
        FARLAND_TRY(Reader list, ber::read_constructed(field, ber::tags::sequence, der));
        FARLAND_TRY_VOID(field.expect_end("negoTokens"));
        while (!list.empty()) {
            if (out.nego_tokens.size() == max_nego_tokens) {
                return fail(Errc::limit_exceeded, "too many negoTokens", list.offset());
            }
            FARLAND_TRY(Reader data, ber::read_constructed(list, ber::tags::sequence, der));
            FARLAND_TRY(Bytes token, read_octets_field(data, 0));
            FARLAND_TRY_VOID(data.expect_end("NegoData"));
            out.nego_tokens.push_back(std::move(token));
        }
        if (out.nego_tokens.empty()) {
            return fail(Errc::invalid_length, "negoTokens is empty", list.offset());
        }
    }
    if (next_is(seq, ber::context(2))) {
        FARLAND_TRY(out.auth_info, read_octets_field(seq, 2));
    }
    if (next_is(seq, ber::context(3))) {
        FARLAND_TRY(out.pub_key_auth, read_octets_field(seq, 3));
    }
    if (next_is(seq, ber::context(4))) {
        FARLAND_TRY(Reader field, read_field(seq, 4));
        const std::size_t offset = field.offset();
        FARLAND_TRY(const std::int64_t code, ber::read_integer(field, der));
        FARLAND_TRY_VOID(field.expect_end("errorCode"));
        // Signed (02 04 c0 00 00 6d) or unsigned (02 05 00 c0 00 00 6d) 32-bit NTSTATUS.
        if (std::cmp_less(code, std::numeric_limits<std::int32_t>::min()) ||
            std::cmp_greater(code, std::numeric_limits<std::uint32_t>::max())) {
            return fail(Errc::invalid_value, "errorCode is not a 32-bit NTSTATUS", offset);
        }
        out.error_code = static_cast<std::uint32_t>(code);
    }
    if (next_is(seq, ber::context(5))) {
        const std::size_t offset = seq.offset();
        FARLAND_TRY(const Bytes nonce, read_octets_field(seq, 5));
        if (nonce.size() != nonce_size) {
            return fail(Errc::invalid_length, "clientNonce is not 32 bytes", offset);
        }
        Nonce value{};
        std::ranges::copy(nonce, value.begin());
        out.client_nonce = value;
    }
    FARLAND_TRY_VOID(seq.expect_end("TSRequest"));
    return out;
}

Result<std::optional<std::size_t>> frame_ts_request(std::span<const std::byte> stream)
{
    Reader r(stream);
    const auto tag = ber::read_tag(r, der);
    if (!tag.has_value()) {
        if (tag.error().code == Errc::truncated) {
            return std::nullopt;
        }
        return std::unexpected(tag.error());
    }
    if (*tag != ber::tags::sequence) {
        return fail(Errc::invalid_value, "TSRequest does not start with a SEQUENCE");
    }
    const auto length = ber::read_length(r, der);
    if (!length.has_value()) {
        if (length.error().code == Errc::truncated) {
            return std::nullopt;
        }
        return std::unexpected(length.error());
    }
    if (*length > max_ts_request_size - r.position()) {
        return fail(Errc::limit_exceeded, "TSRequest larger than 64 KiB");
    }
    return r.position() + *length;
}

// TSCredentials -----------------------------------------------------------

// [MS-CSSP] 2.2.1.2 TSCredentials ::= SEQUENCE { credType [0] INTEGER, credentials [1] OCTET STRING }
Bytes encode(const TsCredentials& credentials)
{
    Writer inner;
    CredType type = CredType::password;
    if (const auto* password = std::get_if<TsPasswordCreds>(&credentials.credentials)) {
        ber::write_constructed(inner, ber::tags::sequence, [&](Writer& seq) {
            write_octets_field(seq, 0, password->domain_name);
            write_octets_field(seq, 1, password->user_name);
            write_octets_field(seq, 2, password->password.view());
        });
    } else if (const auto* smart_card = std::get_if<TsSmartCardCreds>(&credentials.credentials)) {
        type = CredType::smart_card;
        inner.bytes(smart_card->der.view());
    } else {
        type = CredType::remote_guard;
        inner.bytes(std::get<TsRemoteGuardCreds>(credentials.credentials).der.view());
    }
    Writer w;
    ber::write_constructed(w, ber::tags::sequence, [&](Writer& seq) {
        write_field(seq, 0, [&](Writer& field) { ber::write_integer(field, static_cast<std::int64_t>(type)); });
        write_octets_field(seq, 1, inner.view());
    });
    auto scratch = std::move(inner).take();
    secure_zero(scratch);
    return std::move(w).take();
}

Result<TsCredentials> decode_ts_credentials(std::span<const std::byte> bytes)
{
    Reader r(bytes);
    FARLAND_TRY(Reader seq, ber::read_constructed(r, ber::tags::sequence, der));
    FARLAND_TRY_VOID(r.expect_end("TSCredentials"));
    FARLAND_TRY(Reader type_field, read_field(seq, 0));
    const std::size_t type_offset = type_field.offset();
    FARLAND_TRY(const std::int64_t type, ber::read_integer(type_field, der));
    FARLAND_TRY_VOID(type_field.expect_end("credType"));
    FARLAND_TRY(Reader creds_field, read_field(seq, 1));
    FARLAND_TRY(const ber::Tlv creds, ber::expect_tlv(creds_field, ber::tags::octet_string, der));
    FARLAND_TRY_VOID(creds_field.expect_end("credentials"));
    FARLAND_TRY_VOID(seq.expect_end("TSCredentials"));

    Reader contents = creds.reader();
    switch (type) {
    case static_cast<std::int64_t>(CredType::password): {
        FARLAND_TRY(TsPasswordCreds password, decode_password_creds(contents));
        return TsCredentials{std::move(password)};
    }
    case static_cast<std::int64_t>(CredType::smart_card):
        FARLAND_TRY_VOID(check_smart_card_creds(contents));
        return TsCredentials{TsSmartCardCreds{SecretBytes(creds.value)}};
    case static_cast<std::int64_t>(CredType::remote_guard):
        FARLAND_TRY_VOID(check_remote_guard_creds(contents));
        return TsCredentials{TsRemoteGuardCreds{SecretBytes(creds.value)}};
    default:
        return fail(Errc::unsupported, "unknown TSCredentials credType", type_offset);
    }
}

TsCredentials to_ts_credentials(const PasswordCredentials& credentials)
{
    TsPasswordCreds creds;
    creds.domain_name = utf8_to_utf16le(credentials.domain);
    creds.user_name = utf8_to_utf16le(credentials.user);
    creds.password = SecretBytes(utf8_to_utf16le(credentials.password.view()));
    return TsCredentials{std::move(creds)};
}

PasswordCredentials to_password_credentials(const TsPasswordCreds& credentials)
{
    PasswordCredentials out;
    out.domain = utf16le_to_utf8(credentials.domain_name);
    out.user = utf16le_to_utf8(credentials.user_name);
    out.password = SecretString(utf16le_to_utf8(credentials.password.view()));
    return out;
}

Hash client_to_server_hash(const Nonce& nonce, std::span<const std::byte> public_key)
{
    return binding_hash(client_server_magic, nonce, public_key);
}

Hash server_to_client_hash(const Nonce& nonce, std::span<const std::byte> public_key)
{
    return binding_hash(server_client_magic, nonce, public_key);
}

// Acceptor ([MS-CSSP] 3.1.5) ---------------------------------------------------

Acceptor::Acceptor(AcceptorConfig config)
    : config_(std::move(config)), public_key_(config_.server_public_key.begin(), config_.server_public_key.end())
{
    config_.server_public_key = {};
    FARLAND_ASSERT(!public_key_.empty());
    FARLAND_ASSERT(static_cast<bool>(config_.make_mechanism));
    FARLAND_ASSERT(config_.max_version >= min_version);
}

void Acceptor::receive(std::span<const std::byte> bytes)
{
    if (status_ == Status::succeeded) {
        remaining_.insert(remaining_.end(), bytes.begin(), bytes.end());
        return;
    }
    if (status_ == Status::failed) {
        return;
    }
    input_.insert(input_.end(), bytes.begin(), bytes.end());
    while (status_ == Status::in_progress) {
        const auto frame = frame_ts_request(input_);
        if (!frame.has_value()) {
            fail(describe("malformed TSRequest", frame.error()));
            return;
        }
        if (!frame->has_value() || input_.size() < **frame) {
            return;
        }
        const std::span<const std::byte> buffered(input_);
        auto request = decode_ts_request(buffered.first(**frame));
        const auto rest = buffered.subspan(**frame);
        input_ = Bytes(rest.begin(), rest.end());
        if (!request.has_value()) {
            fail(describe("malformed TSRequest", request.error()));
            return;
        }
        if (auto outcome = handle(*request); !outcome.has_value()) {
            fail(std::move(outcome).error());
            return;
        }
    }
    if (status_ == Status::succeeded) {
        remaining_ = std::exchange(input_, {});
    }
}

std::vector<std::byte> Acceptor::take_output()
{
    return std::exchange(output_, {});
}

std::vector<std::byte> Acceptor::take_remaining_input()
{
    return std::exchange(remaining_, {});
}

std::optional<PasswordCredentials> Acceptor::take_credentials()
{
    return std::exchange(credentials_, std::nullopt);
}

void Acceptor::send(TsRequest message)
{
    message.version = version_;
    const Bytes bytes = encode(message);
    output_.insert(output_.end(), bytes.begin(), bytes.end());
}

void Acceptor::fail(std::string reason)
{
    status_ = Status::failed;
    phase_ = Phase::done;
    failure_ = std::move(reason);
    input_.clear();
    credentials_.reset();
    // [MS-CSSP] 2.2.1 errorCode: sent when the negotiated version is 3, 4 or
    // 6 (FreeRDP skips 5 as well). Always STATUS_LOGON_FAILURE, so a client
    // learns nothing about which step failed.
    if (version_ == 3 || version_ == 4 || version_ == 6) {
        TsRequest message;
        message.error_code = status_logon_failure;
        send(std::move(message));
    }
}

Acceptor::Outcome Acceptor::handle(TsRequest& request)
{
    // [MS-CSSP] 2.2.1 version: a higher version than ours is treated as ours.
    if (peer_version_ == 0) {
        if (request.version < min_version) {
            return reject(std::format("client offers CredSSP version {}, below 2", request.version));
        }
        peer_version_ = request.version;
        version_ = std::min(request.version, config_.max_version);
    } else if (request.version != peer_version_) {
        // As FreeRDP: a peer that changes its version mid-handshake is broken or tampering.
        return reject(std::format("client changed its CredSSP version from {} to {}", peer_version_, request.version));
    }
    if (request.error_code) {
        return reject(std::format("client sent errorCode 0x{:08X}", *request.error_code));
    }
    if (request.client_nonce) {
        nonce_ = request.client_nonce;
    }
    switch (phase_) {
    case Phase::negotiate:
        return handle_negotiate(request);
    case Phase::pub_key_auth:
        return handle_pub_key_auth(request);
    case Phase::credentials:
        return handle_credentials(request);
    case Phase::done:
        break;
    }
    return reject("TSRequest after the handshake ended");
}

// [MS-CSSP] 3.1.5 step 2: the SPNEGO, Kerberos or NTLM exchange in negoTokens.
Acceptor::Outcome Acceptor::handle_negotiate(TsRequest& request)
{
    if (request.nego_tokens.size() != 1) {
        return reject("TSRequest during negotiation does not carry exactly one negoToken");
    }
    if (request.auth_info) {
        return reject("client sent authInfo before public key authentication");
    }
    const Bytes& token = request.nego_tokens.front();

    if (!mech_) {
        if (!spnego::is_raw_ntlm(token)) {
            auto decoded = spnego::decode(token);
            if (!decoded.has_value()) {
                return reject(describe("malformed SPNEGO token", decoded.error()));
            }
            const auto* init = std::get_if<spnego::NegTokenInit>(&*decoded);
            if (init == nullptr) {
                return reject("first SPNEGO token is not a NegTokenInit");
            }
            spnego_ = true;
            return start_spnego(*init, request);
        }
        // Raw NTLM, as FreeRDP and other clients without Kerberos send it.
        mech_ = config_.make_mechanism(spnego::ntlm_oid);
        if (!mech_) {
            return reject("client sent a raw NTLM token, but NTLM is not available");
        }
    }

    std::span<const std::byte> mech_input = token;
    std::optional<spnego::NegTokenResp> resp;
    if (spnego_) {
        auto decoded = spnego::decode(token);
        if (!decoded.has_value()) {
            return reject(describe("malformed SPNEGO token", decoded.error()));
        }
        auto* next = std::get_if<spnego::NegTokenResp>(&*decoded);
        if (next == nullptr) {
            return reject("client sent a second NegTokenInit");
        }
        resp = std::move(*next);
        if (resp->neg_state == spnego::NegState::reject) {
            return reject("client rejected the SPNEGO negotiation");
        }
        if (!resp->response_token) {
            return reject("NegTokenResp during negotiation lacks a responseToken");
        }
        mech_input = *resp->response_token;
    }

    auto step = mech_->step(mech_input);
    if (!step.has_value()) {
        return reject(describe("security mechanism rejected the client", step.error()));
    }
    if (step->complete) {
        const std::optional<Bytes> no_mic;
        return finish_negotiation(request, std::move(step->token), resp ? resp->mech_list_mic : no_mic, {});
    }
    TsRequest message;
    if (spnego_) {
        spnego::NegTokenResp reply;
        reply.neg_state = spnego::NegState::accept_incomplete;
        reply.response_token = std::move(step->token);
        message.nego_tokens.push_back(spnego::encode(reply));
    } else {
        message.nego_tokens.push_back(std::move(step->token));
    }
    send(std::move(message));
    return {};
}

// RFC 4178 3.2 and [MS-SPNG] 3.2.5.2: the acceptor's first reply.
Acceptor::Outcome Acceptor::start_spnego(const spnego::NegTokenInit& init, const TsRequest& request)
{
    mech_types_der_ = init.mech_types_der;
    // The first mechanism, in the client's order of preference, that farland
    // implements. Windows lists Kerberos (twice) and NEGOEX before NTLM.
    std::size_t chosen = 0;
    for (; chosen < init.mech_types.size(); ++chosen) {
        mech_ = config_.make_mechanism(init.mech_types[chosen]);
        if (mech_) {
            break;
        }
    }
    if (!mech_) {
        return reject("client offers no security mechanism that farland supports");
    }

    spnego::NegTokenResp reply;
    reply.supported_mech = init.mech_types[chosen];
    if (chosen != 0) {
        // RFC 4178 5: a mechanism other than the initiator's first choice
        // makes the mechListMIC exchange mandatory, signalled by request-mic.
        // An optimistic mechToken belongs to the first choice; drop it and
        // let the client start the selected mechanism with its next token.
        mic_required_ = true;
        reply.neg_state = spnego::NegState::request_mic;
        TsRequest message;
        message.nego_tokens.push_back(spnego::encode(reply));
        send(std::move(message));
        return {};
    }

    reply.neg_state = spnego::NegState::accept_incomplete;
    if (init.mech_token) {
        auto step = mech_->step(*init.mech_token);
        if (!step.has_value()) {
            return reject(describe("security mechanism rejected the client", step.error()));
        }
        if (step->complete) {
            return finish_negotiation(request, std::move(step->token), init.mech_list_mic, std::move(reply));
        }
        reply.response_token = std::move(step->token);
    }
    TsRequest message;
    message.nego_tokens.push_back(spnego::encode(reply));
    send(std::move(message));
    return {};
}

// The mechanism is established. Order of mechanism calls from here on:
// verify_mic (client's mechListMIC), get_mic (ours), unwrap (client's
// pubKeyAuth), wrap (our pubKeyAuth), unwrap (authInfo). Per direction this
// is MIC before the first wrap, as [MS-SPNG] 3.3.5.1 and FreeRDP's
// negotiate_mic_exchange expect.
Acceptor::Outcome Acceptor::finish_negotiation(const TsRequest& request, Bytes mech_output,
                                               const std::optional<Bytes>& client_mic, spnego::NegTokenResp reply)
{
    std::optional<Bytes> nego_token;
    if (spnego_) {
        // RFC 4178 5: verify the initiator's MIC, then send ours.
        if (client_mic) {
            if (auto verified = mech_->verify_mic(mech_types_der_, *client_mic); !verified.has_value()) {
                return reject(describe("client mechListMIC does not verify", verified.error()));
            }
            client_mic_verified_ = true;
        } else if (mic_required_ && mech_output.empty()) {
            return reject("client omitted the mechListMIC that RFC 4178 requires here");
        }
        if (mic_required_ || client_mic) {
            reply.mech_list_mic = mech_->get_mic(mech_types_der_);
        }
        if (reply.mech_list_mic || client_mic_verified_) {
            // [MS-SPNG] 3.3.5.1: the first wrapped message starts from the
            // cipher state the mechListMIC started from. FreeRDP calls
            // ntlm_reset_cipher_state here, after both MICs and before any wrap.
            mech_->reset_cipher_state();
        }
        reply.neg_state = !mic_required_ || client_mic_verified_ ? spnego::NegState::accept_completed
                                                                 : spnego::NegState::accept_incomplete;
        if (!mech_output.empty()) {
            reply.response_token = std::move(mech_output);
        }
        nego_token = spnego::encode(reply);
    } else if (!mech_output.empty()) {
        nego_token = std::move(mech_output);
    }

    phase_ = Phase::pub_key_auth;
    if (request.pub_key_auth) {
        // [MS-CSSP] 3.1.5 step 3: the client's last token comes with pubKeyAuth.
        return answer_pub_key_auth(*request.pub_key_auth, std::move(nego_token));
    }
    // FreeRDP's SPNEGO initiator waits for our final token before sending pubKeyAuth.
    if (nego_token) {
        TsRequest message;
        message.nego_tokens.push_back(std::move(*nego_token));
        send(std::move(message));
    }
    return {};
}

Acceptor::Outcome Acceptor::handle_pub_key_auth(const TsRequest& request)
{
    if (request.auth_info) {
        return reject("client sent authInfo before public key authentication");
    }
    if (!request.nego_tokens.empty()) {
        if (!spnego_ || request.nego_tokens.size() != 1) {
            return reject("unexpected negoToken after the security mechanism completed");
        }
        auto decoded = spnego::decode(request.nego_tokens.front());
        if (!decoded.has_value()) {
            return reject(describe("malformed SPNEGO token", decoded.error()));
        }
        const auto* resp = std::get_if<spnego::NegTokenResp>(&*decoded);
        if (resp == nullptr || resp->neg_state == spnego::NegState::reject ||
            (resp->response_token && !resp->response_token->empty())) {
            return reject("unexpected SPNEGO token after the security mechanism completed");
        }
        if (resp->mech_list_mic && !client_mic_verified_) {
            if (auto verified = mech_->verify_mic(mech_types_der_, *resp->mech_list_mic); !verified.has_value()) {
                return reject(describe("client mechListMIC does not verify", verified.error()));
            }
            client_mic_verified_ = true;
            mech_->reset_cipher_state();  // [MS-SPNG] 3.3.5.1; nothing has been wrapped yet
        }
    }
    if (mic_required_ && !client_mic_verified_) {
        return reject("client omitted the mechListMIC that RFC 4178 requires here");
    }
    if (!request.pub_key_auth) {
        return reject("expected pubKeyAuth");
    }
    return answer_pub_key_auth(*request.pub_key_auth, std::nullopt);
}

// [MS-CSSP] 3.1.5 steps 3 and 4: check the client's binding to our TLS key, answer with ours.
Acceptor::Outcome Acceptor::answer_pub_key_auth(std::span<const std::byte> pub_key_auth,
                                                std::optional<Bytes> nego_token)
{
    auto plaintext = mech_->unwrap(pub_key_auth);
    if (!plaintext.has_value()) {
        return reject(describe("pubKeyAuth does not unwrap", plaintext.error()));
    }
    Bytes expected;
    Bytes answer;
    if (version_ >= 5) {
        if (!nonce_) {
            return reject("CredSSP 5+ client sent pubKeyAuth without a clientNonce");
        }
        const Hash client_hash = client_to_server_hash(*nonce_, public_key_);
        const Hash server_hash = server_to_client_hash(*nonce_, public_key_);
        expected.assign(client_hash.begin(), client_hash.end());
        answer.assign(server_hash.begin(), server_hash.end());
    } else {
        expected = public_key_;
        answer = public_key_plus_one(public_key_);
    }
    if (!equal_constant_time(*plaintext, expected)) {
        return reject("pubKeyAuth does not match the TLS public key (man in the middle, or a client binding "
                      "to another key)");
    }

    TsRequest message;
    if (nego_token) {
        message.nego_tokens.push_back(std::move(*nego_token));
    }
    message.pub_key_auth = mech_->wrap(answer);
    phase_ = Phase::credentials;
    send(std::move(message));
    return {};
}

// [MS-CSSP] 3.1.5 step 5: the delegated credentials.
Acceptor::Outcome Acceptor::handle_credentials(const TsRequest& request)
{
    if (!request.auth_info) {
        return reject("expected authInfo with the delegated credentials");
    }
    auto plaintext = mech_->unwrap(*request.auth_info);
    if (!plaintext.has_value()) {
        return reject(describe("authInfo does not unwrap", plaintext.error()));
    }
    auto credentials = decode_ts_credentials(*plaintext);
    secure_zero(*plaintext);
    if (!credentials.has_value()) {
        return reject(describe("malformed TSCredentials", credentials.error()));
    }
    if (std::holds_alternative<TsSmartCardCreds>(credentials->credentials)) {
        return reject("client delegated smart-card credentials (TSSmartCardCreds), which farland does not support");
    }
    if (std::holds_alternative<TsRemoteGuardCreds>(credentials->credentials)) {
        return reject("client delegated Remote Credential Guard credentials (TSRemoteGuardCreds), which farland "
                      "does not support");
    }
    PasswordCredentials delegated = to_password_credentials(std::get<TsPasswordCreds>(credentials->credentials));
    if (config_.accept_credentials && !config_.accept_credentials(delegated, mech_->identity())) {
        return reject("delegated credentials rejected: they do not belong to the authenticated user");
    }
    credentials_ = std::move(delegated);
    identity_ = mech_->identity();
    phase_ = Phase::done;
    status_ = Status::succeeded;
    return {};
}

// Initiator ([MS-CSSP] 3.1.5, client side) ------------------------------------

Initiator::Initiator(InitiatorConfig config)
    : config_(std::move(config)), nonce_(config_.client_nonce ? *config_.client_nonce : random_nonce()),
      version_(config_.version)
{
    FARLAND_ASSERT(config_.mechanism != nullptr);
    FARLAND_ASSERT(!config_.server_public_key.empty());
    FARLAND_ASSERT(config_.version >= min_version);
    if (auto outcome = start(); !outcome.has_value()) {
        fail(std::move(outcome).error());
    }
}

void Initiator::receive(std::span<const std::byte> bytes)
{
    if (status_ == Status::succeeded) {
        remaining_.insert(remaining_.end(), bytes.begin(), bytes.end());
        return;
    }
    if (status_ == Status::failed) {
        return;
    }
    input_.insert(input_.end(), bytes.begin(), bytes.end());
    while (status_ == Status::in_progress) {
        const auto frame = frame_ts_request(input_);
        if (!frame.has_value()) {
            fail(describe("malformed TSRequest", frame.error()));
            return;
        }
        if (!frame->has_value() || input_.size() < **frame) {
            return;
        }
        const std::span<const std::byte> buffered(input_);
        auto request = decode_ts_request(buffered.first(**frame));
        const auto rest = buffered.subspan(**frame);
        input_ = Bytes(rest.begin(), rest.end());
        if (!request.has_value()) {
            fail(describe("malformed TSRequest", request.error()));
            return;
        }
        if (auto outcome = handle(*request); !outcome.has_value()) {
            fail(std::move(outcome).error());
            return;
        }
    }
    if (status_ == Status::succeeded) {
        remaining_ = std::exchange(input_, {});
    }
}

std::vector<std::byte> Initiator::take_output()
{
    return std::exchange(output_, {});
}

std::vector<std::byte> Initiator::take_remaining_input()
{
    return std::exchange(remaining_, {});
}

void Initiator::send(TsRequest message)
{
    message.version = config_.version;
    if (version_ >= 5) {
        message.client_nonce = nonce_;  // in every TSRequest, as FreeRDP does
    }
    const Bytes bytes = encode(message);
    output_.insert(output_.end(), bytes.begin(), bytes.end());
}

void Initiator::fail(std::string reason)
{
    status_ = Status::failed;
    phase_ = Phase::done;
    failure_ = std::move(reason);
    input_.clear();
}

Initiator::Outcome Initiator::start()
{
    SecurityContext& mech = *config_.mechanism;
    if (!config_.use_spnego) {
        auto step = mech.step({});
        if (!step.has_value()) {
            return reject(describe("security mechanism failed", step.error()));
        }
        return send_step(std::move(*step));
    }

    std::vector<spnego::Oid> mech_types = config_.preferred_mech_types;
    const auto own = mech.mechanism();
    mech_types.emplace_back(own.begin(), own.end());
    // RFC 4178 5: our mechanism is not the first choice, so the MICs are mandatory.
    mic_required_ = mech_types.size() > 1;
    std::optional<Bytes> optimistic;
    if (config_.preferred_mech_types.empty()) {
        auto step = mech.step({});
        if (!step.has_value()) {
            return reject(describe("security mechanism failed", step.error()));
        }
        if (step->complete) {
            return reject("security mechanism completed without a round trip, which SPNEGO here does not support");
        }
        optimistic = std::move(step->token);
    } else {
        pending_first_step_ = true;
    }
    auto init = spnego::make_neg_token_init(std::move(mech_types), std::move(optimistic));
    mech_types_der_ = init.mech_types_der;
    TsRequest message;
    message.nego_tokens.push_back(spnego::encode(init));
    send(std::move(message));
    return {};
}

Initiator::Outcome Initiator::handle(TsRequest& request)
{
    // [MS-CSSP] 3.1.5 step 2: an errorCode ends the handshake at once.
    if (request.error_code) {
        server_error_ = request.error_code;
        return reject(std::format("server reported NTSTATUS 0x{:08X}", *request.error_code));
    }
    if (server_version_ == 0) {
        if (request.version < min_version) {
            return reject(std::format("server offers CredSSP version {}, below 2", request.version));
        }
        server_version_ = request.version;
        version_ = std::min(config_.version, request.version);
    }
    switch (phase_) {
    case Phase::negotiate:
        return handle_negotiate(request);
    case Phase::final_token:
        return handle_final_token(request);
    case Phase::pub_key_auth:
        return handle_pub_key_auth(request);
    case Phase::done:
        break;
    }
    return reject("TSRequest after the handshake ended");
}

Initiator::Outcome Initiator::handle_negotiate(const TsRequest& request)
{
    if (request.nego_tokens.size() != 1) {
        return reject("server reply during negotiation does not carry exactly one negoToken");
    }
    SecurityContext& mech = *config_.mechanism;
    std::span<const std::byte> mech_input = request.nego_tokens.front();
    std::optional<spnego::NegTokenResp> resp;
    if (config_.use_spnego) {
        auto decoded = spnego::decode(request.nego_tokens.front());
        if (!decoded.has_value()) {
            return reject(describe("malformed SPNEGO token", decoded.error()));
        }
        auto* next = std::get_if<spnego::NegTokenResp>(&*decoded);
        if (next == nullptr) {
            return reject("server sent a NegTokenInit");
        }
        resp = std::move(*next);
        if (resp->neg_state == spnego::NegState::reject) {
            return reject("server rejected the SPNEGO negotiation");
        }
        if (first_reply_) {
            // RFC 4178 4.2.2: negState is required in the first reply, and
            // supportedMech names the mechanism the acceptor selected.
            first_reply_ = false;
            if (!resp->neg_state) {
                return reject("first NegTokenResp lacks negState");
            }
            if (resp->supported_mech) {
                if (!std::ranges::equal(*resp->supported_mech, mech.mechanism())) {
                    return reject("server selected a mechanism the initiator cannot run");
                }
            } else if (pending_first_step_) {
                return reject("server did not select a mechanism");
            }
            if (resp->neg_state == spnego::NegState::request_mic) {
                mic_required_ = true;
            }
        }
        if (pending_first_step_) {
            // The server selected our mechanism; start it now (RFC 4178 3.2).
            pending_first_step_ = false;
            if (resp->response_token && !resp->response_token->empty()) {
                return reject("server sent a responseToken before the mechanism started");
            }
            mech_input = {};
        } else if (resp->response_token) {
            mech_input = *resp->response_token;
        } else {
            return reject("NegTokenResp during negotiation lacks a responseToken");
        }
    }

    auto step = mech.step(mech_input);
    if (!step.has_value()) {
        return reject(describe("security mechanism failed", step.error()));
    }
    if (step->complete && resp && resp->mech_list_mic) {
        // The acceptor finished first (Kerberos with mutual authentication) and sent its MIC along.
        if (auto verified = mech.verify_mic(mech_types_der_, *resp->mech_list_mic); !verified.has_value()) {
            return reject(describe("server mechListMIC does not verify", verified.error()));
        }
        server_mic_verified_ = true;
    }
    return send_step(std::move(*step));
}

// [MS-CSSP] 3.1.5 steps 2 and 3. Once the mechanism is established, its last
// token goes out with pubKeyAuth in one TSRequest, as step 3 asks.
//
// With a mechListMIC exchange (RFC 4178 5) the last token carries our MIC
// instead, and pubKeyAuth waits for the acceptor's MIC: reset_cipher_state()
// restarts both directions, so it has to come after both MICs and before the
// first wrap. FreeRDP's SPNEGO initiator orders the messages the same way.
// Mechanism calls: get_mic, reset, then (handle_final_token) verify_mic,
// reset, wrap.
Initiator::Outcome Initiator::send_step(Step step)
{
    SecurityContext& mech = *config_.mechanism;
    TsRequest message;
    if (!step.complete) {
        if (config_.use_spnego) {
            spnego::NegTokenResp resp;
            resp.response_token = std::move(step.token);
            message.nego_tokens.push_back(spnego::encode(resp));
        } else {
            message.nego_tokens.push_back(std::move(step.token));
        }
        send(std::move(message));
        return {};
    }

    if (!config_.use_spnego) {
        if (!step.token.empty()) {
            message.nego_tokens.push_back(std::move(step.token));
        }
        send_pub_key_auth(std::move(message));
        return {};
    }

    spnego::NegTokenResp resp;
    if (!step.token.empty()) {
        resp.response_token = std::move(step.token);
    }
    if (mic_required_) {
        resp.mech_list_mic = mech.get_mic(mech_types_der_);
    }
    if (resp.mech_list_mic || server_mic_verified_) {
        mech.reset_cipher_state();
    }
    if (resp.response_token || resp.mech_list_mic) {
        message.nego_tokens.push_back(spnego::encode(resp));
    }
    if (mic_required_ && !server_mic_verified_) {
        phase_ = Phase::final_token;
        send(std::move(message));
        return {};
    }
    send_pub_key_auth(std::move(message));
    return {};
}

// RFC 4178 5: the acceptor's final token and its mechListMIC.
Initiator::Outcome Initiator::handle_final_token(const TsRequest& request)
{
    if (request.pub_key_auth || request.nego_tokens.size() != 1) {
        return reject("expected the acceptor's final SPNEGO token");
    }
    auto decoded = spnego::decode(request.nego_tokens.front());
    if (!decoded.has_value()) {
        return reject(describe("malformed SPNEGO token", decoded.error()));
    }
    const auto* resp = std::get_if<spnego::NegTokenResp>(&*decoded);
    if (resp == nullptr || resp->neg_state != spnego::NegState::accept_completed ||
        (resp->response_token && !resp->response_token->empty())) {
        return reject("server did not complete the SPNEGO negotiation");
    }
    if (!resp->mech_list_mic) {
        return reject("server omitted the mechListMIC that RFC 4178 requires here");
    }
    SecurityContext& mech = *config_.mechanism;
    if (auto verified = mech.verify_mic(mech_types_der_, *resp->mech_list_mic); !verified.has_value()) {
        return reject(describe("server mechListMIC does not verify", verified.error()));
    }
    server_mic_verified_ = true;
    mech.reset_cipher_state();
    send_pub_key_auth({});
    return {};
}

// [MS-CSSP] 3.1.5 step 3: bind to the server's TLS public key.
void Initiator::send_pub_key_auth(TsRequest message)
{
    SecurityContext& mech = *config_.mechanism;
    if (version_ >= 5) {
        const Hash hash = client_to_server_hash(nonce_, config_.server_public_key);
        message.pub_key_auth = mech.wrap(hash);
    } else {
        message.pub_key_auth = mech.wrap(config_.server_public_key);
    }
    phase_ = Phase::pub_key_auth;
    send(std::move(message));
}

// [MS-CSSP] 3.1.5 steps 4 and 5: check the server's pubKeyAuth, then delegate
// the credentials. Mechanism calls: verify_mic, unwrap, wrap.
Initiator::Outcome Initiator::handle_pub_key_auth(const TsRequest& request)
{
    SecurityContext& mech = *config_.mechanism;
    if (!request.nego_tokens.empty()) {
        if (!config_.use_spnego || request.nego_tokens.size() != 1) {
            return reject("unexpected negoToken after the security mechanism completed");
        }
        auto decoded = spnego::decode(request.nego_tokens.front());
        if (!decoded.has_value()) {
            return reject(describe("malformed SPNEGO token", decoded.error()));
        }
        const auto* resp = std::get_if<spnego::NegTokenResp>(&*decoded);
        if (resp == nullptr || (resp->neg_state && *resp->neg_state != spnego::NegState::accept_completed) ||
            (resp->response_token && !resp->response_token->empty())) {
            return reject("server did not complete the SPNEGO negotiation");
        }
        if (resp->mech_list_mic && !server_mic_verified_) {
            if (auto verified = mech.verify_mic(mech_types_der_, *resp->mech_list_mic); !verified.has_value()) {
                return reject(describe("server mechListMIC does not verify", verified.error()));
            }
            server_mic_verified_ = true;
        }
    }
    if (!request.pub_key_auth) {
        if (!request.nego_tokens.empty()) {
            return {};  // the final SPNEGO token on its own; pubKeyAuth follows
        }
        return reject("server reply lacks pubKeyAuth");
    }
    if (mic_required_ && !server_mic_verified_) {
        return reject("server omitted the mechListMIC that RFC 4178 requires here");
    }

    auto plaintext = mech.unwrap(*request.pub_key_auth);
    if (!plaintext.has_value()) {
        return reject(describe("server pubKeyAuth does not unwrap", plaintext.error()));
    }
    Bytes expected;
    if (version_ >= 5) {
        const Hash hash = server_to_client_hash(nonce_, config_.server_public_key);
        expected.assign(hash.begin(), hash.end());
    } else {
        expected = public_key_plus_one(config_.server_public_key);
    }
    if (!equal_constant_time(*plaintext, expected)) {
        return reject("server pubKeyAuth does not match its TLS public key (possible man in the middle)");
    }

    Bytes credentials = encode(to_ts_credentials(config_.credentials));
    TsRequest message;
    message.auth_info = mech.wrap(credentials);
    secure_zero(credentials);
    phase_ = Phase::done;
    status_ = Status::succeeded;
    send(std::move(message));
    return {};
}

}  // namespace farland::auth::credssp
