// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/auth/gss.hpp>
#include <farland/base/error.hpp>
#include <farland/server/preauth.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

/// Privilege separation (docs/PLAN.md §6): the messages between a sandboxed
/// network process, which parses everything a client sends before
/// authentication, and the monitor, which alone holds the credential store.
///
/// The network process never sees an NT hash: it asks the monitor to verify
/// NTLMv2 responses and delegated passwords. When pre-authentication is
/// complete it reports the result, and the monitor accepts an NLA identity
/// only if it verified that identity itself for this connection.
///
/// Kerberos goes further: the whole security context lives in the monitor,
/// because the keytab is the host's long-term key and the sandboxed process
/// has no file system at all. The network process drives that context one
/// operation at a time over this channel -- a step, a seal, an unseal, a
/// signature -- and never holds the session key either.
///
/// Wire format on the control channel: u32le length of what follows, a u8
/// message type, then the fields. Strings are u16le length plus UTF-8.
namespace farland::server::privsep {

/// One operation on the Kerberos context the monitor holds for this
/// connection. The context is created on the first `step`.
enum class KerberosOp : std::uint8_t {
    step = 1,     ///< gss_accept_sec_context with the client's token
    wrap = 2,     ///< GSS_WrapEx, sealing
    unwrap = 3,   ///< the reverse
    get_mic = 4,  ///< GSS_GetMIC, for SPNEGO's mechListMIC
    verify_mic = 5,
};

struct KerberosRequest {
    KerberosOp op = KerberosOp::step;
    /// The token, the plaintext, the sealed message or the signed message.
    std::vector<std::byte> data;
    /// `verify_mic` only: the signature to check.
    std::vector<std::byte> mic;
    /// `step` only: the mechanism OID SPNEGO settled on, so the monitor
    /// knows whether this is Kerberos at all and which OID to echo.
    std::vector<std::byte> mechanism_oid;

    friend bool operator==(const KerberosRequest&, const KerberosRequest&) = default;
};

struct KerberosResponse {
    /// False when the operation failed; the network process then fails the
    /// handshake without learning why. The monitor logs the reason.
    bool ok = false;
    /// The token, the sealed message or the plaintext, as the operation
    /// produces. Empty for `verify_mic`.
    std::vector<std::byte> data;
    /// `step` only: the context is established and `user`/`domain` name the
    /// principal that proved it.
    bool complete = false;
    std::string user;
    std::string domain;

    friend bool operator==(const KerberosResponse&, const KerberosResponse&) = default;
};

struct VerifyNtlmRequest {
    std::string user;
    std::string domain;
    std::array<std::byte, 8> server_challenge{};
    std::vector<std::byte> nt_response;
};
struct VerifyNtlmResponse {
    std::optional<std::array<std::byte, 16>> session_base_key;
};
struct VerifyPasswordRequest {
    std::string user;
    std::string domain;
    SecretString password;
};
struct VerifyPasswordResponse {
    bool ok = false;
};
/// Pre-authentication is complete; the monitor may start the session.
struct Authenticated {
    Negotiation negotiation;
};

using Message = std::variant<VerifyNtlmRequest, VerifyNtlmResponse, VerifyPasswordRequest, VerifyPasswordResponse,
                             Authenticated, KerberosRequest, KerberosResponse>;

/// Largest message either side accepts (length prefix excluded). A Kerberos
/// AP-REQ carrying a PAC is the big one: Windows' MaxTokenSize is 48000, and
/// the rest of a message is small beside it.
inline constexpr std::size_t max_message_size = 64 * 1024;

[[nodiscard]] std::vector<std::byte> encode(const Message& message);
/// Length of the first complete message in `buffered` (prefix included), or
/// nullopt until it has fully arrived. Errors for an oversized length.
[[nodiscard]] Result<std::optional<std::size_t>> message_length(std::span<const std::byte> buffered);
/// Decodes exactly one framed message.
[[nodiscard]] Result<Message> decode(std::span<const std::byte> frame);

/// One framed request out, one framed reply in (blocking).
using Call = std::function<Result<std::vector<std::byte>>(std::span<const std::byte> request)>;

/// The network process's view of the monitor: an NtlmVerifier whose answers
/// come over the control channel. `call` sends one framed request and
/// returns the framed reply (blocking); an error counts as a failed check.
class RemoteVerifier final : public auth::NtlmVerifier {
public:
    using Call = privsep::Call;

    explicit RemoteVerifier(Call call) : call_(std::move(call)) {}

    [[nodiscard]] std::optional<std::array<std::byte, 16>>
    session_base_key(std::string_view user, std::string_view domain, std::span<const std::byte, 8> server_challenge,
                     std::span<const std::byte> nt_challenge_response) override;
    [[nodiscard]] bool verify_password(std::string_view user, std::string_view domain,
                                       std::string_view password) override;

private:
    Call call_;
};

/// The Kerberos context of one connection, as the network process sees it:
/// a SecurityContext whose every operation is a round trip to the monitor.
/// Nothing of Kerberos -- the keytab, the ticket's keys, the session key --
/// is ever in this process.
class RemoteKerberos final : public auth::SecurityContext {
public:
    RemoteKerberos(Call call, std::span<const std::byte> mechanism_oid)
        : call_(std::move(call)), mechanism_oid_(mechanism_oid.begin(), mechanism_oid.end())
    {
    }

    [[nodiscard]] std::span<const std::byte> mechanism() const noexcept override { return mechanism_oid_; }
    [[nodiscard]] Result<auth::Step> step(std::span<const std::byte> input) override;
    [[nodiscard]] bool complete() const noexcept override { return complete_; }
    [[nodiscard]] std::vector<std::byte> wrap(std::span<const std::byte> plaintext) override;
    [[nodiscard]] Result<std::vector<std::byte>> unwrap(std::span<const std::byte> wrapped) override;
    [[nodiscard]] std::vector<std::byte> get_mic(std::span<const std::byte> message) override;
    [[nodiscard]] Result<void> verify_mic(std::span<const std::byte> message, std::span<const std::byte> mic) override;
    [[nodiscard]] const auth::Identity& identity() const noexcept override { return identity_; }

private:
    [[nodiscard]] Result<KerberosResponse> ask(KerberosOp op, std::span<const std::byte> data,
                                               std::span<const std::byte> mic = {});

    Call call_;
    std::vector<std::byte> mechanism_oid_;
    bool complete_ = false;
    auth::Identity identity_;
};

/// The monitor's side of one connection: answers verification requests with
/// the real verifier and keeps the network process honest.
class MonitorService {
public:
    /// Creates the Kerberos acceptor context for a mechanism OID, or returns
    /// nullptr where this build, this configuration or that OID has none.
    using KerberosFactory = std::function<std::unique_ptr<auth::SecurityContext>(std::span<const std::byte> mech_oid)>;

    /// At most `max_attempts` verifications per connection, successful or not.
    explicit MonitorService(auth::NtlmVerifier& verifier, unsigned max_attempts = 8)
        : verifier_(&verifier), max_attempts_(max_attempts)
    {
    }

    /// Lets this connection authenticate with Kerberos. Without it a
    /// Kerberos request is refused, which is what a build or a host without
    /// a keytab does.
    void serve_kerberos(KerberosFactory factory) { kerberos_ = std::move(factory); }

    /// Handles one framed message. Returns the framed reply for requests,
    /// nothing for the `Authenticated` notice. An error means the network
    /// process misbehaved; the monitor drops the connection.
    [[nodiscard]] Result<std::optional<std::vector<std::byte>>> handle(std::span<const std::byte> frame);

    /// The negotiation reported with `Authenticated`, once accepted.
    [[nodiscard]] std::optional<Negotiation> take_authenticated() { return std::exchange(authenticated_, {}); }

private:
    [[nodiscard]] Result<KerberosResponse> handle_kerberos(const KerberosRequest& request);

    auth::NtlmVerifier* verifier_;  ///< never null
    KerberosFactory kerberos_;
    std::unique_ptr<auth::SecurityContext> kerberos_context_;
    unsigned max_attempts_;
    unsigned attempts_ = 0;
    /// Users this service verified, as (user, domain) exactly as sent.
    std::set<std::pair<std::string, std::string>> verified_;
    bool reported_ = false;
    std::optional<Negotiation> authenticated_;
};

}  // namespace farland::server::privsep
