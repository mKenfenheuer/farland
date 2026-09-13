// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/auth/gss.hpp>
#include <farland/auth/spnego.hpp>
#include <farland/base/error.hpp>
#include <farland/base/text.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

/// CredSSP ([MS-CSSP]), the Network Level Authentication handshake: TSRequest
/// and TSCredentials codecs, and the acceptor (server) and initiator (client)
/// as sans-IO state machines over a SecurityContext, negotiated either
/// through SPNEGO (spnego.hpp) or with raw NTLM tokens.
namespace farland::auth::credssp {

using Bytes = std::vector<std::byte>;

/// Largest TSRequest accepted, including its tag and length.
inline constexpr std::size_t max_ts_request_size = std::size_t{64} * 1024;
/// Largest negoToken, authInfo or pubKeyAuth accepted.
inline constexpr std::size_t max_field_size = spnego::max_token_size;
/// Most NegoData entries accepted in one negoTokens field. Peers send one.
inline constexpr std::size_t max_nego_tokens = 4;
/// Most supplementalCreds accepted in TSRemoteGuardCreds.
inline constexpr std::size_t max_supplemental_creds = 16;
/// clientNonce length ([MS-CSSP] 2.2.1).
inline constexpr std::size_t nonce_size = 32;
inline constexpr std::uint32_t min_version = 2;
inline constexpr std::uint32_t max_version = 6;
/// NTSTATUS STATUS_LOGON_FAILURE ([MS-ERREF] 2.3.1).
inline constexpr std::uint32_t status_logon_failure = 0xC000006D;

using Nonce = std::array<std::byte, nonce_size>;
using Hash = std::array<std::byte, 32>;

// Codecs ------------------------------------------------------------------

/// [MS-CSSP] 2.2.1 TSRequest. `nego_tokens` is NegoData (2.2.1.1): absent
/// when empty.
struct TsRequest {
    std::uint32_t version = max_version;
    std::vector<Bytes> nego_tokens;
    std::optional<Bytes> auth_info;
    std::optional<Bytes> pub_key_auth;
    /// An NTSTATUS. Encoded as a signed 32-bit INTEGER like Windows and
    /// FreeRDP do (0xC000006D is 02 04 c0 00 00 6d); the 5-byte form with a
    /// leading zero octet decodes too.
    std::optional<std::uint32_t> error_code;
    std::optional<Nonce> client_nonce;

    friend bool operator==(const TsRequest&, const TsRequest&) = default;
};

[[nodiscard]] Bytes encode(const TsRequest& request);
/// Decodes exactly one TSRequest (DER) that fills `bytes`.
[[nodiscard]] Result<TsRequest> decode_ts_request(std::span<const std::byte> bytes);
/// The size of the TSRequest at the start of `stream`, or nullopt while its
/// tag and length are incomplete. Fails on anything but a SEQUENCE and on
/// sizes above `max_ts_request_size`.
[[nodiscard]] Result<std::optional<std::size_t>> frame_ts_request(std::span<const std::byte> stream);

/// A byte buffer that wipes itself, for UTF-16LE passwords, PINs and tickets.
class SecretBytes {
public:
    SecretBytes() = default;
    explicit SecretBytes(Bytes bytes) noexcept : bytes_(std::move(bytes)) {}
    explicit SecretBytes(std::span<const std::byte> bytes) : bytes_(bytes.begin(), bytes.end()) {}
    SecretBytes(const SecretBytes& other) = default;
    SecretBytes(SecretBytes&& other) noexcept : bytes_(std::move(other.bytes_)) { other.wipe(); }
    SecretBytes& operator=(const SecretBytes& other)
    {
        if (this != &other) {
            wipe();
            bytes_ = other.bytes_;
        }
        return *this;
    }
    SecretBytes& operator=(SecretBytes&& other) noexcept
    {
        if (this != &other) {
            wipe();
            bytes_ = std::move(other.bytes_);
            other.wipe();
        }
        return *this;
    }
    ~SecretBytes() { wipe(); }

    [[nodiscard]] std::span<const std::byte> view() const noexcept { return bytes_; }

    friend bool operator==(const SecretBytes& a, const SecretBytes& b)
    {
        return std::ranges::equal(a.bytes_, b.bytes_);
    }

private:
    void wipe() noexcept
    {
        secure_zero(bytes_);
        bytes_.clear();
    }

    Bytes bytes_;
};

/// [MS-CSSP] 2.2.1.2.1 TSPasswordCreds, fields as UTF-16LE on the wire.
struct TsPasswordCreds {
    Bytes domain_name;
    Bytes user_name;
    SecretBytes password;

    friend bool operator==(const TsPasswordCreds&, const TsPasswordCreds&) = default;
};

/// [MS-CSSP] 2.2.1.2.2 TSSmartCardCreds: checked, then kept as its DER.
struct TsSmartCardCreds {
    SecretBytes der;

    friend bool operator==(const TsSmartCardCreds&, const TsSmartCardCreds&) = default;
};

/// [MS-CSSP] 2.2.1.2.3 TSRemoteGuardCreds: checked, then kept as its DER.
struct TsRemoteGuardCreds {
    SecretBytes der;

    friend bool operator==(const TsRemoteGuardCreds&, const TsRemoteGuardCreds&) = default;
};

/// [MS-CSSP] 2.2.1.2 TSCredentials. credType is 1, 2 or 6 by alternative.
struct TsCredentials {
    std::variant<TsPasswordCreds, TsSmartCardCreds, TsRemoteGuardCreds> credentials;

    friend bool operator==(const TsCredentials&, const TsCredentials&) = default;
};

/// The encoding holds the secret; wipe it (secure_zero) after use.
[[nodiscard]] Bytes encode(const TsCredentials& credentials);
[[nodiscard]] Result<TsCredentials> decode_ts_credentials(std::span<const std::byte> bytes);

[[nodiscard]] TsCredentials to_ts_credentials(const PasswordCredentials& credentials);
[[nodiscard]] PasswordCredentials to_password_credentials(const TsPasswordCreds& credentials);

/// [MS-CSSP] 3.1.5 step 3, v5+: SHA-256("CredSSP Client-To-Server Binding Hash\0" || nonce || key).
[[nodiscard]] Hash client_to_server_hash(const Nonce& nonce, std::span<const std::byte> public_key);
/// [MS-CSSP] 3.1.5 step 4, v5+: SHA-256("CredSSP Server-To-Client Binding Hash\0" || nonce || key).
[[nodiscard]] Hash server_to_client_hash(const Nonce& nonce, std::span<const std::byte> public_key);

// Acceptor ----------------------------------------------------------------

struct AcceptorConfig {
    /// The TLS certificate's subjectPublicKey contents
    /// (TlsIdentity::subject_public_key()). Copied by the Acceptor.
    std::span<const std::byte> server_public_key;
    /// Creates an acceptor context for a mechanism OID (DER content octets),
    /// or returns nullptr when farland does not implement it. NTLM is
    /// spnego::ntlm_oid.
    std::function<std::unique_ptr<SecurityContext>(std::span<const std::byte> mech_oid)> make_mechanism;
    /// Optional: checks delegated password credentials against the identity
    /// the mechanism authenticated. Returning false fails the handshake.
    std::function<bool(const PasswordCredentials&, const Identity&)> accept_credentials;
    /// Highest CredSSP version to speak (2 to 6).
    std::uint32_t max_version = credssp::max_version;
};

/// The CredSSP server ([MS-CSSP] 3.1.5). Accepts SPNEGO (RFC 4178) and raw
/// NTLM tokens. Only password credentials can be delegated; smart-card and
/// Remote Credential Guard credentials fail the handshake.
class Acceptor final : public NlaAcceptor {
public:
    explicit Acceptor(AcceptorConfig config);

    void receive(std::span<const std::byte> bytes) override;
    [[nodiscard]] std::vector<std::byte> take_output() override;
    [[nodiscard]] Status status() const noexcept override { return status_; }
    [[nodiscard]] std::vector<std::byte> take_remaining_input() override;
    [[nodiscard]] const Identity& identity() const noexcept override { return identity_; }
    [[nodiscard]] std::optional<PasswordCredentials> take_credentials() override;
    [[nodiscard]] std::string_view failure_reason() const noexcept override { return failure_; }

    /// The CredSSP version in use, min(client, max_version); 0 before the first TSRequest.
    [[nodiscard]] std::uint32_t version() const noexcept { return version_; }
    /// Whether the client negotiated through SPNEGO rather than raw NTLM.
    [[nodiscard]] bool used_spnego() const noexcept { return spnego_; }

private:
    enum class Phase : std::uint8_t { negotiate, pub_key_auth, credentials, done };
    using Outcome = std::expected<void, std::string>;

    Outcome handle(TsRequest& request);
    Outcome handle_negotiate(TsRequest& request);
    Outcome start_spnego(const spnego::NegTokenInit& init, const TsRequest& request);
    Outcome finish_negotiation(const TsRequest& request, Bytes mech_output, const std::optional<Bytes>& client_mic,
                               spnego::NegTokenResp reply);
    Outcome handle_pub_key_auth(const TsRequest& request);
    Outcome answer_pub_key_auth(std::span<const std::byte> pub_key_auth, std::optional<Bytes> nego_token);
    Outcome handle_credentials(const TsRequest& request);
    void send(TsRequest message);
    void fail(std::string reason);

    AcceptorConfig config_;
    Bytes public_key_;
    Status status_ = Status::in_progress;
    Phase phase_ = Phase::negotiate;
    std::string failure_;
    Bytes input_;
    Bytes output_;
    Bytes remaining_;

    std::uint32_t peer_version_ = 0;
    std::uint32_t version_ = 0;
    std::optional<Nonce> nonce_;
    std::unique_ptr<SecurityContext> mech_;
    bool spnego_ = false;
    Bytes mech_types_der_;
    bool mic_required_ = false;
    bool client_mic_verified_ = false;

    Identity identity_;
    std::optional<PasswordCredentials> credentials_;
};

// Initiator ---------------------------------------------------------------

struct InitiatorConfig {
    /// An initiator context, already holding the user's credentials.
    std::unique_ptr<SecurityContext> mechanism;
    /// Wrap the mechanism in SPNEGO (as Windows does) or send raw tokens (as
    /// FreeRDP does when NTLM is its only mechanism).
    bool use_spnego = true;
    /// The server's subjectPublicKey contents, from the TLS peer certificate.
    Bytes server_public_key;
    /// Delegated to the server once it has proven its public key.
    PasswordCredentials credentials;
    /// CredSSP version to offer (2 to 6).
    std::uint32_t version = max_version;
    /// Test hook: the v5+ client nonce. Random when unset.
    std::optional<Nonce> client_nonce;
    /// SPNEGO only: mechanisms listed in mechTypes ahead of `mechanism`, which
    /// the initiator offers but cannot run (a Kerberos OID without a ticket,
    /// say). No optimistic token is sent, and the mechListMIC exchange
    /// becomes mandatory (RFC 4178 5).
    std::vector<spnego::Oid> preferred_mech_types;
};

/// The CredSSP client ([MS-CSSP] 3.1.5), the mirror image of Acceptor. The
/// first TSRequest is ready in take_output() right after construction.
class Initiator {
public:
    using Status = NlaAcceptor::Status;

    explicit Initiator(InitiatorConfig config);

    void receive(std::span<const std::byte> bytes);
    [[nodiscard]] std::vector<std::byte> take_output();
    [[nodiscard]] Status status() const noexcept { return status_; }
    /// Bytes that arrived after the server's last TSRequest.
    [[nodiscard]] std::vector<std::byte> take_remaining_input();
    [[nodiscard]] std::string_view failure_reason() const noexcept { return failure_; }
    /// The NTSTATUS from the server's errorCode, if it sent one.
    [[nodiscard]] std::optional<std::uint32_t> server_error_code() const noexcept { return server_error_; }
    /// The CredSSP version in use, min(ours, server's) once the server answered.
    [[nodiscard]] std::uint32_t version() const noexcept { return version_; }

private:
    /// final_token: our last token and mechListMIC are out; waiting for the
    /// acceptor's mechListMIC before sending pubKeyAuth.
    enum class Phase : std::uint8_t { negotiate, final_token, pub_key_auth, done };
    using Outcome = std::expected<void, std::string>;

    Outcome start();
    Outcome handle(TsRequest& request);
    Outcome handle_negotiate(const TsRequest& request);
    Outcome send_step(Step step);
    Outcome handle_final_token(const TsRequest& request);
    void send_pub_key_auth(TsRequest message);
    Outcome handle_pub_key_auth(const TsRequest& request);
    void send(TsRequest message);
    void fail(std::string reason);

    InitiatorConfig config_;
    Nonce nonce_;
    Status status_ = Status::in_progress;
    Phase phase_ = Phase::negotiate;
    std::string failure_;
    std::optional<std::uint32_t> server_error_;
    Bytes input_;
    Bytes output_;
    Bytes remaining_;

    std::uint32_t version_;
    std::uint32_t server_version_ = 0;
    bool first_reply_ = true;
    bool pending_first_step_ = false;
    Bytes mech_types_der_;
    bool mic_required_ = false;
    bool server_mic_verified_ = false;
};

}  // namespace farland::auth::credssp
