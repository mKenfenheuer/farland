// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/auth/openssl.hpp>
#include <farland/auth/tls_identity.hpp>
#include <farland/base/error.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

/// Sans-IO TLS over OpenSSL memory BIOs. RDP switches to TLS directly after
/// the plaintext X.224 Connection Confirm on the same TCP connection
/// ([MS-RDPBCGR] 5.4.5.1); the caller owns the socket and moves bytes.
///
/// Poll loop, per readable/writable event:
///
///     tls.receive(bytes_from_socket);          // on POLLIN
///     if (auto r = tls.process(plaintext); !r) // log tls.last_error(), send
///         ...;                                 // take_ciphertext() (the alert), close
///     if (tls.handshake_complete()) tls.send(reply);
///     queue_for_socket(tls.take_ciphertext()); // after every process/send/close
///
/// Nothing ever blocks: receive() buffers, send() encrypts into an in-memory
/// buffer, take_ciphertext() drains everything pending. Every call that can
/// produce ciphertext must be followed by take_ciphertext().
///
/// Not thread-safe: one connection belongs to one thread at a time. Different
/// connections (also from the same TlsIdentity) may run on different threads.
namespace farland::auth {

/// Base of TlsServer and TlsClient: everything but construction.
class TlsConnection {
public:
    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;
    TlsConnection(TlsConnection&&) noexcept = default;
    TlsConnection& operator=(TlsConnection&&) noexcept = default;
    ~TlsConnection() = default;

    /// Buffers bytes received from the network. Does no TLS processing.
    void receive(std::span<const std::byte> ciphertext);

    /// Drives the handshake and decrypts every complete record buffered so
    /// far, appending the plaintext. Returns success while more input is
    /// needed. Fails on a fatal TLS error ("TLS handshake failed" or "TLS
    /// connection failed", details in last_error()); the connection is then
    /// dead, every later call fails the same way, and take_ciphertext() may
    /// still hold an alert for the peer.
    [[nodiscard]] Result<void> process(std::vector<std::byte>& plaintext);

    /// Encrypts application data. Must only be called once the handshake is
    /// complete and before close(). A write failure (which a peer cannot
    /// cause over memory BIOs) is reported by the next process().
    void send(std::span<const std::byte> plaintext);

    /// All ciphertext pending for the network: handshake messages, records,
    /// alerts and close_notify.
    [[nodiscard]] std::vector<std::byte> take_ciphertext();

    /// Queues close_notify. Without a completed handshake nothing is sent.
    /// process() keeps working, so the peer's close_notify can still arrive.
    void close();

    [[nodiscard]] bool handshake_complete() const noexcept { return handshake_complete_; }
    /// The peer sent close_notify. No more plaintext will arrive.
    [[nodiscard]] bool peer_closed() const noexcept { return peer_closed_; }
    /// process() has failed; the connection cannot be used any more.
    [[nodiscard]] bool failed() const noexcept { return error_.has_value(); }

    /// Negotiated protocol, such as "TLSv1.3". Empty before the handshake completes.
    [[nodiscard]] std::string protocol_version() const;
    /// Negotiated cipher suite (OpenSSL name). Empty before the handshake completes.
    [[nodiscard]] std::string cipher() const;
    /// OpenSSL's error queue at the first failure, for logs. Empty until then.
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

protected:
    enum class Role : std::uint8_t { server, client };

    /// Takes a fresh SSL object and attaches memory BIOs to it.
    TlsConnection(ossl::Ssl ssl, Role role);

    [[nodiscard]] std::span<const std::byte> peer_certificate_der_bytes() const noexcept { return peer_cert_der_; }
    [[nodiscard]] std::span<const std::byte> peer_subject_public_key_bytes() const noexcept { return peer_spk_; }

private:
    [[nodiscard]] Result<void> on_handshake_complete();
    /// Marks the connection dead and keeps OpenSSL's error text for last_error().
    void latch_failure(std::string_view what, int ssl_error);
    [[nodiscard]] Result<void> record_failure(std::string_view what, int ssl_error);

    ossl::Ssl ssl_;
    std::optional<Error> error_;
    std::string last_error_;
    std::vector<std::byte> peer_cert_der_;
    std::vector<std::byte> peer_spk_;
    bool handshake_complete_ = false;
    bool peer_closed_ = false;
    bool closed_ = false;
};

/// Server side of one connection. Sends no CertificateRequest.
class TlsServer : public TlsConnection {
public:
    /// The identity only needs to live through the constructor.
    explicit TlsServer(const TlsIdentity& identity);
};

enum class TlsVersion : std::uint8_t { tls1_2, tls1_3 };

struct TlsClientOptions {
    TlsVersion min_version = TlsVersion::tls1_2;
    TlsVersion max_version = TlsVersion::tls1_3;
    /// Sent as SNI when not empty.
    std::string server_name;
};

/// Client side of one connection. Accepts any server certificate for now:
/// verification against a trust store or a pinned fingerprint comes with the
/// client in phase 2. Call process() once to produce the ClientHello.
class TlsClient : public TlsConnection {
public:
    explicit TlsClient(const TlsClientOptions& options = {});

    /// DER certificate the server presented. Empty before the handshake completes.
    [[nodiscard]] std::span<const std::byte> peer_certificate_der() const noexcept
    {
        return peer_certificate_der_bytes();
    }
    /// subjectPublicKey BIT STRING contents of the server certificate, as
    /// TlsIdentity::subject_public_key(). Empty before the handshake completes.
    [[nodiscard]] std::span<const std::byte> peer_subject_public_key() const noexcept
    {
        return peer_subject_public_key_bytes();
    }
};

}  // namespace farland::auth
