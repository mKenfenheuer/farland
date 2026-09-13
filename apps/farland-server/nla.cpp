// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "nla.hpp"

#include <farland/auth/credssp.hpp>
#include <farland/auth/ntlm.hpp>
#include <farland/auth/spnego.hpp>
#include <farland/base/log.hpp>

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

/// NetBIOS computer name: the first label of the host name, upper case, at
/// most 15 characters.
std::string netbios_name(const std::string& hostname)
{
    std::string name = hostname.substr(0, std::min(hostname.find('.'), std::size_t{15}));
    std::ranges::transform(name, name.begin(), ascii_upper);
    return name.empty() ? "FARLAND" : name;
}

}  // namespace

server::PreAuth::NlaFactory make_nla_factory(const auth::TlsIdentity& identity, auth::NtlmVerifier& verifier,
                                             const std::string& hostname)
{
    const auto bindings = auth::ntlm::channel_bindings_hash(identity.certificate_der());
    return [&identity, &verifier, bindings, computer = netbios_name(hostname),
            dns = hostname]() -> std::unique_ptr<auth::NlaAcceptor> {
        auth::credssp::AcceptorConfig config;
        config.server_public_key = identity.subject_public_key();
        config.make_mechanism = [&verifier, bindings, computer,
                                 dns](std::span<const std::byte> mech_oid) -> std::unique_ptr<auth::SecurityContext> {
            if (!std::ranges::equal(mech_oid, auth::ntlm::mechanism_oid())) {
                return nullptr;  // Kerberos arrives with M7
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
        // Delegated credentials must belong to the user NTLM authenticated:
        // the same name, and a password that verifies for it.
        config.accept_credentials = [&verifier](const auth::PasswordCredentials& credentials,
                                                const auth::Identity& user) {
            if (!iequals(credentials.user, user.user)) {
                log::warn(log_component, "delegated credentials name '{}', but NTLM authenticated '{}'",
                          credentials.user, user.user);
                return false;
            }
            return verifier.verify_password(user.user, user.domain, credentials.password.view());
        };
        return std::make_unique<auth::credssp::Acceptor>(std::move(config));
    };
}

}  // namespace farland::app
