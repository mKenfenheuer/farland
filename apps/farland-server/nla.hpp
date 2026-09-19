// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/auth/gss.hpp>
#include <farland/auth/tls_identity.hpp>
#include <farland/server/preauth.hpp>

#include "privsep_process.hpp"

#include <string>

namespace farland::app {

/// Creates the CredSSP acceptor for each NLA connection: SPNEGO or raw NTLM,
/// bound to the certificate's public key, with NTLM channel bindings from the
/// certificate and delegated passwords checked against the authenticated
/// user. Where `backends` offers Kerberos, SPNEGO may settle on that
/// instead, and the context then lives in the monitor. `identity` and
/// everything `backends` refers to must outlive every acceptor made.
/// `hostname` names the server in the NTLM CHALLENGE.
[[nodiscard]] server::PreAuth::NlaFactory make_nla_factory(const auth::TlsIdentity& identity,
                                                           const NlaBackends& backends, const std::string& hostname);

}  // namespace farland::app
