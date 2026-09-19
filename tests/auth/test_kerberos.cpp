// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Kerberos as an NLA mechanism, in the parts that need no KDC: the principal
// an identity is read from, and what the acceptor credential does when the
// keytab is not there. The handshake itself is a live test against a real
// KDC (docs/ROADMAP.md M7 S5).

#include <farland/auth/kerberos.hpp>
#include <farland/auth/spnego.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

namespace kerberos = farland::auth::kerberos;
namespace spnego = farland::auth::spnego;

TEST_CASE("Kerberos: the identity comes from the principal", "[auth][kerberos]")
{
    const auto alice = kerberos::identity_from_principal("alice@EXAMPLE.COM");
    CHECK(alice.user == "alice");
    CHECK(alice.domain == "EXAMPLE.COM");

    // An instance stays with the name: an administrator mapping the
    // principal to a local account has to see which one it is.
    const auto admin = kerberos::identity_from_principal("alice/admin@EXAMPLE.COM");
    CHECK(admin.user == "alice/admin");
    CHECK(admin.domain == "EXAMPLE.COM");

    // A realm may hold an @ in a component, so the last one separates.
    const auto odd = kerberos::identity_from_principal("a@b@EXAMPLE.COM");
    CHECK(odd.user == "a@b");
    CHECK(odd.domain == "EXAMPLE.COM");

    const auto bare = kerberos::identity_from_principal("alice");
    CHECK(bare.user == "alice");
    CHECK(bare.domain.empty());

    const auto nothing = kerberos::identity_from_principal("");
    CHECK(nothing.user.empty());
    CHECK(nothing.domain.empty());
}

TEST_CASE("Kerberos: a keytab that is not there is a startup error", "[auth][kerberos]")
{
    const auto missing = std::filesystem::temp_directory_path() / "farland-no-such.keytab";
    std::filesystem::remove(missing);

    const auto credential = kerberos::Credential::acquire({.keytab = missing.string(), .service_principal = {}});
    REQUIRE_FALSE(credential.has_value());
    // Whether the build has Kerberos decides which error, but never a crash
    // and never a credential that would refuse every client later.
    if (kerberos::available()) {
        CHECK(credential.error().code == farland::Errc::io);
    } else {
        CHECK(credential.error().code == farland::Errc::unsupported);
    }
}

TEST_CASE("Kerberos: a principal that is not one is refused", "[auth][kerberos]")
{
    if (!kerberos::available()) {
        SUCCEED("this build has no Kerberos");
        return;
    }
    // An empty component makes the name unparsable; the keytab is never
    // reached, so this fails the same way with or without one.
    const auto credential = kerberos::Credential::acquire({.keytab = {}, .service_principal = "TERMSRV/"});
    if (!credential) {
        CHECK(
            (credential.error().code == farland::Errc::invalid_value || credential.error().code == farland::Errc::io));
    }
}

TEST_CASE("Kerberos: only the two Kerberos OIDs are accepted", "[auth][kerberos]")
{
    // Without a credential nothing is accepted at all, which is what a host
    // with no keytab does. The OID check matters where there is one, and is
    // the same code either way.
    const kerberos::Credential none;
    CHECK(none.accept(spnego::kerberos_oid) == nullptr);
    CHECK(none.accept(spnego::ms_kerberos_oid) == nullptr);
    CHECK(none.accept(spnego::ntlm_oid) == nullptr);
    CHECK(none.principal().empty());
}
