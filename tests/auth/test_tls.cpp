// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/openssl.hpp>
#include <farland/auth/tls.hpp>
#include <farland/auth/tls_identity.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>
#include <openssl/err.h>

#include <algorithm>
#include <string>
#include <vector>

using farland::auth::TlsClient;
using farland::auth::TlsClientOptions;
using farland::auth::TlsIdentity;
using farland::auth::TlsServer;
using farland::auth::TlsVersion;
using farland::test::ascii;
namespace ossl = farland::auth::ossl;

namespace {

const TlsIdentity& identity()
{
    static const TlsIdentity instance = TlsIdentity::generate("farland-tls-test").value();
    return instance;
}

/// Both ends of a connection plus the plaintext each has received.
struct Link {
    TlsClient client;
    TlsServer server{identity()};
    std::vector<std::byte> client_received;
    std::vector<std::byte> server_received;

    explicit Link(const TlsClientOptions& options = {}) : client(options) {}

    /// Moves bytes back and forth until neither side has anything to send.
    void pump()
    {
        for (int round = 0; round < 32; ++round) {
            REQUIRE(client.process(client_received).has_value());
            const auto to_server = client.take_ciphertext();
            server.receive(to_server);
            REQUIRE(server.process(server_received).has_value());
            const auto to_client = server.take_ciphertext();
            client.receive(to_client);
            if (to_server.empty() && to_client.empty()) {
                REQUIRE(client.process(client_received).has_value());
                return;
            }
        }
        FAIL("the connection did not settle");
    }
};

std::vector<std::byte> pattern(std::size_t size, unsigned seed)
{
    std::vector<std::byte> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        data[i] = static_cast<std::byte>((i * 31U + seed) & 0xffU);
    }
    return data;
}

std::vector<std::byte> bytes_of(std::string_view text)
{
    const auto view = ascii(text);
    return {view.begin(), view.end()};
}

/// Number of TLS records in a stream of whole records (RFC 8446 5.1).
std::size_t count_records(std::span<const std::byte> stream)
{
    std::size_t records = 0;
    std::size_t pos = 0;
    while (pos + 5 <= stream.size()) {
        const auto length =
            (std::to_integer<std::size_t>(stream[pos + 3]) << 8U) | std::to_integer<std::size_t>(stream[pos + 4]);
        pos += 5 + length;
        ++records;
    }
    CHECK(pos == stream.size());
    return records;
}

std::vector<std::byte> drain(BIO* bio)
{
    std::vector<std::byte> out(BIO_ctrl_pending(bio));
    if (!out.empty()) {
        REQUIRE(BIO_read(bio, out.data(), static_cast<int>(out.size())) == static_cast<int>(out.size()));
    }
    return out;
}

}  // namespace

TEST_CASE("Client and server handshake in memory and exchange data")
{
    Link link;
    CHECK_FALSE(link.client.handshake_complete());
    CHECK(link.client.protocol_version().empty());
    CHECK(link.client.peer_subject_public_key().empty());

    link.pump();
    REQUIRE(link.client.handshake_complete());
    REQUIRE(link.server.handshake_complete());
    CHECK(link.server.protocol_version() == "TLSv1.3");
    CHECK(link.client.protocol_version() == "TLSv1.3");
    CHECK_FALSE(link.server.cipher().empty());
    CHECK(link.server.cipher() == link.client.cipher());
    CHECK(link.server.last_error().empty());

    // The client sees exactly the bytes CredSSP will bind to on the server.
    CHECK(std::ranges::equal(link.client.peer_subject_public_key(), identity().subject_public_key()));
    CHECK(std::ranges::equal(link.client.peer_certificate_der(), identity().certificate_der()));

    link.client.send(ascii("hello server"));
    link.pump();
    CHECK(link.server_received == bytes_of("hello server"));

    link.server.send(ascii("hello client"));
    link.pump();
    CHECK(link.client_received == bytes_of("hello client"));

    SECTION("a 100 KB payload spans many records")
    {
        const auto big = pattern(100 * 1024, 7);
        link.server_received.clear();
        link.client.send(big);
        const auto ciphertext = link.client.take_ciphertext();
        CHECK(ciphertext.size() > big.size());
        CHECK(count_records(ciphertext) >= 7);
        link.server.receive(ciphertext);
        REQUIRE(link.server.process(link.server_received).has_value());
        CHECK(link.server_received == big);

        const auto reply = pattern(100 * 1024, 99);
        link.client_received.clear();
        link.server.send(reply);
        link.pump();
        CHECK(link.client_received == reply);
    }

    SECTION("close_notify in both directions")
    {
        link.client.close();
        link.pump();
        CHECK(link.server.peer_closed());
        CHECK_FALSE(link.client.peer_closed());

        link.server.close();
        link.pump();
        CHECK(link.client.peer_closed());
        CHECK_FALSE(link.server.failed());
        CHECK_FALSE(link.client.failed());
    }
}

TEST_CASE("TLS 1.2 is negotiated when the client offers nothing newer")
{
    Link link(TlsClientOptions{.max_version = TlsVersion::tls1_2, .server_name = "farland-tls-test"});
    link.pump();
    REQUIRE(link.server.handshake_complete());
    CHECK(link.server.protocol_version() == "TLSv1.2");
    CHECK(std::ranges::equal(link.client.peer_subject_public_key(), identity().subject_public_key()));

    link.client.send(ascii("ping"));
    link.pump();
    CHECK(link.server_received == bytes_of("ping"));
}

TEST_CASE("Data in the same flight as the client Finished is delivered with the handshake")
{
    // This is how CredSSP's first TSRequest usually arrives.
    Link link;
    REQUIRE(link.client.process(link.client_received).has_value());
    link.server.receive(link.client.take_ciphertext());
    REQUIRE(link.server.process(link.server_received).has_value());
    link.client.receive(link.server.take_ciphertext());
    REQUIRE(link.client.process(link.client_received).has_value());
    REQUIRE(link.client.handshake_complete());
    CHECK_FALSE(link.server.handshake_complete());

    link.client.send(ascii("TSRequest"));
    link.server.receive(link.client.take_ciphertext());
    REQUIRE(link.server.process(link.server_received).has_value());
    CHECK(link.server.handshake_complete());
    CHECK(link.server_received == bytes_of("TSRequest"));
}

TEST_CASE("Ciphertext may arrive one byte at a time")
{
    Link link;
    for (int round = 0; round < 8 && !link.server.handshake_complete(); ++round) {
        REQUIRE(link.client.process(link.client_received).has_value());
        for (const std::byte b : link.client.take_ciphertext()) {
            link.server.receive(std::span(&b, 1));
            REQUIRE(link.server.process(link.server_received).has_value());
        }
        for (const std::byte b : link.server.take_ciphertext()) {
            link.client.receive(std::span(&b, 1));
            REQUIRE(link.client.process(link.client_received).has_value());
        }
    }
    CHECK(link.server.handshake_complete());
    CHECK(link.client.handshake_complete());
}

TEST_CASE("Garbage ciphertext fails the server with an error")
{
    TlsServer server(identity());
    std::vector<std::byte> plaintext;
    REQUIRE(server.process(plaintext).has_value());  // nothing to do yet
    CHECK(server.take_ciphertext().empty());

    server.receive(ascii("GET / HTTP/1.1\r\nHost: farland\r\n\r\n"));
    const auto result = server.process(plaintext);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == farland::Errc::invalid_value);
    CHECK(result.error().what == "TLS handshake failed");
    CHECK_FALSE(server.last_error().empty());
    CHECK(server.failed());
    CHECK(plaintext.empty());

    // The connection stays dead.
    CHECK_FALSE(server.process(plaintext).has_value());
}

TEST_CASE("A tampered record after the handshake fails the connection")
{
    Link link;
    link.pump();
    link.client.send(ascii("secret"));
    auto ciphertext = link.client.take_ciphertext();
    REQUIRE(ciphertext.size() > 10);
    ciphertext.back() ^= std::byte{0x01};  // inside the AEAD tag
    link.server.receive(ciphertext);

    const auto result = link.server.process(link.server_received);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().what == "TLS connection failed");
    CHECK_FALSE(link.server.last_error().empty());
    CHECK(link.server_received.empty());
    CHECK_FALSE(link.server.take_ciphertext().empty());  // bad_record_mac alert
}

TEST_CASE("A client limited to TLS 1.1 is refused")
{
    // OpenSSL's own client, with the security level lowered so that it is
    // willing to offer TLS 1.1 at all.
    const ossl::SslCtx context(SSL_CTX_new(TLS_client_method()));
    REQUIRE(context);
    SSL_CTX_set_security_level(context.get(), 0);
    REQUIRE(SSL_CTX_set_cipher_list(context.get(), "DEFAULT:@SECLEVEL=0") == 1);
    REQUIRE(SSL_CTX_set_min_proto_version(context.get(), TLS1_1_VERSION) == 1);
    REQUIRE(SSL_CTX_set_max_proto_version(context.get(), TLS1_1_VERSION) == 1);
    const ossl::Ssl client(SSL_new(context.get()));
    REQUIRE(client);
    BIO* client_in = BIO_new(BIO_s_mem());
    BIO* client_out = BIO_new(BIO_s_mem());
    BIO_set_mem_eof_return(client_in, -1);
    SSL_set_bio(client.get(), client_in, client_out);
    SSL_set_connect_state(client.get());

    int rc = SSL_do_handshake(client.get());
    REQUIRE(SSL_get_error(client.get(), rc) == SSL_ERROR_WANT_READ);
    const auto client_hello = drain(client_out);
    REQUIRE_FALSE(client_hello.empty());

    TlsServer server(identity());
    std::vector<std::byte> plaintext;
    server.receive(client_hello);
    const auto result = server.process(plaintext);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().what == "TLS handshake failed");
    CHECK(server.last_error().find("unsupported protocol") != std::string::npos);
    CHECK_FALSE(server.handshake_complete());

    // The server answers with a protocol_version alert and the client gives up.
    const auto alert = server.take_ciphertext();
    REQUIRE_FALSE(alert.empty());
    REQUIRE(BIO_write(client_in, alert.data(), static_cast<int>(alert.size())) == static_cast<int>(alert.size()));
    rc = SSL_do_handshake(client.get());
    CHECK(rc <= 0);
    CHECK(SSL_get_error(client.get(), rc) == SSL_ERROR_SSL);
    ERR_clear_error();
}

TEST_CASE("close before the handshake sends nothing")
{
    TlsServer server(identity());
    server.close();
    CHECK(server.take_ciphertext().empty());
    CHECK_FALSE(server.peer_closed());
}
