// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/auth/gss.hpp>
#include <farland/auth/tls_identity.hpp>
#include <farland/server/preauth.hpp>

#include <string>

namespace farland::app {

/// Creates the CredSSP acceptor for each NLA connection: SPNEGO or raw NTLM,
/// bound to the certificate's public key, with NTLM channel bindings from the
/// certificate and delegated passwords checked against the authenticated
/// user. `identity` and `verifier` must outlive every acceptor made.
/// `hostname` names the server in the NTLM CHALLENGE.
[[nodiscard]] server::PreAuth::NlaFactory make_nla_factory(const auth::TlsIdentity& identity,
                                                           auth::NtlmVerifier& verifier, const std::string& hostname);

}  // namespace farland::app
