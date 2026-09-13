// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/auth/openssl.hpp>
#include <farland/base/error.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace farland::auth {

class TlsServer;

/// The server's certificate and private key, plus the SSL_CTX that every
/// TlsServer built from it shares.
///
/// Generated certificates are self-signed RSA 2048 / SHA-256 X.509 v3, valid
/// from one hour ago for five years (clients pin them on first use), with
/// CN = subjectAltName DNS = hostname, critical basicConstraints CA:FALSE,
/// critical keyUsage digitalSignature + keyEncipherment, extendedKeyUsage
/// serverAuth (mstsc and Windows App fail with 0x907 without it) and a
/// subjectKeyIdentifier.
///
/// Server TLS settings: TLS 1.2 to 1.3, OpenSSL's default ciphers and security
/// level, no renegotiation, no compression, no session tickets or session
/// cache, and no client-certificate request.
///
/// Immutable after construction. One identity may serve any number of
/// connections on any number of threads; TlsServer takes its own reference
/// to the SSL_CTX, so the identity may be destroyed while servers still run.
class TlsIdentity {
public:
    /// Loads `cert_pem` and `key_pem` when both exist. When neither exists,
    /// generates a new identity for `hostname` and saves it (see `save`).
    /// Exactly one of the two existing is an error: overwriting it could
    /// silently replace a certificate clients have pinned.
    [[nodiscard]] static Result<TlsIdentity> load_or_create(const std::filesystem::path& cert_pem,
                                                            const std::filesystem::path& key_pem,
                                                            std::string_view hostname);

    /// Loads a PEM certificate and an unencrypted PEM private key and checks
    /// that they belong together.
    [[nodiscard]] static Result<TlsIdentity> load(const std::filesystem::path& cert_pem,
                                                  const std::filesystem::path& key_pem);

    /// Generates a new self-signed identity in memory. `hostname` becomes the
    /// CN and the DNS subjectAltName; it must be 1 to 64 characters from
    /// [A-Za-z0-9.-_].
    [[nodiscard]] static Result<TlsIdentity> generate(std::string_view hostname);

    /// Writes the key (mode 0600, PKCS#8 PEM) and then the certificate (mode
    /// 0644, PEM), each atomically through a temporary file and rename.
    /// Missing parent directories are created with mode 0700.
    [[nodiscard]] Result<void> save(const std::filesystem::path& cert_pem, const std::filesystem::path& key_pem) const;

    /// SHA-256 of the DER certificate as uppercase hex pairs separated by
    /// colons ("AB:CD:..."), the form clients show to users.
    [[nodiscard]] const std::string& sha256_fingerprint() const noexcept { return fingerprint_; }

    /// Contents of the certificate's subjectPublicKey BIT STRING. For RSA this
    /// is the DER PKCS#1 RSAPublicKey, which is what CredSSP binds to
    /// ([MS-CSSP] 3.1.5). Not the whole SubjectPublicKeyInfo: mstsc fails with
    /// 0x204 otherwise.
    [[nodiscard]] std::span<const std::byte> subject_public_key() const noexcept { return subject_public_key_; }

    [[nodiscard]] std::span<const std::byte> certificate_der() const noexcept { return certificate_der_; }

private:
    friend class TlsServer;

    TlsIdentity() = default;
    [[nodiscard]] static Result<TlsIdentity> from_parts(ossl::X509Ptr certificate, ossl::Pkey key);

    ossl::X509Ptr certificate_;
    ossl::Pkey key_;
    ossl::SslCtx server_context_;
    std::vector<std::byte> certificate_der_;
    std::vector<std::byte> subject_public_key_;
    std::string fingerprint_;
};

}  // namespace farland::auth
