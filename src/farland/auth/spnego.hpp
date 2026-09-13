// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

/// SPNEGO token codecs (RFC 4178, [MS-SPNG]): the GSS-API InitialContextToken
/// wrapper (RFC 2743 3.1) around NegTokenInit, and NegTokenResp. CredSSP
/// carries these in TSRequest.negoTokens ([MS-CSSP] 2.2.1.1). The negotiation
/// logic itself lives in the CredSSP state machines (credssp.hpp).
namespace farland::auth::spnego {

using Bytes = std::vector<std::byte>;

/// An OBJECT IDENTIFIER as DER content octets, without tag and length.
using Oid = std::vector<std::byte>;

/// 1.3.6.1.5.5.2, SPNEGO itself (RFC 4178 3.1).
inline constexpr std::array spnego_oid{std::byte{0x2b}, std::byte{0x06}, std::byte{0x01},
                                       std::byte{0x05}, std::byte{0x05}, std::byte{0x02}};

/// 1.3.6.1.4.1.311.2.2.10, NTLM ([MS-SPNG] 1.9, [MS-NLMP]).
inline constexpr std::array ntlm_oid{std::byte{0x2b}, std::byte{0x06}, std::byte{0x01}, std::byte{0x04},
                                     std::byte{0x01}, std::byte{0x82}, std::byte{0x37}, std::byte{0x02},
                                     std::byte{0x02}, std::byte{0x0a}};

/// 1.2.840.113554.1.2.2, Kerberos V5 (RFC 4121).
inline constexpr std::array kerberos_oid{std::byte{0x2a}, std::byte{0x86}, std::byte{0x48},
                                         std::byte{0x86}, std::byte{0xf7}, std::byte{0x12},
                                         std::byte{0x01}, std::byte{0x02}, std::byte{0x02}};

/// 1.2.840.48018.1.2.2, the legacy Microsoft Kerberos OID that Windows
/// clients list first ([MS-SPNG] 3.1.5.2).
inline constexpr std::array ms_kerberos_oid{std::byte{0x2a}, std::byte{0x86}, std::byte{0x48},
                                            std::byte{0x82}, std::byte{0xf7}, std::byte{0x12},
                                            std::byte{0x01}, std::byte{0x02}, std::byte{0x02}};

/// Largest mechanism token accepted: Windows' default MaxTokenSize.
inline constexpr std::size_t max_token_size = 48000;
/// Largest mechListMIC accepted. NTLM's is 16 bytes, Kerberos' a few dozen.
inline constexpr std::size_t max_mic_size = 1024;
/// Most entries accepted in mechTypes. Windows sends four.
inline constexpr std::size_t max_mech_types = 16;
/// Longest OBJECT IDENTIFIER accepted, in content octets.
inline constexpr std::size_t max_oid_size = 32;

/// NegTokenResp.negState (RFC 4178 4.2.2).
enum class NegState : std::uint8_t {
    accept_completed = 0,
    accept_incomplete = 1,
    reject = 2,
    request_mic = 3,
};

/// NegTokenInit (RFC 4178 4.2.1), also accepting [MS-SPNG] 2.2.1
/// NegTokenInit2. reqFlags and negHints are checked but not kept.
struct NegTokenInit {
    std::vector<Oid> mech_types;
    /// The exact DER of the MechTypeList (the SEQUENCE, tag included). The
    /// mechListMIC is computed over these bytes (RFC 4178 5), so decoding
    /// keeps them as received and encoding writes them verbatim. Use
    /// `make_neg_token_init` to keep them consistent with `mech_types`.
    Bytes mech_types_der;
    std::optional<Bytes> mech_token;
    std::optional<Bytes> mech_list_mic;

    friend bool operator==(const NegTokenInit&, const NegTokenInit&) = default;
};

/// NegTokenResp (RFC 4178 4.2.2).
struct NegTokenResp {
    std::optional<NegState> neg_state;
    std::optional<Oid> supported_mech;
    std::optional<Bytes> response_token;
    std::optional<Bytes> mech_list_mic;

    friend bool operator==(const NegTokenResp&, const NegTokenResp&) = default;
};

/// NegotiationToken (RFC 4178 4.2).
using NegotiationToken = std::variant<NegTokenInit, NegTokenResp>;

/// MechTypeList ::= SEQUENCE OF MechType (RFC 4178 4.1), as DER.
[[nodiscard]] Bytes encode_mech_types(std::span<const Oid> mech_types);

/// A NegTokenInit whose `mech_types_der` matches `mech_types`.
[[nodiscard]] NegTokenInit make_neg_token_init(std::vector<Oid> mech_types,
                                               std::optional<Bytes> mech_token = std::nullopt);

/// The initiator's first token: InitialContextToken { SPNEGO OID, [0] NegTokenInit }.
[[nodiscard]] Bytes encode(const NegTokenInit& token);
/// Every later token: [1] NegTokenResp, without the InitialContextToken wrapper.
[[nodiscard]] Bytes encode(const NegTokenResp& token);

/// Decodes either form above (DER). Anything else, including an
/// InitialContextToken for another mechanism, is an error.
[[nodiscard]] Result<NegotiationToken> decode(std::span<const std::byte> token);

/// Whether `token` is a bare NTLM message ("NTLMSSP\0", [MS-NLMP] 2.2.1)
/// rather than SPNEGO. CredSSP clients without Kerberos send those.
[[nodiscard]] bool is_raw_ntlm(std::span<const std::byte> token) noexcept;

}  // namespace farland::auth::spnego
