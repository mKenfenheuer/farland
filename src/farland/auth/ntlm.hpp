// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/auth/gss.hpp>
#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/text.hpp>
#include <farland/base/writer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// NTLM ([MS-NLMP]) as used inside CredSSP: NTLMv2 with extended session
/// security, key exchange, the MIC and channel bindings. Message codecs, the
/// NTLMv2 key derivation, and sans-IO security contexts for both roles.
namespace farland::auth::ntlm {

/// NegotiateFlags ([MS-NLMP] 2.2.2.5). Letters are the spec's bit names.
namespace flags {
inline constexpr std::uint32_t negotiate_unicode = 0x00000001U;                   // A
inline constexpr std::uint32_t negotiate_oem = 0x00000002U;                       // B
inline constexpr std::uint32_t request_target = 0x00000004U;                      // C
inline constexpr std::uint32_t negotiate_sign = 0x00000010U;                      // D
inline constexpr std::uint32_t negotiate_seal = 0x00000020U;                      // E
inline constexpr std::uint32_t negotiate_datagram = 0x00000040U;                  // F
inline constexpr std::uint32_t negotiate_lm_key = 0x00000080U;                    // G
inline constexpr std::uint32_t negotiate_ntlm = 0x00000200U;                      // H
inline constexpr std::uint32_t anonymous = 0x00000800U;                           // J
inline constexpr std::uint32_t negotiate_oem_domain_supplied = 0x00001000U;       // K
inline constexpr std::uint32_t negotiate_oem_workstation_supplied = 0x00002000U;  // L
inline constexpr std::uint32_t negotiate_always_sign = 0x00008000U;               // M
inline constexpr std::uint32_t target_type_domain = 0x00010000U;                  // N
inline constexpr std::uint32_t target_type_server = 0x00020000U;                  // O
inline constexpr std::uint32_t negotiate_extended_sessionsecurity = 0x00080000U;  // P
inline constexpr std::uint32_t negotiate_identify = 0x00100000U;                  // Q
inline constexpr std::uint32_t request_non_nt_session_key = 0x00400000U;          // R
inline constexpr std::uint32_t negotiate_target_info = 0x00800000U;               // S
inline constexpr std::uint32_t negotiate_version = 0x02000000U;                   // T
inline constexpr std::uint32_t negotiate_128 = 0x20000000U;                       // U
inline constexpr std::uint32_t negotiate_key_exch = 0x40000000U;                  // V
inline constexpr std::uint32_t negotiate_56 = 0x80000000U;                        // W
}  // namespace flags

/// AV_PAIR AvId values ([MS-NLMP] 2.2.2.1).
enum class AvId : std::uint16_t {
    eol = 0x0000,
    nb_computer_name = 0x0001,
    nb_domain_name = 0x0002,
    dns_computer_name = 0x0003,
    dns_domain_name = 0x0004,
    dns_tree_name = 0x0005,
    flags = 0x0006,
    timestamp = 0x0007,
    single_host = 0x0008,
    target_name = 0x0009,
    channel_bindings = 0x000a,
};

/// MsvAvFlags bits ([MS-NLMP] 2.2.2.1).
inline constexpr std::uint32_t av_flag_constrained = 0x00000001U;
inline constexpr std::uint32_t av_flag_mic_present = 0x00000002U;
inline constexpr std::uint32_t av_flag_untrusted_spn = 0x00000004U;

/// farland limits on peer-supplied names and lists.
inline constexpr std::size_t max_name_chars = 256;  ///< UTF-16 code units in user, domain, workstation.
inline constexpr std::size_t max_av_pairs = 32;     ///< AV pairs in one list, MsvAvEOL excluded.

/// One AV_PAIR ([MS-NLMP] 2.2.2.1). Unknown ids are kept as they are.
struct AvPair {
    std::uint16_t id = 0;
    std::vector<std::byte> value;
    friend bool operator==(const AvPair&, const AvPair&) = default;
};

/// Decodes AV pairs up to and including MsvAvEOL (required), consuming them
/// from `r`; bytes after MsvAvEOL are left unread. The result excludes
/// MsvAvEOL. Duplicate ids, more than `max_av_pairs` pairs, and wrong lengths
/// for MsvAvFlags (4), MsvAvTimestamp (8) and MsvAvChannelBindings (16) fail.
[[nodiscard]] Result<std::vector<AvPair>> decode_av_pairs(Reader& r);
/// Writes the pairs followed by MsvAvEOL. The list must not contain MsvAvEOL.
void encode_av_pairs(Writer& w, std::span<const AvPair> pairs);
/// The value of the first pair with `id`, if any.
[[nodiscard]] std::optional<std::span<const std::byte>> find_av_pair(std::span<const AvPair> pairs, AvId id) noexcept;

/// VERSION ([MS-NLMP] 2.2.2.10). Informational only.
struct Version {
    std::uint8_t major = 10;
    std::uint8_t minor = 0;
    std::uint16_t build = 20348;
    std::uint8_t revision = 15;  ///< NTLMSSP_REVISION_W2K3
    friend bool operator==(const Version&, const Version&) = default;
};

/// NEGOTIATE_MESSAGE ([MS-NLMP] 2.2.1.1). Domain and workstation are OEM
/// bytes, only meaningful with the *_SUPPLIED flags.
struct NegotiateMessage {
    std::uint32_t flags = 0;
    std::vector<std::byte> domain;
    std::vector<std::byte> workstation;
    std::optional<Version> version;
    friend bool operator==(const NegotiateMessage&, const NegotiateMessage&) = default;
};

/// CHALLENGE_MESSAGE ([MS-NLMP] 2.2.1.2). The target name is kept as raw
/// bytes (UTF-16LE with NTLMSSP_NEGOTIATE_UNICODE).
struct ChallengeMessage {
    std::uint32_t flags = 0;
    std::array<std::byte, 8> server_challenge{};
    std::vector<std::byte> target_name;
    std::vector<AvPair> target_info;  ///< Written when non-empty or NTLMSSP_NEGOTIATE_TARGET_INFO is set.
    std::optional<Version> version;
    friend bool operator==(const ChallengeMessage&, const ChallengeMessage&) = default;
};

/// AUTHENTICATE_MESSAGE ([MS-NLMP] 2.2.1.3). Only the Unicode form is
/// supported; names are decoded to UTF-8, capped at `max_name_chars`, and may
/// not contain NUL.
struct AuthenticateMessage {
    std::uint32_t flags = 0;
    std::vector<std::byte> lm_challenge_response;
    std::vector<std::byte> nt_challenge_response;
    std::string domain;
    std::string user;
    std::string workstation;
    std::vector<std::byte> encrypted_random_session_key;
    std::optional<Version> version;
    /// Present when the header has room for it before the payload.
    std::optional<std::array<std::byte, 16>> mic;
    friend bool operator==(const AuthenticateMessage&, const AuthenticateMessage&) = default;
};

/// Offset of the MIC in an AUTHENTICATE_MESSAGE with these flags: after the
/// Version field when NTLMSSP_NEGOTIATE_VERSION is set (72), else 64.
[[nodiscard]] constexpr std::size_t authenticate_mic_offset(std::uint32_t negotiate_flags) noexcept
{
    return (negotiate_flags & flags::negotiate_version) != 0 ? 72 : 64;
}

/// Encoders. Payloads are laid out in the order Windows uses; the Version
/// field is written when `version` is set (and, for AUTHENTICATE, whenever a
/// MIC is written and NTLMSSP_NEGOTIATE_VERSION is set).
[[nodiscard]] std::vector<std::byte> encode(const NegotiateMessage& message);
[[nodiscard]] std::vector<std::byte> encode(const ChallengeMessage& message);
[[nodiscard]] std::vector<std::byte> encode(const AuthenticateMessage& message);

/// Decoders. Every non-empty payload field must lie inside the message, after
/// the fixed header, and must not overlap another field.
[[nodiscard]] Result<NegotiateMessage> decode_negotiate(std::span<const std::byte> message);
[[nodiscard]] Result<ChallengeMessage> decode_challenge(std::span<const std::byte> message);
[[nodiscard]] Result<AuthenticateMessage> decode_authenticate(std::span<const std::byte> message);

/// NTLMv2 cryptography ([MS-NLMP] 3.3.2, 3.4.5).
using Key = std::array<std::byte, 16>;

/// NTOWFv1: MD4 of the UTF-16LE password ([MS-NLMP] 3.3.1).
[[nodiscard]] NtHash nt_hash(std::string_view password);
/// NTOWFv2 = HMAC_MD5(NT hash, UNICODE(Uppercase(user) + domain)) ([MS-NLMP]
/// 3.3.2), the ResponseKeyNT. Uppercasing uses simple case mapping for Latin,
/// Greek and Cyrillic.
[[nodiscard]] Key ntowf_v2(const NtHash& hash, std::string_view user, std::string_view domain);
/// The NTLMv2_CLIENT_CHALLENGE that follows NTProofStr ([MS-NLMP] 2.2.2.7,
/// "temp" in 3.3.2): versions, timestamp, client challenge, AV pairs + MsvAvEOL, Z(4).
[[nodiscard]] std::vector<std::byte>
ntlmv2_temp(std::uint64_t timestamp, std::span<const std::byte, 8> client_challenge, std::span<const AvPair> av_pairs);
/// NTProofStr = HMAC_MD5(ResponseKeyNT, ServerChallenge + temp).
[[nodiscard]] Key nt_proof_str(const Key& response_key, std::span<const std::byte, 8> server_challenge,
                               std::span<const std::byte> temp);
/// SessionBaseKey = HMAC_MD5(ResponseKeyNT, NTProofStr). With NTLMv2 it is
/// also the KeyExchangeKey ([MS-NLMP] 3.4.5.1).
[[nodiscard]] Key session_base_key(const Key& response_key, std::span<const std::byte, 16> nt_proof);
/// LMv2 response = HMAC_MD5(ResponseKeyLM, ServerChallenge + ClientChallenge) + ClientChallenge.
[[nodiscard]] std::array<std::byte, 24> lmv2_response(const Key& response_key,
                                                      std::span<const std::byte, 8> server_challenge,
                                                      std::span<const std::byte, 8> client_challenge);

enum class Direction : std::uint8_t { client_to_server, server_to_client };
/// SIGNKEY ([MS-NLMP] 3.4.5.2), extended session security.
[[nodiscard]] Key signing_key(const Key& exported_session_key, Direction direction);
/// SEALKEY ([MS-NLMP] 3.4.5.3), extended session security: the exported key
/// is cut to 16, 7 or 5 bytes by NTLMSSP_NEGOTIATE_128 / _56 first.
[[nodiscard]] Key sealing_key(const Key& exported_session_key, std::uint32_t negotiate_flags, Direction direction);

/// MD5 of a gss_channel_bindings_struct with zero-length addresses and the
/// application data "tls-server-end-point:" + SHA-256(certificate) (RFC
/// 5929 4; farland certificates are signed with SHA-256). This is the
/// MsvAvChannelBindings value ([MS-NLMP] 2.2.2.1).
[[nodiscard]] std::array<std::byte, 16> channel_bindings_hash(std::span<const std::byte> certificate_der);

/// The NTLM mechanism OID 1.3.6.1.4.1.311.2.2.10 as DER content octets.
[[nodiscard]] std::span<const std::byte> mechanism_oid() noexcept;

namespace detail {
class Session;
}

enum class ChannelBindingPolicy : std::uint8_t {
    ignore,             ///< Do not look at MsvAvChannelBindings.
    verify_if_present,  ///< Reject bindings that do not match; accept absent (or all-zero) ones.
    require,            ///< Reject absent, all-zero or mismatched bindings.
};

struct AcceptorConfig {
    NtlmVerifier& verifier;  // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): config is short-lived
    /// Names for the CHALLENGE TargetInfo. Empty DNS names fall back to the NetBIOS names.
    std::string netbios_domain = "WORKGROUP";
    std::string netbios_computer = "FARLAND";
    std::string dns_domain;
    std::string dns_computer;
    /// channel_bindings_hash() of the server certificate.
    std::optional<std::array<std::byte, 16>> channel_bindings;
    /// `require` needs `channel_bindings`.
    ChannelBindingPolicy channel_binding_policy = ChannelBindingPolicy::verify_if_present;
    /// Test hooks: fixed server challenge and CHALLENGE timestamp (FILETIME).
    std::optional<std::array<std::byte, 8>> server_challenge;
    std::optional<std::uint64_t> timestamp;
};

/// NTLM server: NEGOTIATE in, CHALLENGE out, AUTHENTICATE in ([MS-NLMP]
/// 3.2.5). Accepts only NTLMv2 with extended session security.
class Acceptor final : public SecurityContext {
public:
    explicit Acceptor(AcceptorConfig config);
    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;
    Acceptor(Acceptor&&) = delete;
    Acceptor& operator=(Acceptor&&) = delete;
    ~Acceptor() override;

    [[nodiscard]] std::span<const std::byte> mechanism() const noexcept override;
    [[nodiscard]] Result<Step> step(std::span<const std::byte> input) override;
    [[nodiscard]] bool complete() const noexcept override;
    [[nodiscard]] std::vector<std::byte> wrap(std::span<const std::byte> plaintext) override;
    [[nodiscard]] Result<std::vector<std::byte>> unwrap(std::span<const std::byte> wrapped) override;
    [[nodiscard]] std::vector<std::byte> get_mic(std::span<const std::byte> message) override;
    [[nodiscard]] Result<void> verify_mic(std::span<const std::byte> message, std::span<const std::byte> mic) override;
    [[nodiscard]] const Identity& identity() const noexcept override;

    /// Flags in effect: the CHALLENGE's flags intersected with AUTHENTICATE's.
    [[nodiscard]] std::uint32_t negotiated_flags() const noexcept { return negotiated_flags_; }
    /// The client's MsvAvTargetName (the SPN it meant to reach), if it sent
    /// one. Not enforced here.
    [[nodiscard]] const std::optional<std::string>& target_name() const noexcept { return target_name_; }
    /// True when the client sent channel bindings that matched ours.
    [[nodiscard]] bool channel_bound() const noexcept { return channel_bound_; }
    /// Re-initialises both RC4 handles and both sequence numbers, as SPNEGO
    /// requires after the mechListMIC exchange ([MS-SPNG] 3.3.5.1 behaviour
    /// of Windows; FreeRDP and Samba do the same).
    void reset_cipher_state() override;

private:
    enum class State : std::uint8_t { expect_negotiate, expect_authenticate, complete, failed };
    [[nodiscard]] Result<Step> on_negotiate(std::span<const std::byte> input);
    [[nodiscard]] Result<Step> on_authenticate(std::span<const std::byte> input);

    AcceptorConfig config_;
    State state_ = State::expect_negotiate;
    std::array<std::byte, 8> server_challenge_{};
    std::vector<std::byte> negotiate_message_;
    std::vector<std::byte> challenge_message_;
    std::uint32_t challenge_flags_ = 0;
    std::uint32_t negotiated_flags_ = 0;
    std::optional<std::string> target_name_;
    bool channel_bound_ = false;
    Identity identity_;
    std::unique_ptr<detail::Session> session_;
};

struct InitiatorConfig {
    std::string user;
    std::string domain;
    SecretString password;
    std::string workstation;
    /// channel_bindings_hash() of the server certificate. Without it the
    /// client sends 16 zero bytes, which servers treat as "no bindings".
    std::optional<std::array<std::byte, 16>> channel_bindings;
    /// MsvAvTargetName, e.g. "TERMSRV/host.example".
    std::optional<std::string> target_name;
    /// Test hooks.
    std::optional<std::array<std::byte, 8>> client_challenge;
    std::optional<std::uint64_t> timestamp;  ///< Used when the CHALLENGE has no MsvAvTimestamp.
    std::optional<Key> random_session_key;
};

/// NTLM client: NEGOTIATE out, CHALLENGE in, AUTHENTICATE out ([MS-NLMP]
/// 3.1.5). Always NTLMv2 with extended session security, key exchange, a MIC
/// (MsvAvFlags = 2) and MsvAvChannelBindings.
class Initiator final : public SecurityContext {
public:
    explicit Initiator(InitiatorConfig config);
    Initiator(const Initiator&) = delete;
    Initiator& operator=(const Initiator&) = delete;
    Initiator(Initiator&&) = delete;
    Initiator& operator=(Initiator&&) = delete;
    ~Initiator() override;

    [[nodiscard]] std::span<const std::byte> mechanism() const noexcept override;
    [[nodiscard]] Result<Step> step(std::span<const std::byte> input) override;
    [[nodiscard]] bool complete() const noexcept override;
    [[nodiscard]] std::vector<std::byte> wrap(std::span<const std::byte> plaintext) override;
    [[nodiscard]] Result<std::vector<std::byte>> unwrap(std::span<const std::byte> wrapped) override;
    [[nodiscard]] std::vector<std::byte> get_mic(std::span<const std::byte> message) override;
    [[nodiscard]] Result<void> verify_mic(std::span<const std::byte> message, std::span<const std::byte> mic) override;
    [[nodiscard]] const Identity& identity() const noexcept override;

    [[nodiscard]] std::uint32_t negotiated_flags() const noexcept { return negotiated_flags_; }
    /// See Acceptor::reset_cipher_state().
    void reset_cipher_state() override;

private:
    enum class State : std::uint8_t { initial, expect_challenge, complete, failed };
    [[nodiscard]] Result<Step> on_challenge(std::span<const std::byte> input);

    InitiatorConfig config_;
    NtHash nt_hash_{};
    State state_ = State::initial;
    std::vector<std::byte> negotiate_message_;
    std::uint32_t negotiated_flags_ = 0;
    Identity identity_;
    std::unique_ptr<detail::Session> session_;
};

/// Verifies NTLMv2 against NT hashes from a lookup function, in process.
class LocalNtlmVerifier final : public NtlmVerifier {
public:
    /// Returns the user's NT hash, or nullopt for an unknown user.
    using Lookup = std::function<std::optional<NtHash>(std::string_view user, std::string_view domain)>;

    explicit LocalNtlmVerifier(Lookup lookup) : lookup_(std::move(lookup)) {}

    [[nodiscard]] std::optional<std::array<std::byte, 16>>
    session_base_key(std::string_view user, std::string_view domain, std::span<const std::byte, 8> server_challenge,
                     std::span<const std::byte> nt_challenge_response) override;
    [[nodiscard]] bool verify_password(std::string_view user, std::string_view domain,
                                       std::string_view password) override;

private:
    Lookup lookup_;
};

}  // namespace farland::auth::ntlm
