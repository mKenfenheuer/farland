// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/text.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Interfaces shared by the NLA pieces: GSS-style security contexts (NTLM now,
/// Kerberos later), the CredSSP acceptor the server's pre-authentication stage
/// drives, and the verifier that keeps password hashes out of the pre-auth
/// process (docs/PLAN.md §6, privilege separation).
namespace farland::auth {

/// An authenticated principal as the client named it.
struct Identity {
    std::string user;
    std::string domain;
};

/// Password credentials delegated by the client (TSPasswordCreds, [MS-CSSP] 2.2.1.2.1).
struct PasswordCredentials {
    std::string domain;
    std::string user;
    SecretString password;
};

/// One step of a security context: the token to send (possibly empty) and
/// whether the context is established.
struct Step {
    std::vector<std::byte> token;
    bool complete = false;
};

/// A security context for one mechanism and one role, in the style of the
/// GSS-API (RFC 2743). CredSSP and SPNEGO drive it; the NTLM initiator and
/// acceptor implement it. Sans-IO and single-threaded.
class SecurityContext {
public:
    SecurityContext() = default;
    SecurityContext(const SecurityContext&) = delete;
    SecurityContext& operator=(const SecurityContext&) = delete;
    SecurityContext(SecurityContext&&) = delete;
    SecurityContext& operator=(SecurityContext&&) = delete;
    virtual ~SecurityContext() = default;

    /// The mechanism's OID as DER content octets (no tag or length), e.g. NTLM
    /// 1.3.6.1.4.1.311.2.2.10 is 2b 06 01 04 01 82 37 02 02 0a.
    [[nodiscard]] virtual std::span<const std::byte> mechanism() const noexcept = 0;
    /// Consumes the peer's token (empty on an initiator's first call) and
    /// returns the next one. An error ends the context.
    [[nodiscard]] virtual Result<Step> step(std::span<const std::byte> input) = 0;
    [[nodiscard]] virtual bool complete() const noexcept = 0;

    /// GSS_WrapEx with confidentiality, in the layout CredSSP carries it
    /// (for NTLM: the 16-byte signature, then the sealed message).
    [[nodiscard]] virtual std::vector<std::byte> wrap(std::span<const std::byte> plaintext) = 0;
    [[nodiscard]] virtual Result<std::vector<std::byte>> unwrap(std::span<const std::byte> wrapped) = 0;
    /// GSS_GetMIC and GSS_VerifyMIC, for SPNEGO's mechListMIC.
    [[nodiscard]] virtual std::vector<std::byte> get_mic(std::span<const std::byte> message) = 0;
    [[nodiscard]] virtual Result<void> verify_mic(std::span<const std::byte> message,
                                                  std::span<const std::byte> mic) = 0;
    /// Restarts sealing/signing state after the SPNEGO mechListMIC exchange ([MS-SPNG] 3.3.5.1); a no-op for
    /// mechanisms without such state.
    virtual void reset_cipher_state() {}

    /// Acceptors: the authenticated client once complete. Initiators: the
    /// identity they authenticated as.
    [[nodiscard]] virtual const Identity& identity() const noexcept = 0;
};

/// NT hash: MD4 of the UTF-16LE password ([MS-NLMP] 3.3.1, NTOWFv1).
using NtHash = std::array<std::byte, 16>;

/// Verifies NTLMv2 responses and delegated passwords without handing the NT
/// hash to the caller. In a privilege-separated server the monitor process
/// implements it; the network-facing process only ever sees session keys.
class NtlmVerifier {
public:
    NtlmVerifier() = default;
    NtlmVerifier(const NtlmVerifier&) = delete;
    NtlmVerifier& operator=(const NtlmVerifier&) = delete;
    NtlmVerifier(NtlmVerifier&&) = delete;
    NtlmVerifier& operator=(NtlmVerifier&&) = delete;
    virtual ~NtlmVerifier() = default;

    /// Checks an NTLMv2 response ([MS-NLMP] 3.3.2): recomputes NTProofStr
    /// from the user's NT hash, `user` and `domain` exactly as they appear in
    /// the AUTHENTICATE message, the server challenge, and the response blob.
    /// Returns the SessionBaseKey on success, nullopt for an unknown user or a
    /// wrong password.
    [[nodiscard]] virtual std::optional<std::array<std::byte, 16>>
    session_base_key(std::string_view user, std::string_view domain, std::span<const std::byte, 8> server_challenge,
                     std::span<const std::byte> nt_challenge_response) = 0;

    /// Checks a delegated password against the stored NT hash.
    [[nodiscard]] virtual bool verify_password(std::string_view user, std::string_view domain,
                                               std::string_view password) = 0;
};

/// The CredSSP (NLA) server handshake as the server's pre-authentication
/// stage drives it ([MS-CSSP] 3.1.5). Sans-IO: TLS plaintext in, TLS
/// plaintext out.
class NlaAcceptor {
public:
    enum class Status : std::uint8_t { in_progress, succeeded, failed };

    NlaAcceptor() = default;
    NlaAcceptor(const NlaAcceptor&) = delete;
    NlaAcceptor& operator=(const NlaAcceptor&) = delete;
    NlaAcceptor(NlaAcceptor&&) = delete;
    NlaAcceptor& operator=(NlaAcceptor&&) = delete;
    virtual ~NlaAcceptor() = default;

    virtual void receive(std::span<const std::byte> bytes) = 0;
    [[nodiscard]] virtual std::vector<std::byte> take_output() = 0;
    [[nodiscard]] virtual Status status() const noexcept = 0;
    /// Bytes that arrived after the final TSRequest: the start of the RDP stream.
    [[nodiscard]] virtual std::vector<std::byte> take_remaining_input() = 0;
    /// The authenticated client, once succeeded.
    [[nodiscard]] virtual const Identity& identity() const noexcept = 0;
    /// Delegated password credentials, if the client sent TSPasswordCreds.
    [[nodiscard]] virtual std::optional<PasswordCredentials> take_credentials() = 0;
    /// Why the handshake failed, for logs. Never sent to the client.
    [[nodiscard]] virtual std::string_view failure_reason() const noexcept = 0;
};

}  // namespace farland::auth
