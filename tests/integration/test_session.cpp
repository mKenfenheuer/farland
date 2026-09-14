// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// End to end over a real socket: farland-server's session loop (TLS pump,
// connection state machine, test pattern, frame encoder) against a scripted
// client that does TLS, activates, decodes the planar frames and clicks. The
// same client runs against the in-process loop and the privilege-separated
// network process.

#include <farland/auth/credential_store.hpp>
#include <farland/auth/credssp.hpp>
#include <farland/auth/ntlm.hpp>
#include <farland/auth/tls.hpp>
#include <farland/auth/tls_identity.hpp>
#include <farland/codec/planar.hpp>
#include <farland/proto/bitmap.hpp>
#include <farland/proto/fastpath.hpp>
#include <farland/proto/framing.hpp>
#include <farland/proto/mcs.hpp>
#include <farland/proto/share.hpp>
#include <farland/proto/x224.hpp>

#include "desktop.hpp"
#include "nla.hpp"
#include "privsep_process.hpp"
#include "session.hpp"
#include "support/client_pdus.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
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

    /// The TLS connection, once start_tls has run.
    [[nodiscard]] const farland::auth::TlsClient& tls() const { return *tls_; }

    /// The next complete TSRequest (CredSSP, inside TLS).
    Bytes read_ts_request()
    {
        for (;;) {
            const auto size = farland::auth::credssp::frame_ts_request(plain_);
            REQUIRE(size.has_value());
            if (size->has_value() && plain_.size() >= **size) {
                return take(**size);
            }
            read_more();
        }
    }

    /// The next `n` plaintext bytes.
    Bytes read_exact(std::size_t n)
    {
        while (plain_.size() < n) {
            read_more();
        }
        return take(n);
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
    Bytes take(std::size_t n)
    {
        Bytes out(plain_.begin(), plain_.begin() + static_cast<std::ptrdiff_t>(n));
        plain_.erase(plain_.begin(), plain_.begin() + static_cast<std::ptrdiff_t>(n));
        return out;
    }

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
    std::size_t other_updates = 0;

    /// Applies a fast-path PDU; ignores everything else.
    void apply(const Bytes& pdu)
    {
        if (std::to_integer<unsigned>(pdu.at(0)) == 0x03) {
            return;
        }
        Reader r(pdu);
        // Named: GCC before 15 lacks C++23's lifetime extension in range-for.
        const auto fragments = proto::fastpath::decode_output_pdu(r).value();
        for (const auto& fragment : fragments) {
            auto update = reassembler.add(fragment).value();
            if (!update) {
                continue;
            }
            if (update->code != proto::fastpath::update_code::bitmap) {
                ++other_updates;  // pointer updates, for one
                continue;
            }
            Reader u(update->data);
            // Named: GCC before 15 lacks C++23's lifetime extension in range-for.
            const auto rects = proto::decode_bitmap_update(u).value();
            for (const auto& rect : rects) {
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

struct Login {
    std::string user;
    std::string password;
};

/// X.224 in plaintext, then TLS on the same stream and, with a login, NLA
/// with SPNEGO and NTLM. The server's NTSTATUS when it refused the login.
std::optional<std::uint32_t> negotiate(TestClient& c, std::uint32_t protocol, const Login* login)
{
    const auto requested = login != nullptr
                               ? proto::protocol::ssl | proto::protocol::hybrid | proto::protocol::hybrid_ex
                               : proto::protocol::ssl;
    c.send(client::connection_request(requested));
    {
        const auto cc = c.read_pdu();
        Reader r(cc);
        Reader tpdu = proto::read_tpkt(r).value();
        const auto confirm = proto::decode_connection_confirm(tpdu).value();
        REQUIRE(std::get<proto::NegotiationResponse>(confirm.result).selected_protocol == protocol);
    }
    c.start_tls();
    if (login == nullptr) {
        return std::nullopt;
    }

    namespace credssp = farland::auth::credssp;
    namespace ntlm = farland::auth::ntlm;
    ntlm::InitiatorConfig mechanism;
    mechanism.user = login->user;
    mechanism.password = farland::SecretString(std::string(login->password));
    mechanism.workstation = "E2E";
    mechanism.channel_bindings = ntlm::channel_bindings_hash(c.tls().peer_certificate_der());
    credssp::InitiatorConfig config;
    config.mechanism = std::make_unique<ntlm::Initiator>(std::move(mechanism));
    const auto key = c.tls().peer_subject_public_key();
    config.server_public_key.assign(key.begin(), key.end());
    config.credentials =
        farland::auth::PasswordCredentials{"", login->user, farland::SecretString(std::string(login->password))};
    credssp::Initiator initiator(std::move(config));
    for (;;) {
        c.send(initiator.take_output());
        if (initiator.status() != credssp::Initiator::Status::in_progress) {
            break;
        }
        initiator.receive(c.read_ts_request());
    }
    if (initiator.status() != credssp::Initiator::Status::succeeded) {
        return initiator.server_error_code().value_or(0);
    }
    if (protocol == proto::protocol::hybrid_ex) {
        // Early User Authorization Result: AUTHZ_SUCCESS.
        CHECK(c.read_exact(4) == Bytes(4, std::byte{0}));
    }
    return std::nullopt;
}

/// The next PDU from the server; auto-detect requests get their answer.
Bytes next_pdu(TestClient& c, client::AutoDetectResponder& autodetect)
{
    auto pdu = c.read_pdu();
    if (auto reply = autodetect.answer(pdu)) {
        c.send(*reply);
    }
    return pdu;
}

/// The next PDU that is not on the message channel.
Bytes next_io_pdu(TestClient& c, client::AutoDetectResponder& autodetect)
{
    for (;;) {
        auto pdu = next_pdu(c, autodetect);
        if (!autodetect.on_message_channel(pdu)) {
            return pdu;
        }
    }
}

/// MCS setup, Client Info, connect-time auto-detect, licensing, capabilities
/// and finalization, asking for a `client_width` x `client_height` desktop.
/// Like FreeRDP 3, the client supports auto-detect and heartbeats and joins
/// the message channel; `autodetect` answers for it from here on. Returns
/// the desktop size the server announced.
std::pair<std::uint16_t, std::uint16_t> activate(TestClient& c, std::uint32_t protocol, std::uint16_t client_width,
                                                 std::uint16_t client_height,
                                                 std::optional<client::AutoDetectResponder>& autodetect)
{
    // MCS connect and domain setup.
    auto client_data = client::client_data(protocol, 0x0001, client_width, client_height);
    client::request_autodetect(client_data);
    c.send(client::connect_initial(client_data));
    const auto message_channel = client::message_channel_id(c.read_pdu());  // Connect Response
    REQUIRE(message_channel.has_value());
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
    for (const std::uint16_t channel : {user, mcs::io_channel_id, *message_channel}) {
        c.send(client::channel_join(user, channel));
        static_cast<void>(c.read_pdu());
    }

    // Client Info, connect-time auto-detect, license, capabilities, finalization.
    autodetect.emplace(user, *message_channel);
    c.send(client::client_info(user, "e2e"));
    static_cast<void>(next_io_pdu(c, *autodetect));  // license, after the auto-detect exchange
    CHECK(autodetect->rtt_answers == 1);
    CHECK(autodetect->connect_time_results == 1);
    CHECK(autodetect->result.has_value());
    std::uint32_t share_id = 0;
    std::pair<std::uint16_t, std::uint16_t> size{client_width, client_height};
    {
        const auto payload = indication(next_io_pdu(c, *autodetect));
        Reader r(payload);
        auto control = proto::read_share_control(r).value();
        const auto demand = proto::decode_demand_active(control.body).value();
        share_id = demand.share_id;
        REQUIRE(demand.capabilities.bitmap.has_value());
        size = {demand.capabilities.bitmap->desktop_width, demand.capabilities.bitmap->desktop_height};
    }
    // Clients take the size the server announces in the Demand Active.
    c.send(client::confirm_active(user, share_id, true, size.first, size.second));
    c.send(client::finalization(user, share_id));
    for (int i = 0; i < 4; ++i) {
        static_cast<void>(next_io_pdu(c, *autodetect));  // Synchronize, Cooperate, Granted, Font Map
    }
    return size;
}

/// The scripted client, from X.224 to the server's shutdown: sets `stop`
/// once it has seen frames and input, then expects the ultimatum.
void run_client(int fd, std::atomic<bool>& stop, std::uint32_t protocol = proto::protocol::ssl,
                const Login* login = nullptr)
{
    TestClient c(fd);

    REQUIRE_FALSE(negotiate(c, protocol, login).has_value());

    std::optional<client::AutoDetectResponder> autodetect;
    activate(c, protocol, width, height, autodetect);

    // The first frames cover the whole desktop: 5 x 4 tiles.
    Canvas canvas;
    for (int i = 0; i < 400 && canvas.tiles_seen.size() < 20; ++i) {
        canvas.apply(next_pdu(c, *autodetect));
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
        canvas.apply(next_pdu(c, *autodetect));
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

/// No NLA users: these tests cover the TLS-only path.
class NoUsers final : public farland::auth::NtlmVerifier {
public:
    std::optional<std::array<std::byte, 16>> session_base_key(std::string_view /*user*/, std::string_view /*domain*/,
                                                              std::span<const std::byte, 8> /*challenge*/,
                                                              std::span<const std::byte> /*response*/) override
    {
        return std::nullopt;
    }
    bool verify_password(std::string_view /*user*/, std::string_view /*domain*/, std::string_view /*password*/) override
    {
        return false;
    }
};

/// One NLA user, alice, in an in-memory credential store.
class AliceVerifier final : public farland::auth::NtlmVerifier {
public:
    AliceVerifier()
        : verifier_([this](std::string_view user, std::string_view domain) { return store_.lookup(user, domain); })
    {
        store_.set("alice", "", farland::auth::ntlm::nt_hash("Secret1!"));
    }
    std::optional<std::array<std::byte, 16>> session_base_key(std::string_view user, std::string_view domain,
                                                              std::span<const std::byte, 8> challenge,
                                                              std::span<const std::byte> response) override
    {
        return verifier_.session_base_key(user, domain, challenge, response);
    }
    bool verify_password(std::string_view user, std::string_view domain, std::string_view password) override
    {
        return verifier_.verify_password(user, domain, password);
    }

private:
    farland::auth::CredentialStore store_;
    farland::auth::ntlm::LocalNtlmVerifier verifier_;
};

/// A shared desktop for tests: one solid frame, one cursor shape, and a sink
/// that records the input it gets. With `dmabuf_only`, the frame comes as a
/// dmabuf without pixels, which the session must map.
class FakeDesktop final : public farland::app::Desktop {
public:
    static constexpr std::uint32_t color = 0x3366CC;

    /// Written on the session thread; read by the test after joining it.
    class Frames final : public farland::platform::FrameSource {
    public:
        explicit Frames(bool dmabuf_only) : pixels_(static_cast<std::size_t>(width) * height * 4), dmabuf_only_(dmabuf_only)
        {
            for (std::size_t i = 0; i < pixels_.size(); i += 4) {
                pixels_[i] = std::byte{color & 0xFFU};
                pixels_[i + 1] = std::byte{(color >> 8U) & 0xFFU};
                pixels_[i + 2] = std::byte{(color >> 16U) & 0xFFU};
                pixels_[i + 3] = std::byte{0xFF};
            }
        }
        [[nodiscard]] int wake_fd() const noexcept override { return -1; }
        [[nodiscard]] std::optional<farland::platform::Frame> take_frame() override
        {
            if (!fresh_) {
                return std::nullopt;
            }
            fresh_ = false;
            if (dmabuf_only_) {
                farland::platform::Frame frame;
                frame.sequence = 1;
                frame.dmabuf = farland::platform::Dmabuf{.drm_format = 0x34325258,  // XRGB8888
                                                         .width = width,
                                                         .height = height,
                                                         .plane_count = 1};
                return frame;
            }
            return farland::platform::Frame{{pixels_, width, height, std::size_t{width} * 4}, {}, 1, std::nullopt};
        }
        [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> size() const override { return {width, height}; }
        void set_access(farland::platform::FrameAccess a) override { access = a; }
        [[nodiscard]] std::optional<farland::codec::ImageView> map_frame() override
        {
            ++maps;
            return farland::codec::ImageView{pixels_, width, height, std::size_t{width} * 4};
        }
        void release_frame() override { ++releases; }

        std::optional<farland::platform::FrameAccess> access;
        int maps = 0;
        int releases = 0;

    private:
        std::vector<std::byte> pixels_;
        bool dmabuf_only_;
        bool fresh_ = true;
    };

    class Cursor final : public farland::platform::CursorSource {
    public:
        [[nodiscard]] int wake_fd() const noexcept override { return -1; }
        [[nodiscard]] std::optional<farland::platform::CursorUpdate> take_cursor() override
        {
            if (!fresh_) {
                return std::nullopt;
            }
            fresh_ = false;
            farland::platform::CursorImage shape{16, 16, 1, 1, std::vector<std::byte>(16 * 16 * 4, std::byte{0xFF})};
            return farland::platform::CursorUpdate{std::move(shape), std::nullopt, true};
        }

    private:
        bool fresh_ = true;
    };

    /// Written on the session thread; read by the test after joining it.
    class Sink final : public farland::platform::InputSink {
    public:
        void key(std::uint32_t evdev_code, bool pressed) override
        {
            keys.emplace_back(evdev_code, pressed);
            ++events;
        }
        void pointer_motion_absolute(double x, double y) override
        {
            motion = {x, y};
            ++events;
        }
        void pointer_motion_relative(double /*dx*/, double /*dy*/) override {}
        void button(std::uint32_t /*evdev_button*/, bool /*pressed*/) override {}
        void scroll_discrete(std::int32_t /*x_v120*/, std::int32_t /*y_v120*/) override {}
        void text(char32_t /*codepoint*/) override {}
        void flush() override {}

        std::vector<std::pair<std::uint32_t, bool>> keys;
        std::optional<std::pair<double, double>> motion;
        std::atomic<int> events{0};
    };

    explicit FakeDesktop(bool dmabuf_only = false) : frames_(dmabuf_only) {}

    [[nodiscard]] Frames& frames() override { return frames_; }
    [[nodiscard]] farland::platform::CursorSource* cursor() override { return &cursor_; }
    [[nodiscard]] farland::platform::InputSink& input() override { return sink; }
    [[nodiscard]] std::vector<int> dispatch_fds() const override { return {}; }
    void dispatch() override {}
    [[nodiscard]] bool closed() const override { return false; }

    Sink sink;

private:
    Frames frames_;
    Cursor cursor_;
};

/// A certificate and key on disk for the network process, removed afterwards.
struct TempIdentity {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / ("farland-e2e-" + std::to_string(::getpid()));
    std::filesystem::path cert = dir / "cert.pem";
    std::filesystem::path key = dir / "key.pem";
    TempIdentity() { REQUIRE(farland::auth::TlsIdentity::load_or_create(cert, key, "e2e.farland.test").has_value()); }
    TempIdentity(const TempIdentity&) = delete;
    TempIdentity& operator=(const TempIdentity&) = delete;
    ~TempIdentity() { std::filesystem::remove_all(dir); }
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
    options.preauth.require_nla = false;  // this test covers the TLS-only path
    std::thread server([&] { farland::app::run_session(fds[0], "e2e", identity, options, stop); });
    run_client(fds[1], stop);
    server.join();
}

TEST_CASE("End to end through the privilege-separated network process")
{
    // The network process is farland-server itself; meson passes its path.
    const char* executable = std::getenv("FARLAND_SERVER");
    if (executable == nullptr) {
        SKIP("FARLAND_SERVER is not set");
    }
    const TempIdentity files;

    std::array<int, 2> fds{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
    const farland::app::ChildLaunch launch{executable,
                                           {"--cert", files.cert.string(), "--key", files.key.string(), "--hostname",
                                            "e2e.farland.test", "--log-level", "warn", "--allow-tls-only"}};
    NoUsers verifier;
    std::atomic<bool> stop{false};
    farland::app::SessionOptions options;
    options.frames_per_second = 60;
    options.preauth.require_nla = false;
    std::thread monitor(
        [&] { farland::app::run_monitored_session(fds[0], "e2e-privsep", launch, verifier, options, stop); });
    run_client(fds[1], stop);
    monitor.join();
}

TEST_CASE("End to end with NLA: HYBRID_EX, SPNEGO and NTLM in process")
{
    std::array<int, 2> fds{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
    const auto identity = farland::auth::TlsIdentity::generate("e2e.farland.test").value();
    AliceVerifier verifier;
    std::atomic<bool> stop{false};
    farland::app::SessionOptions options;
    options.frames_per_second = 60;
    options.make_nla = farland::app::make_nla_factory(identity, verifier, "e2e.farland.test");
    std::thread server([&] { farland::app::run_session(fds[0], "e2e-nla", identity, options, stop); });
    const Login login{"alice", "Secret1!"};
    run_client(fds[1], stop, proto::protocol::hybrid_ex, &login);
    server.join();
}

TEST_CASE("NLA with a wrong password ends with STATUS_LOGON_FAILURE")
{
    std::array<int, 2> fds{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
    const auto identity = farland::auth::TlsIdentity::generate("e2e.farland.test").value();
    AliceVerifier verifier;
    std::atomic<bool> stop{false};
    farland::app::SessionOptions options;
    options.make_nla = farland::app::make_nla_factory(identity, verifier, "e2e.farland.test");
    std::thread server([&] { farland::app::run_session(fds[0], "e2e-nla-bad", identity, options, stop); });
    {
        TestClient c(fds[1]);
        const Login login{"alice", "wrong"};
        CHECK(negotiate(c, proto::protocol::hybrid_ex, &login) == farland::auth::credssp::status_logon_failure);
    }
    server.join();
}

TEST_CASE("TLS-only clients are refused while NLA is required")
{
    std::array<int, 2> fds{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
    const auto identity = farland::auth::TlsIdentity::generate("e2e.farland.test").value();
    AliceVerifier verifier;
    std::atomic<bool> stop{false};
    farland::app::SessionOptions options;
    options.make_nla = farland::app::make_nla_factory(identity, verifier, "e2e.farland.test");
    std::thread server([&] { farland::app::run_session(fds[0], "e2e-tls-refused", identity, options, stop); });
    {
        TestClient c(fds[1]);
        c.send(client::connection_request(proto::protocol::ssl));
        const auto cc = c.read_pdu();
        Reader r(cc);
        Reader tpdu = proto::read_tpkt(r).value();
        const auto confirm = proto::decode_connection_confirm(tpdu).value();
        CHECK(std::get<proto::NegotiationFailureCode>(confirm.result) ==
              proto::NegotiationFailureCode::hybrid_required_by_server);
    }
    server.join();
}

TEST_CASE("End to end with NLA through the privilege-separated network process")
{
    const char* executable = std::getenv("FARLAND_SERVER");
    if (executable == nullptr) {
        SKIP("FARLAND_SERVER is not set");
    }
    const TempIdentity files;
    std::array<int, 2> fds{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
    const farland::app::ChildLaunch launch{executable,
                                           {"--cert", files.cert.string(), "--key", files.key.string(), "--hostname",
                                            "e2e.farland.test", "--log-level", "warn"}};
    AliceVerifier verifier;  // only the monitor holds the store
    std::atomic<bool> stop{false};
    farland::app::SessionOptions options;
    options.frames_per_second = 60;
    std::thread monitor(
        [&] { farland::app::run_monitored_session(fds[0], "e2e-privsep-nla", launch, verifier, options, stop); });
    const Login login{"alice", "Secret1!"};
    run_client(fds[1], stop, proto::protocol::hybrid_ex, &login);
    monitor.join();
}

TEST_CASE("End to end with a shared desktop: its size, frames and cursor, input into it")
{
    // Frames as pixels, and as dmabufs the session maps for bitmap updates.
    const bool dmabuf_only = GENERATE(false, true);
    CAPTURE(dmabuf_only);
    std::array<int, 2> fds{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
    const auto identity = farland::auth::TlsIdentity::generate("e2e.farland.test").value();
    FakeDesktop desktop(dmabuf_only);
    std::atomic<bool> stop{false};
    farland::app::SessionOptions options;
    options.frames_per_second = 60;
    options.preauth.require_nla = false;
    options.desktop = &desktop;
    std::thread server([&] { farland::app::run_session(fds[0], "e2e-desktop", identity, options, stop); });
    {
        TestClient c(fds[1]);
        REQUIRE_FALSE(negotiate(c, proto::protocol::ssl, nullptr).has_value());
        // The client asks for 640x480, but a shared desktop has the size it has.
        std::optional<client::AutoDetectResponder> autodetect;
        CHECK(activate(c, proto::protocol::ssl, 640, 480, autodetect) ==
              std::pair<std::uint16_t, std::uint16_t>{width, height});

        Canvas canvas;
        for (int i = 0; i < 400 && (canvas.tiles_seen.size() < 20 || canvas.other_updates == 0); ++i) {
            canvas.apply(next_pdu(c, *autodetect));
        }
        CHECK(canvas.tiles_seen.size() == 20);
        CHECK(canvas.rgb(5, 5) == FakeDesktop::color);
        CHECK(canvas.rgb(315, 235) == FakeDesktop::color);
        CHECK(canvas.other_updates > 0);  // the cursor, as pointer updates

        const std::array input{
            proto::InputEvent{proto::MouseEvent{proto::ptr_flags::move, 100, 50}},
            proto::InputEvent{proto::KeyboardEvent{0, 0x1E}},                          // A down
            proto::InputEvent{proto::KeyboardEvent{proto::kbd_flags::release, 0x1E}},  // A up
        };
        c.send(client::fastpath_input(input));
        for (int i = 0; i < 300 && desktop.sink.events.load() < 3; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        stop = true;
    }
    server.join();
    const std::vector<std::pair<std::uint32_t, bool>> expected_keys{{30, true}, {30, false}};  // KEY_A
    CHECK(desktop.sink.keys == expected_keys);
    CHECK(desktop.sink.motion == std::pair{100.0, 50.0});
    // Bitmap updates need pixels: the one dmabuf frame was mapped, and the
    // session leaves the desktop in CPU mode with its frame given back.
    CHECK(desktop.frames().maps == (dmabuf_only ? 1 : 0));
    CHECK(desktop.frames().access == farland::platform::FrameAccess::cpu);
    CHECK(desktop.frames().releases == 1);
}
