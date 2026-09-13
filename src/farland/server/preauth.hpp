// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/auth/gss.hpp>
#include <farland/base/writer.hpp>
#include <farland/proto/x224.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

/// The pre-authentication stage of a server connection: X.224 negotiation,
/// the switch to TLS and, for HYBRID and HYBRID_EX, the CredSSP handshake
/// with the Early User Authorization Result ([MS-RDPBCGR] 1.3.1.1 and 5.4).
/// This is everything a privilege-separated server runs in its sandboxed
/// network process before anyone is authenticated (docs/PLAN.md §6).
///
/// Caller contract, as for `Connection`: after every call, send
/// `take_output()` (raw before `StartTls`, through TLS after), then handle
/// the events. On `Ready`, build the `Connection` from `negotiation()` and
/// feed it `take_remaining_input()`.
namespace farland::server {

/// What pre-authentication settled; the input to `Connection`.
struct Negotiation {
    std::string cookie;  ///< X.224 mstshash user name hint, if any
    std::uint32_t requested_protocols = 0;
    std::uint32_t selected_protocol = 0;
    /// The NLA-authenticated user; empty for TLS-only connections.
    std::optional<auth::Identity> identity;
};

struct PreAuthConfig {
    /// Protocols farland may select; NLA ones only with an NLA factory.
    std::uint32_t supported_protocols = proto::protocol::ssl | proto::protocol::hybrid | proto::protocol::hybrid_ex;
    /// Refuse clients that cannot do NLA (HYBRID_REQUIRED_BY_SERVER).
    bool require_nla = true;
    /// Set DYNVC_GFX_PROTOCOL_SUPPORTED in the negotiation response
    /// ([MS-RDPBCGR] 2.2.1.2.1): the server can run the Graphics Pipeline.
    bool advertise_gfx = false;
};

/// Early User Authorization Result values, [MS-RDPBCGR] 2.2.10.2.
namespace early_auth {
inline constexpr std::uint32_t success = 0x00000000;
inline constexpr std::uint32_t access_denied = 0x00000005;
}  // namespace early_auth

namespace preauth_event {
/// The Connection Confirm is queued: flush output, then run the TLS server
/// handshake and call `tls_established()`.
struct StartTls {};
/// NLA succeeded. Credentials are present if the client delegated a password.
struct Authenticated {
    auth::Identity identity;
    std::optional<auth::PasswordCredentials> credentials;
};
/// Pre-authentication is complete.
struct Ready {};
/// The connection must be closed (after flushing output: it may hold an
/// RDP_NEG_FAILURE or a CredSSP error code).
struct Failed {
    std::string reason;
};
}  // namespace preauth_event

using PreAuthEvent =
    std::variant<preauth_event::StartTls, preauth_event::Authenticated, preauth_event::Ready, preauth_event::Failed>;

class PreAuth {
public:
    enum class State : std::uint8_t { wait_connection_request, wait_tls, nla, ready, failed };
    using NlaFactory = std::function<std::unique_ptr<auth::NlaAcceptor>()>;

    /// Without an NLA factory, only TLS can be selected.
    explicit PreAuth(PreAuthConfig config = {}, NlaFactory make_nla = nullptr);

    /// Client bytes: plaintext before TLS, decrypted TLS data after.
    void receive(std::span<const std::byte> bytes);
    /// The TLS handshake requested by `StartTls` has completed.
    void tls_established();

    [[nodiscard]] std::vector<std::byte> take_output();
    [[nodiscard]] std::optional<PreAuthEvent> poll_event();
    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] const Negotiation& negotiation() const noexcept { return negotiation_; }
    /// Client bytes that followed pre-authentication: the start of the MCS
    /// stream, for `Connection::receive`.
    [[nodiscard]] std::vector<std::byte> take_remaining_input();

private:
    Result<void> on_connection_request(std::span<const std::byte> packet);
    void pump_nla();
    void finish();
    void fail(std::string reason);

    PreAuthConfig config_;
    NlaFactory make_nla_;
    State state_ = State::wait_connection_request;
    Negotiation negotiation_;
    std::unique_ptr<auth::NlaAcceptor> nla_;
    std::vector<std::byte> input_;
    Writer output_;
    std::deque<PreAuthEvent> events_;
};

}  // namespace farland::server
