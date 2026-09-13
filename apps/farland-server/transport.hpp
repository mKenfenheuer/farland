// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/auth/tls.hpp>
#include <farland/auth/tls_identity.hpp>
#include <farland/server/preauth.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

/// Byte transports under a session: the client's socket with TLS and
/// pre-authentication, or a plaintext socket whose peer already did both.
namespace farland::app {

/// Writes everything to a (possibly non-blocking) socket, waiting for it to
/// drain. False on error or a vanished peer; never raises SIGPIPE.
[[nodiscard]] bool send_all(int fd, std::span<const std::byte> data);
/// EAGAIN or EWOULDBLOCK.
[[nodiscard]] bool would_block(int error) noexcept;
/// Marks `fd` close-on-exec and, where the platform needs it, SO_NOSIGPIPE.
void prepare_socket(int fd);

/// What a session reads from and writes to.
class Transport {
public:
    Transport() = default;
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;
    virtual ~Transport() = default;

    /// The descriptor to poll for readability.
    [[nodiscard]] virtual int fd() const noexcept = 0;
    /// Reads what is available, appending application data (the RDP stream
    /// after pre-authentication). False when the connection is over.
    [[nodiscard]] virtual bool read(std::vector<std::byte>& data) = 0;
    /// Sends application data. Only once ready(). False on error.
    [[nodiscard]] virtual bool send(std::span<const std::byte> data) = 0;
    /// Pre-authentication is complete and negotiation() is final.
    [[nodiscard]] virtual bool ready() const noexcept = 0;
    [[nodiscard]] virtual const server::Negotiation& negotiation() const = 0;
    /// Ends the connection gracefully (TLS close_notify) and closes the socket.
    virtual void close() = 0;
};

/// The client's socket: X.224 negotiation, TLS and NLA by `server::PreAuth`,
/// then TLS-protected application data.
class NetworkStage final : public Transport {
public:
    /// Takes ownership of `fd`. `identity` must outlive the stage.
    NetworkStage(int fd, std::string peer, const auth::TlsIdentity& identity, const server::PreAuthConfig& config,
                 server::PreAuth::NlaFactory make_nla);
    NetworkStage(const NetworkStage&) = delete;
    NetworkStage& operator=(const NetworkStage&) = delete;
    NetworkStage(NetworkStage&&) = delete;
    NetworkStage& operator=(NetworkStage&&) = delete;
    ~NetworkStage() override;

    [[nodiscard]] int fd() const noexcept override { return fd_; }
    [[nodiscard]] bool read(std::vector<std::byte>& data) override;
    [[nodiscard]] bool send(std::span<const std::byte> data) override;
    [[nodiscard]] bool ready() const noexcept override { return ready_; }
    [[nodiscard]] const server::Negotiation& negotiation() const override { return preauth_.negotiation(); }
    void close() override;

private:
    /// Sends PreAuth output, then handles its events. False on failure.
    [[nodiscard]] bool pump(std::vector<std::byte>& data);

    int fd_;
    std::string peer_;
    const auth::TlsIdentity& identity_;
    server::PreAuth preauth_;
    std::optional<auth::TlsServer> tls_;
    std::vector<std::byte> buffer_;
    bool tls_notified_ = false;
    bool ready_ = false;
};

/// A plaintext socket carrying the RDP stream of an already pre-authenticated
/// connection (from the privilege-separated network process).
class PlainTransport final : public Transport {
public:
    /// Takes ownership of `fd`.
    PlainTransport(int fd, server::Negotiation negotiation);
    PlainTransport(const PlainTransport&) = delete;
    PlainTransport& operator=(const PlainTransport&) = delete;
    PlainTransport(PlainTransport&&) = delete;
    PlainTransport& operator=(PlainTransport&&) = delete;
    ~PlainTransport() override;

    [[nodiscard]] int fd() const noexcept override { return fd_; }
    [[nodiscard]] bool read(std::vector<std::byte>& data) override;
    [[nodiscard]] bool send(std::span<const std::byte> data) override;
    [[nodiscard]] bool ready() const noexcept override { return true; }
    [[nodiscard]] const server::Negotiation& negotiation() const override { return negotiation_; }
    void close() override;

private:
    int fd_;
    server::Negotiation negotiation_;
    std::vector<std::byte> buffer_;
};

}  // namespace farland::app
