// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/proto/framing.hpp>
#include <farland/server/preauth.hpp>

#include <format>
#include <utility>

namespace farland::server {

namespace {

constexpr std::string_view log_component = "server.preauth";
/// A Connection Request is at most a TPKT around a 255-byte TPDU.
constexpr std::size_t max_connection_request = 4 + 255;
constexpr std::uint32_t nla_protocols = proto::protocol::hybrid | proto::protocol::hybrid_ex;

std::string_view protocol_name(std::uint32_t protocol)
{
    switch (protocol) {
    case proto::protocol::ssl:
        return "TLS";
    case proto::protocol::hybrid:
        return "NLA (HYBRID)";
    case proto::protocol::hybrid_ex:
        return "NLA (HYBRID_EX)";
    default:
        return "unknown";
    }
}

}  // namespace

PreAuth::PreAuth(PreAuthConfig config, NlaFactory make_nla) : config_(config), make_nla_(std::move(make_nla))
{
    if (!make_nla_) {
        config_.supported_protocols &= ~nla_protocols;
    }
}

void PreAuth::receive(std::span<const std::byte> bytes)
{
    switch (state_) {
    case State::wait_connection_request: {
        input_.insert(input_.end(), bytes.begin(), bytes.end());
        const auto frame = proto::peek_frame(input_);
        if (!frame) {
            fail(frame.error().message());
            return;
        }
        if (!frame->has_value()) {
            return;
        }
        if ((*frame)->kind != proto::FrameKind::tpkt || (*frame)->length > max_connection_request) {
            fail("expected an X.224 Connection Request");
            return;
        }
        if (input_.size() < (*frame)->length) {
            return;
        }
        if (input_.size() > (*frame)->length) {
            // The client must wait for the Connection Confirm before TLS starts.
            fail("client data after the X.224 Connection Request");
            return;
        }
        const auto packet = std::exchange(input_, {});
        if (auto handled = on_connection_request(packet); !handled) {
            fail(handled.error().message());
        }
        return;
    }
    case State::wait_tls:
        fail("client data before the TLS handshake completed");
        return;
    case State::nla:
        nla_->receive(bytes);
        pump_nla();
        return;
    case State::ready:
        // Held until the caller moves the stream over to its Connection.
        input_.insert(input_.end(), bytes.begin(), bytes.end());
        return;
    case State::failed:
        return;
    }
}

Result<void> PreAuth::on_connection_request(std::span<const std::byte> packet)
{
    Reader r(packet);
    FARLAND_TRY(Reader tpdu, proto::read_tpkt(r));
    FARLAND_TRY(const proto::TpduCode code, proto::peek_tpdu_code(tpdu));
    if (code != proto::TpduCode::connection_request) {
        return farland::fail(Errc::invalid_value, "expected an X.224 Connection Request", tpdu.offset());
    }
    FARLAND_TRY(const auto request, proto::decode_connection_request(tpdu));
    negotiation_.cookie = request.cookie;
    if (!request.negotiation) {
        // Without RDP_NEG_REQ the client only speaks Standard RDP Security,
        // which farland never offers. There is no way to say so politely.
        fail("client does not negotiate a security protocol (Standard RDP Security is not supported)");
        return {};
    }
    const std::uint32_t requested = request.negotiation->requested_protocols;
    negotiation_.requested_protocols = requested;
    const std::uint32_t usable = requested & config_.supported_protocols;

    std::optional<std::uint32_t> selected;
    if ((usable & proto::protocol::hybrid_ex) != 0) {
        selected = proto::protocol::hybrid_ex;
    } else if ((usable & proto::protocol::hybrid) != 0) {
        selected = proto::protocol::hybrid;
    } else if (!config_.require_nla && (usable & proto::protocol::ssl) != 0) {
        selected = proto::protocol::ssl;
    }

    proto::ConnectionConfirm confirm;
    if (selected) {
        negotiation_.selected_protocol = *selected;
        const std::uint8_t flags = proto::neg_rsp_flags::extended_client_data_supported |
                                   (config_.advertise_gfx ? proto::neg_rsp_flags::dynvc_gfx_protocol_supported : 0U);
        confirm.result = proto::NegotiationResponse{flags, *selected};
    } else {
        const bool nla_offered = (config_.supported_protocols & nla_protocols) != 0;
        confirm.result = nla_offered && config_.require_nla ? proto::NegotiationFailureCode::hybrid_required_by_server
                                                            : proto::NegotiationFailureCode::ssl_required_by_server;
    }
    proto::encode_connection_confirm(output_, confirm);
    if (!selected) {
        fail(std::format("client requested protocols 0x{:x}, none of which this server accepts", requested));
        return {};
    }
    log::info(log_component, "client requested protocols 0x{:x}, selected {}{}{}", requested, protocol_name(*selected),
              negotiation_.cookie.empty() ? "" : ", cookie user ", negotiation_.cookie);
    state_ = State::wait_tls;
    events_.emplace_back(preauth_event::StartTls{});
    return {};
}

void PreAuth::tls_established()
{
    if (state_ != State::wait_tls) {
        fail("TLS established in the wrong state");
        return;
    }
    if (negotiation_.selected_protocol == proto::protocol::ssl) {
        finish();
        return;
    }
    nla_ = make_nla_();
    state_ = State::nla;
    pump_nla();
}

void PreAuth::pump_nla()
{
    output_.bytes(nla_->take_output());
    switch (nla_->status()) {
    case auth::NlaAcceptor::Status::in_progress:
        return;
    case auth::NlaAcceptor::Status::failed:
        fail(std::format("NLA failed: {}", nla_->failure_reason()));
        nla_.reset();
        return;
    case auth::NlaAcceptor::Status::succeeded:
        break;
    }
    negotiation_.identity = nla_->identity();
    log::info(log_component, "NLA authenticated '{}'{}{}", nla_->identity().user,
              nla_->identity().domain.empty() ? "" : " in domain ", nla_->identity().domain);
    events_.emplace_back(preauth_event::Authenticated{nla_->identity(), nla_->take_credentials()});
    if (negotiation_.selected_protocol == proto::protocol::hybrid_ex) {
        output_.u32le(early_auth::success);
    }
    input_ = nla_->take_remaining_input();
    nla_.reset();
    finish();
}

void PreAuth::finish()
{
    state_ = State::ready;
    events_.emplace_back(preauth_event::Ready{});
}

std::vector<std::byte> PreAuth::take_output()
{
    auto bytes = std::move(output_).take();
    output_ = Writer{};
    return bytes;
}

std::optional<PreAuthEvent> PreAuth::poll_event()
{
    if (events_.empty()) {
        return std::nullopt;
    }
    PreAuthEvent event = std::move(events_.front());
    events_.pop_front();
    return event;
}

std::vector<std::byte> PreAuth::take_remaining_input()
{
    return std::exchange(input_, {});
}

void PreAuth::fail(std::string reason)
{
    log::warn(log_component, "closing connection: {}", reason);
    state_ = State::failed;
    events_.emplace_back(preauth_event::Failed{std::move(reason)});
}

}  // namespace farland::server
