// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/auth/gss.hpp>
#include <farland/base/error.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

/// Kerberos V5 as a CredSSP mechanism (RFC 4121, [MS-SPNG] 1.9), over the
/// GSS-API of MIT krb5. It is the acceptor side only: farland is a server,
/// and a client that has a ticket for it proves so with an AP-REQ.
///
/// **Where this runs.** The keytab is the host's long-term key, so none of
/// this belongs in the sandboxed network process -- which could not read it
/// anyway, since that process keeps no file system access at all
/// (apps/farland-server/sandbox.hpp). The monitor holds the credential and
/// the context; the network process drives them over the control channel
/// (server/privsep.hpp), the way it already asks the monitor to check NTLM
/// responses instead of holding NT hashes.
///
/// Built only where MIT krb5 is (`-Dkerberos`); `available()` says so at run
/// time, and `make_acceptor()` returns nullptr when it is not.
namespace farland::auth::kerberos {

/// Whether this build can accept Kerberos.
[[nodiscard]] bool available() noexcept;

struct Config {
    /// The keytab to accept with. Empty uses the system default, which is
    /// what `KRB5_KTNAME` or /etc/krb5.keytab names.
    std::string keytab;
    /// The service principal to accept as, such as
    /// `TERMSRV/host.example.com` or `host/host.example.com`. Empty accepts
    /// any principal the keytab holds, which is what a host with one
    /// service wants.
    std::string service_principal;
};

/// The acceptor credential, acquired once from the keytab and shared by
/// every connection. Acquiring it reads the keytab and `krb5.conf`, so it
/// happens in the monitor, before any client is served.
class Credential {
public:
    /// A credential that accepts nothing: what a host without a keytab has.
    /// Defined out of line, because Impl is only complete there.
    Credential();
    Credential(const Credential&) = delete;
    Credential& operator=(const Credential&) = delete;
    Credential(Credential&&) noexcept;
    Credential& operator=(Credential&&) noexcept;
    ~Credential();

    /// Acquires the credential. Fails when the build has no Kerberos, the
    /// keytab is missing or unreadable, or it holds no key for the
    /// principal -- all of which an administrator wants to hear at startup
    /// rather than on the first connection.
    [[nodiscard]] static Result<Credential> acquire(const Config& config);

    /// The principal the credential accepts as, for the log.
    [[nodiscard]] const std::string& principal() const noexcept { return principal_; }

    /// A new acceptor context. `mechanism_oid` is the OID SPNEGO settled on
    /// -- the Kerberos OID or the legacy Microsoft one, which the acceptor
    /// must echo back unchanged ([MS-SPNG] 3.1.5.2) even though both mean
    /// the same mechanism.
    [[nodiscard]] std::unique_ptr<SecurityContext> accept(std::span<const std::byte> mechanism_oid) const;

    struct Impl;

private:
    explicit Credential(std::unique_ptr<Impl> impl, std::string principal);

    std::unique_ptr<Impl> impl_;
    std::string principal_;
};

/// Splits a Kerberos principal into the identity CredSSP reports: `user`
/// from the first component and `domain` from the realm, both as the KDC
/// spelled them. `alice/admin@EXAMPLE.COM` keeps its instance in `user`,
/// because an administrator who maps it to a local account should see the
/// whole name. A principal without a realm gets an empty domain.
[[nodiscard]] Identity identity_from_principal(std::string_view principal);

}  // namespace farland::auth::kerberos
