// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// End to end over a real socket: farland-server's session loop (TLS pump,
// connection state machine, test pattern, frame encoder) against a scripted
// client that does TLS, activates, decodes the planar frames and clicks.

#include <farland/auth/tls.hpp>
#include <farland/auth/tls_identity.hpp>
#include <farland/codec/planar.hpp>
#include <farland/proto/bitmap.hpp>
#include <farland/proto/fastpath.hpp>
#include <farland/proto/framing.hpp>
#include <farland/proto/mcs.hpp>
#include <farland/proto/share.hpp>
#include <farland/proto/x224.hpp>

#include "session.hpp"
#include "support/client_pdus.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <optional>
#include <poll.h>
#include <set>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace proto = farland::proto;
namespace mcs = farland::proto::mcs;
namespace client = farland::test::client;
using farland::Reader;
using Bytes = std::vector<std::byte>;

namespace {

constexpr std::uint16_t width = 320;
constexpr std::uint16_t height = 240;

#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;
#endif

/// The client end of the socket pair; TLS once `start_tls` has run.
class TestClient {
public:
    explicit TestClient(int fd) : fd_(fd) {}
    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;
    ~TestClient() { ::close(fd_); }

    void send(std::span<const std::byte> bytes)
    {
        if (tls_) {
            tls_->send(bytes);
            send_raw(tls_->take_ciphertext());
        } else {
            send_raw(bytes);
        }
    }

    void start_tls()
    {
        tls_.emplace();
        pump_tls();  // ClientHello
        while (!tls_->handshake_complete()) {
            read_more();
        }
    }

    /// The next complete PDU (plaintext).
    Bytes read_pdu()
    {
        for (;;) {
            const auto frame = proto::peek_frame(plain_);
            REQUIRE(frame.has_value());
            if (frame->has_value() && plain_.size() >= (*frame)->length) {
                const auto length = static_cast<std::ptrdiff_t>((*frame)->length);
                Bytes pdu(plain_.begin(), plain_.begin() + length);
                plain_.erase(plain_.begin(), plain_.begin() + length);
                return pdu;
            }
            read_more();
        }
    }

private:
    void send_raw(std::span<const std::byte> bytes)
    {
        while (!bytes.empty()) {
            const ssize_t n = ::send(fd_, bytes.data(), bytes.size(), send_flags);
            REQUIRE(n > 0);
            bytes = bytes.subspan(static_cast<std::size_t>(n));
        }
    }

    void read_more()
    {
        pollfd pfd{fd_, POLLIN, 0};
        REQUIRE(::poll(&pfd, 1, 5000) == 1);
        std::array<std::byte, 65536> buffer{};
        const ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);
        REQUIRE(n > 0);
        const auto received = std::span(buffer).first(static_cast<std::size_t>(n));
        if (tls_) {
            tls_->receive(received);
            pump_tls();
        } else {
            plain_.insert(plain_.end(), received.begin(), received.end());
        }
    }

    void pump_tls()
    {
        Bytes plaintext;
        REQUIRE(tls_->process(plaintext).has_value());
        plain_.insert(plain_.end(), plaintext.begin(), plaintext.end());
        send_raw(tls_->take_ciphertext());
    }

    int fd_;
    std::optional<farland::auth::TlsClient> tls_;
    Bytes plain_;
};

/// The payload of a server Send Data Indication.
Bytes indication(const Bytes& tpkt)
{
    Reader r(tpkt);
    Reader tpdu = proto::read_tpkt(r).value();
    Reader data = proto::decode_data_tpdu(tpdu).value();
    const auto pdu = mcs::decode_domain_pdu(data).value();
    const auto& send = std::get<mcs::SendDataIndication>(pdu);
    return {send.data.begin(), send.data.end()};
}

/// What the client has painted from the server's bitmap updates.
struct Canvas {
    std::vector<std::byte> pixels = std::vector<std::byte>(static_cast<std::size_t>(width) * height * 4);
    std::set<std::pair<std::uint16_t, std::uint16_t>> tiles_seen;
    proto::fastpath::Reassembler reassembler{std::size_t{1} << 20U};

    /// Applies a fast-path PDU; ignores everything else.
    void apply(const Bytes& pdu)
    {
        if (std::to_integer<unsigned>(pdu.at(0)) == 0x03) {
            return;
        }
        Reader r(pdu);
        for (const auto& fragment : proto::fastpath::decode_output_pdu(r).value()) {
            auto update = reassembler.add(fragment).value();
            if (!update || update->code != proto::fastpath::update_code::bitmap) {
                continue;
            }
            Reader u(update->data);
            for (const auto& rect : proto::decode_bitmap_update(u).value()) {
                std::vector<std::byte> decoded(static_cast<std::size_t>(rect.width) * rect.height * 4);
                REQUIRE(farland::codec::planar::decode(rect.data, rect.width, rect.height,
                                                       farland::codec::planar::Orientation::bottom_up, decoded)
                            .has_value());
                for (std::uint32_t y = rect.dest_top; y <= rect.dest_bottom; ++y) {
                    for (std::uint32_t x = rect.dest_left; x <= rect.dest_right; ++x) {
                        const auto src =
                            ((static_cast<std::size_t>(y - rect.dest_top) * rect.width) + (x - rect.dest_left)) * 4;
                        const auto dst = ((static_cast<std::size_t>(y) * width) + x) * 4;
                        std::copy_n(decoded.begin() + static_cast<std::ptrdiff_t>(src), 4,
                                    pixels.begin() + static_cast<std::ptrdiff_t>(dst));
                    }
                }
                tiles_seen.emplace(rect.dest_left / 64, rect.dest_top / 64);
            }
        }
    }

    /// The pixel at (x, y) as 0xRRGGBB.
    [[nodiscard]] std::uint32_t rgb(std::uint32_t x, std::uint32_t y) const
    {
        const auto at = ((static_cast<std::size_t>(y) * width) + x) * 4;
        return std::to_integer<std::uint32_t>(pixels.at(at)) |
               (std::to_integer<std::uint32_t>(pixels.at(at + 1)) << 8U) |
               (std::to_integer<std::uint32_t>(pixels.at(at + 2)) << 16U);
    }
};

}  // namespace

TEST_CASE("End to end: TLS, activation, planar frames and input over a socket")
{
    std::array<int, 2> fds{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
    const auto identity = farland::auth::TlsIdentity::generate("e2e.farland.test").value();
    std::atomic<bool> stop{false};
    farland::app::SessionOptions options;
    options.frames_per_second = 60;
    std::thread server([&] { farland::app::run_session(fds[0], "e2e", identity, options, stop); });

    {
        TestClient c(fds[1]);

        // X.224 in plaintext, then TLS on the same stream.
        c.send(client::connection_request(proto::protocol::ssl));
        {
            const auto cc = c.read_pdu();
            Reader r(cc);
            Reader tpdu = proto::read_tpkt(r).value();
            const auto confirm = proto::decode_connection_confirm(tpdu).value();
            REQUIRE(std::get<proto::NegotiationResponse>(confirm.result).selected_protocol == proto::protocol::ssl);
        }
        c.start_tls();

        // MCS connect and domain setup.
        c.send(client::connect_initial(client::client_data(proto::protocol::ssl, 0x0001, width, height)));
        static_cast<void>(c.read_pdu());  // Connect Response
        c.send(client::erect_domain());
        c.send(client::attach_user());
        std::uint16_t user = 0;
        {
            const auto confirm = c.read_pdu();
            Reader r(confirm);
            Reader tpdu = proto::read_tpkt(r).value();
            Reader data = proto::decode_data_tpdu(tpdu).value();
            user = std::get<mcs::AttachUserConfirm>(mcs::decode_domain_pdu(data).value()).initiator.value();
        }
        for (const std::uint16_t channel : {user, mcs::io_channel_id}) {
            c.send(client::channel_join(user, channel));
            static_cast<void>(c.read_pdu());
        }

        // Client Info, license, capabilities, finalization.
        c.send(client::client_info(user, "e2e"));
        static_cast<void>(c.read_pdu());  // license
        std::uint32_t share_id = 0;
        {
            const auto payload = indication(c.read_pdu());
            Reader r(payload);
            auto control = proto::read_share_control(r).value();
            share_id = proto::decode_demand_active(control.body).value().share_id;
        }
        c.send(client::confirm_active(user, share_id, true, width, height));
        c.send(client::finalization(user, share_id));
        for (int i = 0; i < 4; ++i) {
            static_cast<void>(c.read_pdu());  // Synchronize, Cooperate, Granted, Font Map
        }

        // The first frames cover the whole desktop: 5 x 4 tiles.
        Canvas canvas;
        for (int i = 0; i < 400 && canvas.tiles_seen.size() < 20; ++i) {
            canvas.apply(c.read_pdu());
        }
        REQUIRE(canvas.tiles_seen.size() == 20);
        CHECK(canvas.rgb(5, 5) == 0xFFFFFF);    // first color bar
        CHECK(canvas.rgb(315, 5) == 0x000000);  // last color bar

        // Press the left button at (160, 200): the crosshair turns red.
        const std::array input{
            proto::InputEvent{proto::MouseEvent{proto::ptr_flags::move, 160, 200}},
            proto::InputEvent{proto::MouseEvent{proto::ptr_flags::down | proto::ptr_flags::button1, 160, 200}},
        };
        c.send(client::fastpath_input(input));
        for (int i = 0; i < 400 && canvas.rgb(170, 200) != 0xFF2020; ++i) {
            canvas.apply(c.read_pdu());
        }
        CHECK(canvas.rgb(170, 200) == 0xFF2020);

        // Server shutdown: Set Error Info, then the Disconnect Provider Ultimatum.
        stop = true;
        bool ultimatum = false;
        for (int i = 0; i < 400 && !ultimatum; ++i) {
            const auto pdu = c.read_pdu();
            if (std::to_integer<unsigned>(pdu.at(0)) != 0x03) {
                continue;
            }
            Reader r(pdu);
            Reader tpdu = proto::read_tpkt(r).value();
            Reader data = proto::decode_data_tpdu(tpdu).value();
            ultimatum = std::holds_alternative<mcs::DisconnectProviderUltimatum>(mcs::decode_domain_pdu(data).value());
        }
        CHECK(ultimatum);
    }
    server.join();
}
