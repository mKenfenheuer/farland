// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

/// Auto-reconnect cookies, [MS-RDPBCGR] 5.5. The server gives the client a
/// 16-byte random, the ArcRandomBits of ARC_SC_PRIVATE_PACKET (sent in the
/// Save Session Info PDU, proto/save_session_info.hpp). A reconnecting client
/// proves it holds that random with the SecurityVerifier of its
/// ARC_CS_PRIVATE_PACKET (in the Client Info PDU):
///
///     SecurityVerifier = HMAC-MD5(key = ArcRandomBits, data = client random)
///
/// The client random under Enhanced RDP Security (TLS, CredSSP/NLA, RDSTLS,
/// which is all farland speaks): there is no Security Exchange PDU and so no
/// client random. [MS-RDPBCGR] 5.5, step 4: "When Enhanced RDP Security is in
/// effect the client random value is not generated (section 5.3.2). In this
/// case, for the purpose of generating the security verifier, the client
/// random is assumed to be an array of 32 zero bytes." FreeRDP's client does
/// the same: rdp_compute_client_auto_reconnect_cookie
/// (libfreerdp/core/info.c) HMACs a zeroed 32-byte ClientRandom and copies
/// the real one in only when SelectedProtocol == PROTOCOL_RDP (since commit
/// 076b8a84c, 2013).
///
/// The verifier is therefore fixed for a given random. What keeps it from
/// being replayed is that the server rotates the random on every connection
/// (5.5: "flushed and regenerated whenever a client connects").
namespace farland::auth::arc {

inline constexpr std::size_t random_size = 16;
inline constexpr std::size_t client_random_size = 32;
using Random = std::array<std::byte, random_size>;
using Verifier = std::array<std::byte, 16>;

/// The client random that Enhanced RDP Security assumes: 32 zero bytes.
inline constexpr std::array<std::byte, client_random_size> enhanced_security_client_random{};

/// SecurityVerifier for `random` and `client_random` ([MS-RDPBCGR] 5.5).
[[nodiscard]] Verifier
security_verifier(const Random& random,
                  std::span<const std::byte, client_random_size> client_random = enhanced_security_client_random);

/// The auto-reconnect secret of one session: its logon id and the current
/// random. Each random is good for one check, and rotate() replaces it, so
/// only the client of the most recent connection can come back.
class Secret {
public:
    explicit Secret(std::uint32_t logon_id) noexcept : logon_id_(logon_id) {}
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    Secret(Secret&&) = delete;
    Secret& operator=(Secret&&) = delete;
    ~Secret();

    [[nodiscard]] std::uint32_t logon_id() const noexcept { return logon_id_; }

    /// A fresh random for the next Save Session Info PDU; the previous one
    /// stops working. Fails only if OpenSSL has no randomness.
    [[nodiscard]] Result<Random> rotate();

    /// Checks a returning client's ARC_CS_PRIVATE_PACKET fields, in constant
    /// time. Consumes the current random whatever the outcome.
    [[nodiscard]] bool
    verify(std::uint32_t logon_id, std::span<const std::byte, 16> verifier,
           std::span<const std::byte, client_random_size> client_random = enhanced_security_client_random);

private:
    void forget() noexcept;

    std::uint32_t logon_id_;
    std::optional<Random> random_;
};

}  // namespace farland::auth::arc
