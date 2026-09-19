// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "nla.hpp"

#include <farland/auth/credssp.hpp>
#include <farland/auth/kerberos.hpp>
#include <farland/auth/ntlm.hpp>
#include <farland/auth/spnego.hpp>
#include <farland/base/log.hpp>
#include <farland/server/privsep.hpp>

#include <algorithm>
#include <chrono>

namespace farland::app {

namespace {

constexpr std::string_view log_component = "app.nla";

/// The current time as a FILETIME (100 ns since 1601), for MsvAvTimestamp.
/// The auth library reads no clocks; the application passes the time in.
std::uint64_t filetime_now()
{
    using Ticks = std::chrono::duration<std::uint64_t, std::ratio<1, 10'000'000>>;
    constexpr std::uint64_t unix_epoch_in_filetime = 116'444'736'000'000'000ULL;
    const auto since_unix = std::chrono::duration_cast<Ticks>(std::chrono::system_clock::now().time_since_epoch());
    return unix_epoch_in_filetime + since_unix.count();
}

char ascii_upper(char c)
{
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

bool iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size() &&
           std::ranges::equal(a, b, [](char x, char y) { return ascii_upper(x) == ascii_upper(y); });
}

/// Both Kerberos OIDs name the same mechanism; Windows lists the legacy
/// Microsoft one first ([MS-SPNG] 3.1.5.2).
bool is_kerberos(std::span<const std::byte> oid)
{
    return std::ranges::equal(oid, auth::spnego::kerberos_oid) ||
           std::ranges::equal(oid, auth::spnego::ms_kerberos_oid);
}

/// NetBIOS computer name: the first label of the host name, upper case, at
/// most 15 characters.
std::string netbios_name(const std::string& hostname)
{
    std::string name = hostname.substr(0, std::min(hostname.find('.'), std::size_t{15}));
    std::ranges::transform(name, name.begin(), ascii_upper);
    return name.empty() ? "FARLAND" : name;
}

}  // namespace

server::PreAuth::NlaFactory make_nla_factory(const auth::TlsIdentity& identity, const NlaBackends& backends,
                                             const std::string& hostname)
{
    const auto bindings = auth::ntlm::channel_bindings_hash(identity.certificate_der());
    auto& verifier = backends.verifier;
    const bool kerberos = backends.kerberos || backends.credential != nullptr;
    const bool kerberos_only = backends.kerberos_only;
    return [&identity, &verifier, bindings, kerberos, kerberos_only, monitor = backends.monitor,
            credential = backends.credential, computer = netbios_name(hostname),
            dns = hostname]() -> std::unique_ptr<auth::NlaAcceptor> {
        auth::credssp::AcceptorConfig config;
        config.server_public_key = identity.subject_public_key();
        config.make_mechanism = [&verifier, bindings, kerberos, kerberos_only, monitor, credential, computer,
                                 dns](std::span<const std::byte> mech_oid) -> std::unique_ptr<auth::SecurityContext> {
            if (kerberos && is_kerberos(mech_oid)) {
                // With privilege separation the keytab and the context are
                // the monitor's; without it there is no sandbox and the
                // credential is right here.
                if (monitor) {
                    return std::make_unique<server::privsep::RemoteKerberos>(monitor, mech_oid);
                }
                return credential != nullptr ? credential->accept(mech_oid) : nullptr;
            }
            if (kerberos_only) {
                // SPNEGO then finds no mechanism it and farland share, and
                // the handshake ends without a login.
                log::info(log_component, "a client offered no Kerberos ticket, and nothing else is accepted");
                return nullptr;
            }
            if (!std::ranges::equal(mech_oid, auth::ntlm::mechanism_oid())) {
                return nullptr;
            }
            return std::make_unique<auth::ntlm::Acceptor>(auth::ntlm::AcceptorConfig{
                .verifier = verifier,
                .netbios_domain = "WORKGROUP",
                .netbios_computer = computer,
                .dns_domain = {},
                .dns_computer = dns,
                .channel_bindings = bindings,
                .channel_binding_policy = auth::ntlm::ChannelBindingPolicy::verify_if_present,
                .server_challenge = std::nullopt,
                .timestamp = filetime_now(),
            });
        };
        // Delegated credentials must belong to the user the mechanism
        // authenticated. NTLM shares a password hash with the server, so
        // the password is checked too. Kerberos shares nothing of the kind:
        // the ticket is the proof, the server has no hash to test a
        // password against, and a client with single sign-on delegates no
        // password at all. There the name is what can be checked, and it is
        // checked.
        config.accept_credentials = [&verifier](const auth::PasswordCredentials& credentials,
                                                const auth::Identity& user, std::span<const std::byte> mech_oid) {
            if (!iequals(credentials.user, user.user)) {
                log::warn(log_component, "delegated credentials name '{}', but the client authenticated as '{}'",
                          credentials.user, user.user);
                return false;
            }
            if (is_kerberos(mech_oid)) {
                return true;
            }
            return verifier.verify_password(user.user, user.domain, credentials.password.view());
        };
        return std::make_unique<auth::credssp::Acceptor>(std::move(config));
    };
}

}  // namespace farland::app
