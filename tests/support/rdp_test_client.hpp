// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/auth/tls.hpp>
#include <farland/proto/client_info.hpp>
#include <farland/proto/fastpath.hpp>
#include <farland/proto/save_session_info.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/// A scripted RDP client for end-to-end tests of multi-session (farlandd,
/// farland-agent): TCP or a socket pair, X.224 and TLS with NLA, or the
/// plaintext stream after them; activation with bitmap updates, planar
/// decoding into a canvas, input, and what the server says at the end.
namespace farland::test {

using Bytes = std::vector<std::byte>;

class RdpTestClient {
public:
    /// Takes ownership of a connected socket.
    explicit RdpTestClient(int fd);
    RdpTestClient(const RdpTestClient&) = delete;
    RdpTestClient& operator=(const RdpTestClient&) = delete;
    RdpTestClient(RdpTestClient&&) = delete;
    RdpTestClient& operator=(RdpTestClient&&) = delete;
    ~RdpTestClient();

    /// Connects over TCP to 127.0.0.1:`port`, retrying for up to `wait_ms`.
    [[nodiscard]] static int connect_tcp(std::uint16_t port, int wait_ms = 10'000);

    /// X.224 (HYBRID_EX), TLS and NLA with SPNEGO and NTLM. False when the
    /// server refused the login.
    [[nodiscard]] bool login(const std::string& user, const std::string& password);

    /// MCS, Client Info (with `cookie` as ARC_CS_PRIVATE_PACKET), licensing,
    /// capabilities and finalization at `width` x `height`. Returns the
    /// size the server announced.
    std::pair<std::uint16_t, std::uint16_t> activate(std::uint16_t width, std::uint16_t height,
                                                     const std::optional<proto::AutoReconnectCookie>& cookie = {});

    /// Reads server PDUs for up to `max_pdus`, painting bitmap updates, until
    /// `done()` holds. False when the server ended the connection first.
    template <class Done>
    bool pump_until(Done done, int max_pdus = 2000)
    {
        for (int i = 0; i < max_pdus && !done(); ++i) {
            if (!read_one()) {
                return done();
            }
        }
        return done();
    }

    void send_keys(std::initializer_list<std::uint16_t> scancodes);
    /// Closes the socket as a dropped network would.
    void drop();

    /// The canvas pixel at (x, y) as 0xRRGGBB.
    [[nodiscard]] std::uint32_t rgb(std::uint32_t x, std::uint32_t y) const;
    [[nodiscard]] std::size_t bitmap_updates() const noexcept { return bitmap_updates_; }
    /// The last Save Session Info cookie, the Set Error Info code, and
    /// whether the Disconnect Provider Ultimatum came.
    [[nodiscard]] const std::optional<proto::ServerAutoReconnectCookie>& arc_cookie() const noexcept { return arc_; }
    [[nodiscard]] std::optional<std::uint32_t> error_info() const noexcept { return error_info_; }
    [[nodiscard]] bool ended() const noexcept { return ended_; }
    [[nodiscard]] std::uint16_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint16_t height() const noexcept { return height_; }

private:
    void send(std::span<const std::byte> bytes);
    void send_raw(std::span<const std::byte> bytes);
    [[nodiscard]] bool read_more(int timeout_ms = 5000);
    void pump_tls();
    [[nodiscard]] std::optional<Bytes> read_pdu();
    [[nodiscard]] Bytes read_exact(std::size_t n);
    [[nodiscard]] bool read_one();
    void apply_fastpath(const Bytes& pdu);
    void apply_slow_path(const Bytes& pdu);

    int fd_;
    std::optional<auth::TlsClient> tls_;
    Bytes plain_;
    bool closed_ = false;
    std::uint16_t user_ = 0;
    std::uint32_t share_id_ = 0;
    std::uint16_t width_ = 0;
    std::uint16_t height_ = 0;
    std::vector<std::byte> pixels_;
    proto::fastpath::Reassembler reassembler_{std::size_t{1} << 20U};
    std::size_t bitmap_updates_ = 0;
    std::optional<proto::ServerAutoReconnectCookie> arc_;
    std::optional<std::uint32_t> error_info_;
    bool ended_ = false;
};

}  // namespace farland::test
