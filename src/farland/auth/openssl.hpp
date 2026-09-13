// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

/// RAII handles and small helpers for OpenSSL 3. Used by farland-auth and its
/// tests; not part of the protocol-level API.
namespace farland::auth::ossl {

/// `std::unique_ptr` deleter that calls an OpenSSL `*_free` function.
template <auto Free>
struct Deleter {
    template <class T>
    void operator()(T* object) const noexcept
    {
        Free(object);
    }
};

using Bio = std::unique_ptr<BIO, Deleter<&BIO_free_all>>;
using Bignum = std::unique_ptr<BIGNUM, Deleter<&BN_free>>;
using Pkey = std::unique_ptr<EVP_PKEY, Deleter<&EVP_PKEY_free>>;
using PkeyCtx = std::unique_ptr<EVP_PKEY_CTX, Deleter<&EVP_PKEY_CTX_free>>;
using Ssl = std::unique_ptr<SSL, Deleter<&SSL_free>>;
using SslCtx = std::unique_ptr<SSL_CTX, Deleter<&SSL_CTX_free>>;
using X509Ptr = std::unique_ptr<X509, Deleter<&X509_free>>;
using X509Extension = std::unique_ptr<X509_EXTENSION, Deleter<&X509_EXTENSION_free>>;

/// Drains this thread's OpenSSL error queue into one line of text (entries
/// separated by "; "). Empty when the queue was empty.
[[nodiscard]] std::string take_errors();

/// DER encoding of a certificate.
[[nodiscard]] std::vector<std::byte> certificate_der(const X509& certificate);

/// Contents of the certificate's subjectPublicKey BIT STRING (RFC 5280
/// 4.1.2.7), without the unused-bits octet. For RSA keys this is the DER
/// PKCS#1 RSAPublicKey that CredSSP binds to ([MS-CSSP] 3.1.5).
[[nodiscard]] Result<std::vector<std::byte>> subject_public_key(const X509& certificate);

}  // namespace farland::auth::ossl
