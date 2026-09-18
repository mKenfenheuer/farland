// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "rdp_test_client.hpp"

#include <farland/auth/credssp.hpp>
#include <farland/auth/ntlm.hpp>
#include <farland/codec/planar.hpp>
#include <farland/proto/bitmap.hpp>
#include <farland/proto/client_info.hpp>
#include <farland/proto/framing.hpp>
#include <farland/proto/mcs.hpp>
#include <farland/proto/security.hpp>
#include <farland/proto/share.hpp>
#include <farland/proto/x224.hpp>

#include "support/client_pdus.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace farland::test {

namespace {

namespace mcs = proto::mcs;

#ifdef MSG_NOSIGNAL
constexpr int send_flags = MSG_NOSIGNAL;
#else
constexpr int send_flags = 0;
#endif

/// The payload of a server Send Data Indication, or nullopt for other PDUs.
std::optional<Bytes> indication(const Bytes& tpkt)
{
    Reader r(tpkt);
    auto tpdu = proto::read_tpkt(r);
    if (!tpdu) {
        return std::nullopt;
    }
    auto data = proto::decode_data_tpdu(*tpdu);
    if (!data) {
        return std::nullopt;
    }
    auto pdu = mcs::decode_domain_pdu(*data);
    if (!pdu) {
        return std::nullopt;
    }
    if (const auto* send = std::get_if<mcs::SendDataIndication>(&*pdu)) {
        return Bytes(send->data.begin(), send->data.end());
    }
    return std::nullopt;
}

}  // namespace

RdpTestClient::RdpTestClient(int fd) : fd_(fd)
{
#ifdef SO_NOSIGPIPE
    const int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

RdpTestClient::~RdpTestClient()
{
    drop();
}

int RdpTestClient::connect_tcp(std::uint16_t port, int wait_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (true) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API takes a generic sockaddr
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
            return fd;
        }
        ::close(fd);
        if (std::chrono::steady_clock::now() > deadline) {
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void RdpTestClient::drop()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void RdpTestClient::send_raw(std::span<const std::byte> bytes)
{
    while (!bytes.empty() && fd_ >= 0) {
        const ssize_t n = ::send(fd_, bytes.data(), bytes.size(), send_flags);
        if (n <= 0) {
            closed_ = true;
            return;
        }
        bytes = bytes.subspan(static_cast<std::size_t>(n));
    }
}

void RdpTestClient::send(std::span<const std::byte> bytes)
{
    if (tls_) {
        tls_->send(bytes);
        send_raw(tls_->take_ciphertext());
    } else {
        send_raw(bytes);
    }
}

void RdpTestClient::pump_tls()
{
    Bytes plaintext;
    REQUIRE(tls_->process(plaintext).has_value());
    plain_.insert(plain_.end(), plaintext.begin(), plaintext.end());
    send_raw(tls_->take_ciphertext());
}

bool RdpTestClient::read_more(int timeout_ms)
{
    if (closed_ || fd_ < 0) {
        return false;
    }
    pollfd pfd{fd_, POLLIN, 0};
    if (::poll(&pfd, 1, timeout_ms) != 1) {
        return false;
    }
    std::array<std::byte, 65536> buffer{};
    const ssize_t n = ::recv(fd_, buffer.data(), buffer.size(), 0);
    if (n <= 0) {
        closed_ = true;
        return false;
    }
    const auto received = std::span(buffer).first(static_cast<std::size_t>(n));
    if (tls_) {
        tls_->receive(received);
        pump_tls();
    } else {
        plain_.insert(plain_.end(), received.begin(), received.end());
    }
    return true;
}

std::optional<Bytes> RdpTestClient::read_pdu()
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
        if (!read_more()) {
            return std::nullopt;
        }
    }
}

Bytes RdpTestClient::read_exact(std::size_t n)
{
    while (plain_.size() < n) {
        REQUIRE(read_more());
    }
    Bytes out(plain_.begin(), plain_.begin() + static_cast<std::ptrdiff_t>(n));
    plain_.erase(plain_.begin(), plain_.begin() + static_cast<std::ptrdiff_t>(n));
    return out;
}

bool RdpTestClient::login(const std::string& user, const std::string& password)
{
    send(client::connection_request(proto::protocol::ssl | proto::protocol::hybrid | proto::protocol::hybrid_ex));
    {
        const auto cc = read_pdu();
        REQUIRE(cc.has_value());
        Reader r(*cc);
        Reader tpdu = proto::read_tpkt(r).value();
        const auto confirm = proto::decode_connection_confirm(tpdu).value();
        REQUIRE(std::get<proto::NegotiationResponse>(confirm.result).selected_protocol == proto::protocol::hybrid_ex);
    }
    tls_.emplace();
    pump_tls();
    while (!tls_->handshake_complete()) {
        REQUIRE(read_more());
    }

    namespace credssp = auth::credssp;
    namespace ntlm = auth::ntlm;
    ntlm::InitiatorConfig mechanism;
    mechanism.user = user;
    mechanism.password = SecretString(std::string(password));
    mechanism.workstation = "E2E";
    mechanism.channel_bindings = ntlm::channel_bindings_hash(tls_->peer_certificate_der());
    credssp::InitiatorConfig config;
    config.mechanism = std::make_unique<ntlm::Initiator>(std::move(mechanism));
    const auto key = tls_->peer_subject_public_key();
    config.server_public_key.assign(key.begin(), key.end());
    config.credentials = auth::PasswordCredentials{"", user, SecretString(std::string(password))};
    credssp::Initiator initiator(std::move(config));
    for (;;) {
        send(initiator.take_output());
        if (initiator.status() != credssp::Initiator::Status::in_progress) {
            break;
        }
        for (;;) {
            const auto size = credssp::frame_ts_request(plain_);
            REQUIRE(size.has_value());
            if (size->has_value() && plain_.size() >= **size) {
                break;
            }
            if (!read_more()) {
                return false;
            }
        }
        const std::size_t size = **credssp::frame_ts_request(plain_);
        initiator.receive(read_exact(size));
    }
    if (initiator.status() != credssp::Initiator::Status::succeeded) {
        return false;
    }
    return read_exact(4) == Bytes(4, std::byte{0});  // Early User Authorization Result: AUTHZ_SUCCESS
}

std::pair<std::uint16_t, std::uint16_t> RdpTestClient::activate(std::uint16_t width, std::uint16_t height,
                                                                const std::optional<proto::AutoReconnectCookie>& cookie)
{
    // 0x0001: RNS_UD_CS_SUPPORT_ERRINFO_PDU, so the server says why it ends.
    const auto data = client::client_data(proto::protocol::hybrid_ex, 0x0001, width, height);
    send(client::connect_initial(data));
    REQUIRE(read_pdu().has_value());  // Connect Response
    send(client::erect_domain());
    send(client::attach_user());
    {
        const auto confirm = read_pdu();
        REQUIRE(confirm.has_value());
        Reader r(*confirm);
        Reader tpdu = proto::read_tpkt(r).value();
        Reader d = proto::decode_data_tpdu(tpdu).value();
        user_ = std::get<mcs::AttachUserConfirm>(mcs::decode_domain_pdu(d).value()).initiator.value();
    }
    for (const std::uint16_t channel : {user_, mcs::io_channel_id}) {
        send(client::channel_join(user_, channel));
        REQUIRE(read_pdu().has_value());
    }

    proto::ClientInfo info;
    info.flags = proto::info_flags::unicode | proto::info_flags::mouse;
    info.user_name = "e2e";
    if (cookie) {
        proto::ExtendedInfo extended;
        extended.client_address = "127.0.0.1";
        extended.client_dir = "C:\\farland";
        extended.time_zone = proto::TimeZoneInformation{};
        extended.session_id = 0;
        extended.performance_flags = 0;
        extended.auto_reconnect_cookie = cookie;
        info.extended = std::move(extended);
    }
    Writer w;
    proto::write_basic_security_header(w, proto::sec_flags::info_pkt);
    proto::encode_client_info(w, info);
    send(client::io(user_, w.view()));

    // License, then the Demand Active.
    for (;;) {
        const auto pdu = read_pdu();
        REQUIRE(pdu.has_value());
        const auto payload = indication(*pdu);
        if (!payload) {
            continue;
        }
        Reader r(*payload);
        auto control = proto::read_share_control(r);
        if (!control || control->type != proto::pdu_type::demand_active) {
            continue;
        }
        const auto demand = proto::decode_demand_active(control->body).value();
        share_id_ = demand.share_id;
        REQUIRE(demand.capabilities.bitmap.has_value());
        width_ = demand.capabilities.bitmap->desktop_width;
        height_ = demand.capabilities.bitmap->desktop_height;
        break;
    }
    pixels_.assign(static_cast<std::size_t>(width_) * height_ * 4, std::byte{0});
    send(client::confirm_active(user_, share_id_, true, width_, height_));
    send(client::finalization(user_, share_id_));
    return {width_, height_};
}

void RdpTestClient::send_keys(std::initializer_list<std::uint16_t> scancodes)
{
    std::vector<proto::InputEvent> events;
    for (const auto code : scancodes) {
        events.emplace_back(proto::KeyboardEvent{0, code});
        events.emplace_back(proto::KeyboardEvent{proto::kbd_flags::release, code});
    }
    send(client::fastpath_input(events));
}

bool RdpTestClient::read_one()
{
    if (ended_) {
        return false;
    }
    const auto pdu = read_pdu();
    if (!pdu) {
        ended_ = true;
        return false;
    }
    if (std::to_integer<unsigned>(pdu->at(0)) == 0x03) {
        apply_slow_path(*pdu);
    } else {
        apply_fastpath(*pdu);
    }
    return !ended_;
}

void RdpTestClient::apply_slow_path(const Bytes& pdu)
{
    Reader r(pdu);
    Reader tpdu = proto::read_tpkt(r).value();
    auto data = proto::decode_data_tpdu(tpdu);
    if (!data) {
        return;
    }
    const auto domain = mcs::decode_domain_pdu(*data);
    if (!domain) {
        return;
    }
    if (std::holds_alternative<mcs::DisconnectProviderUltimatum>(*domain)) {
        ended_ = true;
        return;
    }
    const auto* send = std::get_if<mcs::SendDataIndication>(&*domain);
    if (send == nullptr) {
        return;
    }
    Reader body(send->data);
    auto control = proto::read_share_control(body);
    if (!control || control->type != proto::pdu_type::data) {
        return;
    }
    auto share = proto::read_share_data(control->body);
    if (!share) {
        return;
    }
    if (share->type2 == proto::pdu_type2::save_session_info) {
        const auto info = proto::decode_save_session_info(share->payload);
        REQUIRE(info.has_value());
        if (info->auto_reconnect_cookie) {
            arc_ = info->auto_reconnect_cookie;
        }
        return;
    }
    if (share->type2 == proto::pdu_type2::set_error_info) {
        const auto decoded = proto::decode_data_pdu(*share);
        REQUIRE(decoded.has_value());
        error_info_ = std::get<proto::SetErrorInfo>(*decoded).error_info;
    }
}

void RdpTestClient::apply_fastpath(const Bytes& pdu)
{
    Reader r(pdu);
    const auto fragments = proto::fastpath::decode_output_pdu(r).value();
    for (const auto& fragment : fragments) {
        auto update = reassembler_.add(fragment).value();
        if (!update || update->code != proto::fastpath::update_code::bitmap) {
            continue;
        }
        Reader u(update->data);
        const auto rects = proto::decode_bitmap_update(u).value();
        for (const auto& rect : rects) {
            std::vector<std::byte> decoded(static_cast<std::size_t>(rect.width) * rect.height * 4);
            REQUIRE(codec::planar::decode(rect.data, rect.width, rect.height, codec::planar::Orientation::bottom_up,
                                          decoded)
                        .has_value());
            for (std::uint32_t y = rect.dest_top; y <= rect.dest_bottom && y < height_; ++y) {
                for (std::uint32_t x = rect.dest_left; x <= rect.dest_right && x < width_; ++x) {
                    const auto src =
                        ((static_cast<std::size_t>(y - rect.dest_top) * rect.width) + (x - rect.dest_left)) * 4;
                    const auto dst = ((static_cast<std::size_t>(y) * width_) + x) * 4;
                    std::copy_n(decoded.begin() + static_cast<std::ptrdiff_t>(src), 4,
                                pixels_.begin() + static_cast<std::ptrdiff_t>(dst));
                }
            }
        }
        ++bitmap_updates_;
    }
}

std::uint32_t RdpTestClient::rgb(std::uint32_t x, std::uint32_t y) const
{
    const auto at = ((static_cast<std::size_t>(y) * width_) + x) * 4;
    return std::to_integer<std::uint32_t>(pixels_.at(at)) | (std::to_integer<std::uint32_t>(pixels_.at(at + 1)) << 8U) |
           (std::to_integer<std::uint32_t>(pixels_.at(at + 2)) << 16U);
}

}  // namespace farland::test
