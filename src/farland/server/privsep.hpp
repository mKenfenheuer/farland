// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/auth/gss.hpp>
#include <farland/base/error.hpp>
#include <farland/server/preauth.hpp>

#include <array>
#include <cstdint>
#include <functional>
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
/// Wire format on the control channel: u32le length of what follows, a u8
/// message type, then the fields. Strings are u16le length plus UTF-8.
namespace farland::server::privsep {

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

using Message =
    std::variant<VerifyNtlmRequest, VerifyNtlmResponse, VerifyPasswordRequest, VerifyPasswordResponse, Authenticated>;

/// Largest message either side accepts (length prefix excluded).
inline constexpr std::size_t max_message_size = 8192;

[[nodiscard]] std::vector<std::byte> encode(const Message& message);
/// Length of the first complete message in `buffered` (prefix included), or
/// nullopt until it has fully arrived. Errors for an oversized length.
[[nodiscard]] Result<std::optional<std::size_t>> message_length(std::span<const std::byte> buffered);
/// Decodes exactly one framed message.
[[nodiscard]] Result<Message> decode(std::span<const std::byte> frame);

/// The network process's view of the monitor: an NtlmVerifier whose answers
/// come over the control channel. `call` sends one framed request and
/// returns the framed reply (blocking); an error counts as a failed check.
class RemoteVerifier final : public auth::NtlmVerifier {
public:
    using Call = std::function<Result<std::vector<std::byte>>(std::span<const std::byte> request)>;

    explicit RemoteVerifier(Call call) : call_(std::move(call)) {}

    [[nodiscard]] std::optional<std::array<std::byte, 16>>
    session_base_key(std::string_view user, std::string_view domain, std::span<const std::byte, 8> server_challenge,
                     std::span<const std::byte> nt_challenge_response) override;
    [[nodiscard]] bool verify_password(std::string_view user, std::string_view domain,
                                       std::string_view password) override;

private:
    Call call_;
};

/// The monitor's side of one connection: answers verification requests with
/// the real verifier and keeps the network process honest.
class MonitorService {
public:
    /// At most `max_attempts` verifications per connection, successful or not.
    explicit MonitorService(auth::NtlmVerifier& verifier, unsigned max_attempts = 8)
        : verifier_(&verifier), max_attempts_(max_attempts)
    {
    }

    /// Handles one framed message. Returns the framed reply for requests,
    /// nothing for the `Authenticated` notice. An error means the network
    /// process misbehaved; the monitor drops the connection.
    [[nodiscard]] Result<std::optional<std::vector<std::byte>>> handle(std::span<const std::byte> frame);

    /// The negotiation reported with `Authenticated`, once accepted.
    [[nodiscard]] std::optional<Negotiation> take_authenticated() { return std::exchange(authenticated_, {}); }

private:
    auth::NtlmVerifier* verifier_;  ///< never null
    unsigned max_attempts_;
    unsigned attempts_ = 0;
    /// Users this service verified, as (user, domain) exactly as sent.
    std::set<std::pair<std::string, std::string>> verified_;
    bool reported_ = false;
    std::optional<Negotiation> authenticated_;
};

}  // namespace farland::server::privsep
