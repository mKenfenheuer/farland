// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/tls.hpp>
#include <farland/base/assert.hpp>

#include <openssl/err.h>

#include <format>
#include <utility>

namespace farland::auth {

namespace {

/// Plaintext is decrypted in steps of at most one TLS record.
constexpr std::size_t read_chunk = 16384;

int to_openssl(TlsVersion version)
{
    return version == TlsVersion::tls1_2 ? TLS1_2_VERSION : TLS1_3_VERSION;
}

ossl::Ssl new_server_ssl(SSL_CTX* context)
{
    FARLAND_ASSERT(context != nullptr);
    return ossl::Ssl(SSL_new(context));
}

ossl::Ssl new_client_ssl(const TlsClientOptions& options)
{
    FARLAND_ASSERT(options.min_version <= options.max_version);
    const ossl::SslCtx context(SSL_CTX_new(TLS_client_method()));
    FARLAND_ASSERT(context != nullptr);
    SSL_CTX* ctx = context.get();
    SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION | SSL_OP_NO_TICKET);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    FARLAND_ASSERT(SSL_CTX_set_min_proto_version(ctx, to_openssl(options.min_version)) == 1);
    FARLAND_ASSERT(SSL_CTX_set_max_proto_version(ctx, to_openssl(options.max_version)) == 1);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    // The SSL object keeps its own reference to the context.
    ossl::Ssl ssl(SSL_new(ctx));
    FARLAND_ASSERT(ssl != nullptr);
    if (!options.server_name.empty()) {
        // SSL_set_tlsext_host_name is a macro around SSL_ctrl with a C-style
        // cast; calling SSL_ctrl directly keeps -Wold-style-cast quiet. OpenSSL
        // copies the name and never writes through the pointer.
        const long set =
            SSL_ctrl(ssl.get(), SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name,
                     const_cast<char*>(options.server_name.c_str()));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
        FARLAND_ASSERT(set == 1);
    }
    return ssl;
}

}  // namespace

// TlsConnection --------------------------------------------------------------

TlsConnection::TlsConnection(ossl::Ssl ssl, Role role) : ssl_(std::move(ssl))
{
    FARLAND_ASSERT(ssl_ != nullptr);
    ossl::Bio in(BIO_new(BIO_s_mem()));
    ossl::Bio out(BIO_new(BIO_s_mem()));
    FARLAND_ASSERT(in != nullptr && out != nullptr);
    // An empty input buffer means "wait for more", not end of file.
    BIO_set_mem_eof_return(in.get(), -1);
    SSL_set_bio(ssl_.get(), in.release(), out.release());
    if (role == Role::server) {
        SSL_set_accept_state(ssl_.get());
    } else {
        SSL_set_connect_state(ssl_.get());
    }
}

void TlsConnection::receive(std::span<const std::byte> ciphertext)
{
    FARLAND_ASSERT(ssl_ != nullptr);
    if (ciphertext.empty()) {
        return;
    }
    std::size_t written = 0;
    const int ok = BIO_write_ex(SSL_get_rbio(ssl_.get()), ciphertext.data(), ciphertext.size(), &written);
    FARLAND_ASSERT(ok == 1 && written == ciphertext.size());
}

Result<void> TlsConnection::process(std::vector<std::byte>& plaintext)
{
    FARLAND_ASSERT(ssl_ != nullptr);
    if (error_) {
        return std::unexpected(*error_);
    }

    if (!handshake_complete_) {
        ERR_clear_error();
        const int rc = SSL_do_handshake(ssl_.get());
        if (rc != 1) {
            const int ssl_error = SSL_get_error(ssl_.get(), rc);
            if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
                return {};
            }
            return record_failure("TLS handshake failed", ssl_error);
        }
        FARLAND_TRY_VOID(on_handshake_complete());
    }

    // Application data may already follow the peer's Finished message.
    for (;;) {
        const std::size_t old_size = plaintext.size();
        plaintext.resize(old_size + read_chunk);
        std::size_t read = 0;
        ERR_clear_error();
        const int rc = SSL_read_ex(ssl_.get(), std::span(plaintext).subspan(old_size).data(), read_chunk, &read);
        plaintext.resize(old_size + read);
        if (rc == 1) {
            continue;
        }
        const int ssl_error = SSL_get_error(ssl_.get(), rc);
        switch (ssl_error) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            return {};
        case SSL_ERROR_ZERO_RETURN:
            peer_closed_ = true;
            return {};
        default:
            return record_failure("TLS connection failed", ssl_error);
        }
    }
}

void TlsConnection::send(std::span<const std::byte> plaintext)
{
    FARLAND_ASSERT(handshake_complete_);
    FARLAND_ASSERT(!closed_);
    if (error_ || plaintext.empty()) {
        return;
    }
    // Without SSL_MODE_ENABLE_PARTIAL_WRITE, SSL_write_ex() encrypts all of
    // it, and a memory BIO always has room.
    std::size_t written = 0;
    ERR_clear_error();
    const int rc = SSL_write_ex(ssl_.get(), plaintext.data(), plaintext.size(), &written);
    if (rc != 1) {
        latch_failure("TLS write failed", SSL_get_error(ssl_.get(), rc));
        return;
    }
    FARLAND_ASSERT(written == plaintext.size());
}

std::vector<std::byte> TlsConnection::take_ciphertext()
{
    FARLAND_ASSERT(ssl_ != nullptr);
    BIO* out = SSL_get_wbio(ssl_.get());
    std::vector<std::byte> ciphertext;
    for (std::size_t pending = BIO_ctrl_pending(out); pending > 0; pending = BIO_ctrl_pending(out)) {
        const std::size_t old_size = ciphertext.size();
        ciphertext.resize(old_size + pending);
        std::size_t read = 0;
        const int ok = BIO_read_ex(out, std::span(ciphertext).subspan(old_size).data(), pending, &read);
        FARLAND_ASSERT(ok == 1 && read == pending);
    }
    return ciphertext;
}

void TlsConnection::close()
{
    FARLAND_ASSERT(ssl_ != nullptr);
    if (closed_) {
        return;
    }
    closed_ = true;
    if (!handshake_complete_ || error_) {
        return;
    }
    // Returns 0 (sent, peer's not received yet) or 1; both are fine here.
    ERR_clear_error();
    static_cast<void>(SSL_shutdown(ssl_.get()));
    ERR_clear_error();
}

std::string TlsConnection::protocol_version() const
{
    if (!handshake_complete_) {
        return {};
    }
    return SSL_get_version(ssl_.get());
}

std::string TlsConnection::cipher() const
{
    const SSL_CIPHER* current = handshake_complete_ ? SSL_get_current_cipher(ssl_.get()) : nullptr;
    if (current == nullptr) {
        return {};
    }
    return SSL_CIPHER_get_name(current);
}

Result<void> TlsConnection::on_handshake_complete()
{
    // Servers never request a client certificate, so this is the client's view.
    if (const X509* peer = SSL_get0_peer_certificate(ssl_.get()); peer != nullptr) {
        auto spk = ossl::subject_public_key(*peer);
        if (!spk) {
            last_error_ = std::format("peer certificate: {}", spk.error().message());
            const Error error{Errc::invalid_value, "TLS handshake failed"};
            error_ = error;
            return std::unexpected(error);
        }
        peer_spk_ = std::move(*spk);
        peer_cert_der_ = ossl::certificate_der(*peer);
    }
    handshake_complete_ = true;
    return {};
}

void TlsConnection::latch_failure(std::string_view what, int ssl_error)
{
    last_error_ = ossl::take_errors();
    if (last_error_.empty()) {
        last_error_ = std::format("SSL_get_error() = {}", ssl_error);
    }
    error_ = Error{Errc::invalid_value, what};
}

Result<void> TlsConnection::record_failure(std::string_view what, int ssl_error)
{
    latch_failure(what, ssl_error);
    return std::unexpected(Error{Errc::invalid_value, what});
}

// TlsServer / TlsClient --------------------------------------------------------

TlsServer::TlsServer(const TlsIdentity& identity)
    : TlsConnection(new_server_ssl(identity.server_context_.get()), Role::server)
{
}

TlsClient::TlsClient(const TlsClientOptions& options) : TlsConnection(new_client_ssl(options), Role::client) {}

}  // namespace farland::auth
