// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/proto/client_info.hpp>
#include <farland/proto/fastpath.hpp>
#include <farland/proto/framing.hpp>
#include <farland/proto/license.hpp>
#include <farland/proto/mcs.hpp>
#include <farland/proto/security.hpp>
#include <farland/server/connection.hpp>

#include <algorithm>
#include <utility>

namespace farland::server {

namespace {

namespace caps = proto::caps;
namespace gcc = proto::gcc;
namespace mcs = proto::mcs;

constexpr std::string_view log_component = "server.connection";
constexpr std::size_t max_buffered_input = std::size_t{1} << 20U;
constexpr std::uint32_t max_rdp_version = 0x00080011;  // RDP 10.12, as FreeRDP's server
constexpr std::uint32_t channel_chunk_length = 1600;
constexpr std::size_t slow_path_update_limit = 0x3FFF - 64;  // MCS PER length minus headers

/// The color depth to run the session at, from the client's CS_CORE.
std::uint16_t choose_bits_per_pixel(const gcc::ClientCoreData& core, std::uint16_t max_bpp)
{
    const std::uint16_t supported = core.supported_color_depths.value_or(0);
    if (max_bpp >= 32 && (supported & gcc::color_depth_support::bpp32) != 0) {
        return 32;
    }
    if (max_bpp >= 24 && ((supported & gcc::color_depth_support::bpp24) != 0 || core.high_color_depth == 24)) {
        return 24;
    }
    return 16;
}

caps::CapabilitySets server_capabilities(const Session& session)
{
    caps::CapabilitySets sets;
    sets.general = caps::General{};
    sets.general->os_major_type = caps::os_major::unix;
    sets.general->extra_flags = caps::general_extra_flags::fastpath_output_supported |
                                caps::general_extra_flags::long_credentials_supported |
                                caps::general_extra_flags::no_bitmap_compression_hdr;
    sets.general->refresh_rect_support = 1;
    sets.general->suppress_output_support = 1;

    sets.bitmap = caps::Bitmap{};
    sets.bitmap->preferred_bits_per_pixel = session.bits_per_pixel;
    sets.bitmap->desktop_width = session.desktop_width;
    sets.bitmap->desktop_height = session.desktop_height;

    // No drawing orders; NEGOTIATEORDERSUPPORT and ZEROBOUNDSDELTASSUPPORT are
    // mandatory, [MS-RDPBCGR] 2.2.7.1.3.
    sets.order = caps::Order{};
    sets.order->order_flags = 0x0002 | 0x0008 | 0x0020;

    sets.pointer = caps::Pointer{};
    sets.input = caps::Input{};
    sets.input->input_flags = caps::input_flags::scancodes | caps::input_flags::mousex | caps::input_flags::unicode |
                              caps::input_flags::fastpath_input | caps::input_flags::fastpath_input2 |
                              caps::input_flags::mouse_hwheel;
    sets.virtual_channel = caps::VirtualChannel{0, channel_chunk_length};
    sets.share = caps::Share{mcs::server_channel_id};
    sets.font = caps::Font{};

    // What the server can reassemble from the client, sized like FreeRDP's
    // server: one 16 KB fragment per 64x64 tile of the desktop, plus one.
    const std::uint32_t tiles = ((session.desktop_width + 63U) / 64U) * ((session.desktop_height + 63U) / 64U);
    sets.multifragment_update = caps::MultifragmentUpdate{(tiles + 1) * 16384U};
    return sets;
}

}  // namespace

std::string_view to_string(State state) noexcept
{
    switch (state) {
    case State::wait_connect_initial:
        return "wait-connect-initial";
    case State::wait_erect_domain:
        return "wait-erect-domain";
    case State::wait_attach_user:
        return "wait-attach-user";
    case State::wait_channel_joins:
        return "wait-channel-joins";
    case State::wait_confirm_active:
        return "wait-confirm-active";
    case State::finalizing:
        return "finalizing";
    case State::active:
        return "active";
    case State::closed:
        return "closed";
    }
    return "unknown";
}

std::optional<std::uint16_t> Session::static_channel_id(std::string_view name) const
{
    const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    const auto found = std::ranges::find_if(static_channels, [&](const auto& channel) {
        return channel.first.size() == name.size() &&
               std::ranges::equal(channel.first, name, [&](char a, char b) { return lower(a) == lower(b); });
    });
    if (found == static_channels.end()) {
        return std::nullopt;
    }
    return found->second;
}

bool Session::supports_gfx() const
{
    return client_data.core.has_early_flag(gcc::cs_early_flags::support_dynvc_gfx_protocol) && bits_per_pixel == 32 &&
           static_channel_id("drdynvc").has_value();
}

Connection::Connection(ServerConfig config, Negotiation negotiation) : config_(config)
{
    session_.negotiation = std::move(negotiation);
}

// Input -------------------------------------------------------------------------

void Connection::receive(std::span<const std::byte> bytes)
{
    if (state_ == State::closed) {
        return;
    }
    input_.insert(input_.end(), bytes.begin(), bytes.end());
    while (state_ != State::closed) {
        const auto frame = proto::peek_frame(input_);
        if (!frame) {
            fail(frame.error().message());
            return;
        }
        if (!frame->has_value()) {
            break;
        }
        const std::size_t length = (*frame)->length;
        if (input_.size() < length) {
            break;
        }
        const std::vector<std::byte> pdu(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(length));
        input_.erase(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(length));
        if (auto handled = handle_pdu((*frame)->kind, pdu); !handled) {
            fail(handled.error().message());
            return;
        }
    }
    if (input_.size() > max_buffered_input) {
        fail("too much unframed input");
    }
}

std::vector<std::byte> Connection::take_output()
{
    auto bytes = std::move(output_).take();
    output_ = Writer{};
    return bytes;
}

std::optional<Event> Connection::poll_event()
{
    if (events_.empty()) {
        return std::nullopt;
    }
    Event event = std::move(events_.front());
    events_.pop_front();
    return event;
}

Result<void> Connection::handle_pdu(proto::FrameKind kind, std::span<const std::byte> pdu)
{
    Reader r(pdu);
    if (kind == proto::FrameKind::fastpath) {
        return on_fastpath_input(r);
    }
    FARLAND_TRY(Reader tpdu, proto::read_tpkt(r));
    FARLAND_TRY(const proto::TpduCode code, proto::peek_tpdu_code(tpdu));
    if (code == proto::TpduCode::disconnect_request) {
        close("client sent an X.224 Disconnect Request");
        return {};
    }
    if (code != proto::TpduCode::data) {
        return farland::fail(Errc::invalid_value, "unexpected X.224 TPDU", tpdu.offset());
    }
    FARLAND_TRY(Reader data, proto::decode_data_tpdu(tpdu));
    if (state_ == State::wait_connect_initial) {
        return on_connect_initial(data);
    }
    return on_domain_pdu(data);
}

// Connection sequence -------------------------------------------------------------

Result<void> Connection::on_connect_initial(Reader& data)
{
    FARLAND_TRY(const auto initial, mcs::decode_connect_initial(data));
    Reader user_data(initial.user_data);
    FARLAND_TRY(const auto blocks, gcc::decode_conference_create_request(user_data));
    Reader block_reader(blocks);
    FARLAND_TRY(session_.client_data, gcc::decode_client_data(block_reader));
    const auto& client = session_.client_data;
    const auto& core = client.core;

    // [MS-RDPBCGR] 3.3.5.3.3: serverSelectedProtocol must repeat our choice;
    // a mismatch means the X.224 exchange was tampered with.
    if (core.server_selected_protocol && *core.server_selected_protocol != session_.negotiation.selected_protocol) {
        close("serverSelectedProtocol does not match the negotiated protocol");
        return {};
    }

    session_.desktop_width = std::clamp(core.desktop_width, config_.min_desktop_size, config_.max_desktop_size);
    session_.desktop_height = std::clamp(core.desktop_height, config_.min_desktop_size, config_.max_desktop_size);
    session_.bits_per_pixel = choose_bits_per_pixel(core, config_.max_bits_per_pixel);
    session_.supports_error_info = core.has_early_flag(gcc::cs_early_flags::support_errinfo_pdu);

    // MCS channel IDs: I/O 1003, static channels from 1004, then the message
    // channel, then the user channel (as FreeRDP's server assigns them).
    std::uint16_t next_id = mcs::io_channel_id + 1;
    gcc::ServerData server;
    server.network.io_channel_id = mcs::io_channel_id;
    if (client.network) {
        for (const auto& channel : client.network->channels) {
            session_.static_channels.emplace_back(channel.name, next_id);
            server.network.channel_ids.push_back(next_id);
            ++next_id;
        }
    }
    if (client.message_channel) {
        session_.message_channel_id = next_id;
        server.message_channel_id = next_id;
        ++next_id;
    }
    session_.user_channel_id = next_id;

    server.core.version = std::clamp(core.version, gcc::rdp_version_5_plus, max_rdp_version);
    server.core.client_requested_protocols = session_.negotiation.requested_protocols;
    server.core.early_capability_flags = core.has_early_flag(gcc::cs_early_flags::support_skip_channeljoin)
                                             ? gcc::sc_early_flags::skip_channeljoin_supported
                                             : 0U;

    Writer server_blocks;
    gcc::encode_server_data(server_blocks, server);
    Writer conference;
    gcc::encode_conference_create_response(conference, server_blocks.view());
    mcs::ConnectResponse response;
    response.parameters = mcs::negotiate_domain_parameters(initial);
    response.user_data = conference.view();

    const std::size_t start = proto::begin_data_tpdu(output_);
    mcs::encode_connect_response(output_, response);
    proto::end_tpkt(output_, start);

    log::info(log_component, "client '{}' build {}: {}x{} at {} bpp, {} static channels, user channel {}",
              core.client_name, core.client_build, session_.desktop_width, session_.desktop_height,
              session_.bits_per_pixel, session_.static_channels.size(), session_.user_channel_id);
    state_ = State::wait_erect_domain;
    return {};
}

Result<void> Connection::on_domain_pdu(Reader& data)
{
    const std::size_t start = data.offset();
    FARLAND_TRY(const auto pdu, mcs::decode_domain_pdu(data));
    if (std::holds_alternative<mcs::DisconnectProviderUltimatum>(pdu)) {
        close("client disconnected");
        return {};
    }
    switch (state_) {
    case State::wait_erect_domain:
        if (!std::holds_alternative<mcs::ErectDomainRequest>(pdu)) {
            return farland::fail(Errc::invalid_value, "expected MCS Erect Domain Request", start);
        }
        state_ = State::wait_attach_user;
        return {};
    case State::wait_attach_user: {
        if (!std::holds_alternative<mcs::AttachUserRequest>(pdu)) {
            return farland::fail(Errc::invalid_value, "expected MCS Attach User Request", start);
        }
        const std::size_t tpkt = proto::begin_data_tpdu(output_);
        mcs::encode(output_, mcs::AttachUserConfirm{mcs::ResultCode::successful, session_.user_channel_id});
        proto::end_tpkt(output_, tpkt);
        state_ = State::wait_channel_joins;
        return {};
    }
    default:
        break;
    }

    if (const auto* join = std::get_if<mcs::ChannelJoinRequest>(&pdu)) {
        if (state_ != State::wait_channel_joins) {
            return farland::fail(Errc::invalid_value, "MCS Channel Join Request after the joins", start);
        }
        return on_channel_join(join->initiator, join->channel_id);
    }
    const auto* send = std::get_if<mcs::SendDataRequest>(&pdu);
    if (send == nullptr) {
        return farland::fail(Errc::invalid_value, "unexpected MCS domain PDU", start);
    }
    if (send->initiator != session_.user_channel_id) {
        return farland::fail(Errc::invalid_value, "MCS data from an unknown user", start);
    }
    Reader payload(send->data, start);
    if (send->channel_id == mcs::io_channel_id) {
        if (state_ == State::wait_channel_joins) {
            return on_client_info(payload);
        }
        return on_io_data(payload);
    }
    if (session_.message_channel_id && send->channel_id == *session_.message_channel_id) {
        log::debug(log_component, "ignoring {} bytes on the message channel", send->data.size());
        return {};
    }
    const bool known = std::ranges::any_of(session_.static_channels,
                                           [id = send->channel_id](const auto& c) { return c.second == id; });
    if (!known) {
        return farland::fail(Errc::invalid_value, "MCS data on an unknown channel", start);
    }
    events_.emplace_back(event::ChannelData{send->channel_id, {send->data.begin(), send->data.end()}});
    return {};
}

Result<void> Connection::on_channel_join(std::uint16_t initiator, std::uint16_t channel_id)
{
    if (initiator != session_.user_channel_id) {
        return farland::fail(Errc::invalid_value, "Channel Join Request from an unknown user", 0);
    }
    const bool allowed =
        channel_id == session_.user_channel_id || channel_id == mcs::io_channel_id ||
        (session_.message_channel_id && channel_id == *session_.message_channel_id) ||
        std::ranges::any_of(session_.static_channels, [channel_id](const auto& c) { return c.second == channel_id; });
    const auto result = allowed ? mcs::ResultCode::successful : mcs::ResultCode::no_such_channel;
    if (allowed) {
        joined_channels_.push_back(channel_id);
    }
    const std::size_t tpkt = proto::begin_data_tpdu(output_);
    mcs::encode(output_, mcs::ChannelJoinConfirm{result, session_.user_channel_id, channel_id,
                                                 allowed ? std::optional<std::uint16_t>(channel_id) : std::nullopt});
    proto::end_tpkt(output_, tpkt);
    return {};
}

Result<void> Connection::on_client_info(Reader& data)
{
    FARLAND_TRY(const std::uint16_t flags, proto::read_basic_security_header(data));
    if ((flags & proto::sec_flags::info_pkt) == 0) {
        return farland::fail(Errc::invalid_value, "expected the Client Info PDU", data.offset());
    }
    FARLAND_TRY(auto info, proto::decode_client_info(data));
    session_.user_name = info.user_name;
    session_.domain = info.domain;
    session_.info_flags = info.flags;
    log::info(log_component, "client info: user '{}' domain '{}' flags 0x{:x}", info.user_name, info.domain,
              info.flags);
    events_.emplace_back(event::ClientInfo{info.domain, info.user_name, std::move(info.password), info.flags});

    // Licensing is over at once ([MS-RDPBCGR] 2.2.1.12), then capabilities.
    Writer license;
    proto::encode_license_valid_client(license);
    send_io(license.view());
    session_.share_id = 0x00010000U | session_.user_channel_id;
    send_demand_active();
    return {};
}

void Connection::send_demand_active()
{
    proto::DemandActive demand;
    demand.share_id = session_.share_id;
    demand.capabilities = server_capabilities(session_);
    Writer pdu;
    proto::encode_demand_active(pdu, mcs::server_channel_id, demand);
    send_io(pdu.view());
    state_ = State::wait_confirm_active;
}

Result<void> Connection::on_io_data(Reader& data)
{
    FARLAND_TRY(auto control, proto::read_share_control(data));
    switch (control.type) {
    case proto::pdu_type::flow:
        return {};
    case proto::pdu_type::confirm_active:
        if (state_ != State::wait_confirm_active) {
            log::warn(log_component, "ignoring an unexpected Confirm Active");
            return {};
        }
        return on_confirm_active(control.body);
    case proto::pdu_type::data:
        if (state_ == State::wait_confirm_active) {
            return {};  // Data PDUs from before a reactivation; the share they belonged to is gone.
        }
        return on_data_pdu(control.body);
    default:
        log::debug(log_component, "ignoring share control PDU type {}", control.type);
        return {};
    }
}

Result<void> Connection::on_confirm_active(Reader& body)
{
    const std::size_t start = body.offset();
    FARLAND_TRY(auto confirm, proto::decode_confirm_active(body));
    if (confirm.share_id != session_.share_id) {
        return farland::fail(Errc::invalid_value, "Confirm Active for another share", start);
    }
    auto& caps = confirm.capabilities;
    caps.other.clear();  // raw bodies point into the receive buffer
    if (!caps.general || !caps.bitmap) {
        return farland::fail(Errc::invalid_value, "Confirm Active without general or bitmap capabilities", start);
    }
    session_.fastpath_output = (caps.general->extra_flags & caps::general_extra_flags::fastpath_output_supported) != 0;
    session_.no_bitmap_compression_header =
        (caps.general->extra_flags & caps::general_extra_flags::no_bitmap_compression_hdr) != 0;
    session_.max_request_size = caps.multifragment_update ? caps.multifragment_update->max_request_size : 0;
    session_.client_capabilities = std::move(caps);

    // Finalization, server half: Synchronize and Control Cooperate go out at
    // once; Control Granted and Font Map answer the client's PDUs. This is
    // the order FreeRDP's server uses, which every client accepts.
    state_ = State::finalizing;
    send_data_pdu(proto::Synchronize{1, session_.user_channel_id});
    send_data_pdu(proto::Control{proto::control_action::cooperate, 0, 0});
    return {};
}

Result<void> Connection::on_data_pdu(Reader& body)
{
    FARLAND_TRY(auto data, proto::read_share_data(body));
    if (data.share_id != session_.share_id) {
        log::debug(log_component, "ignoring data PDU for share 0x{:x}", data.share_id);
        return {};
    }
    FARLAND_TRY(auto pdu, proto::decode_data_pdu(data));
    if (auto* control = std::get_if<proto::Control>(&pdu)) {
        if (control->action == proto::control_action::request_control) {
            send_data_pdu(proto::Control{proto::control_action::granted_control, session_.user_channel_id,
                                         mcs::server_channel_id});
        }
        return {};
    }
    if (std::holds_alternative<proto::FontList>(pdu)) {
        send_data_pdu(proto::FontMap{});
        if (state_ == State::finalizing) {
            activate();
        }
        return {};
    }
    if (auto* input = std::get_if<proto::InputPdu>(&pdu)) {
        if (!input->events.empty()) {
            events_.emplace_back(event::Input{std::move(input->events)});
        }
        return {};
    }
    if (auto* refresh = std::get_if<proto::RefreshRect>(&pdu)) {
        events_.emplace_back(event::RefreshRequested{std::move(refresh->areas)});
        return {};
    }
    if (auto* suppress = std::get_if<proto::SuppressOutput>(&pdu)) {
        events_.emplace_back(event::OutputSuppressed{!suppress->allow_display_updates, suppress->desktop_rect});
        return {};
    }
    if (std::holds_alternative<proto::ShutdownRequest>(pdu)) {
        events_.emplace_back(event::ShutdownRequested{});
        return {};
    }
    // Synchronize, Persistent Key List, Frame Acknowledge and others need no answer.
    return {};
}

Result<void> Connection::on_fastpath_input(Reader& pdu)
{
    if (state_ != State::finalizing && state_ != State::active && state_ != State::wait_confirm_active) {
        return farland::fail(Errc::invalid_value, "fast-path input before the capability exchange", 0);
    }
    FARLAND_TRY(auto events, proto::decode_fastpath_input(pdu));
    if (!events.empty() && state_ != State::wait_confirm_active) {
        events_.emplace_back(event::Input{std::move(events)});
    }
    return {};
}

void Connection::activate()
{
    state_ = State::active;
    log::info(log_component, "connection active: {}x{} at {} bpp, {} output{}", session_.desktop_width,
              session_.desktop_height, session_.bits_per_pixel, session_.fastpath_output ? "fast-path" : "slow-path",
              reactivating_ ? " (reactivated)" : "");
    events_.emplace_back(event::Activated{reactivating_});
    reactivating_ = false;
}

// Output -------------------------------------------------------------------------

std::size_t Connection::max_update_size() const noexcept
{
    if (!session_.fastpath_output) {
        return slow_path_update_limit;
    }
    if (session_.max_request_size == 0) {
        return proto::fastpath::max_fragment_size;
    }
    return std::min<std::size_t>(session_.max_request_size, std::size_t{1} << 20U);
}

void Connection::send_bitmap_update(std::span<const std::byte> update_data)
{
    if (state_ != State::active) {
        return;
    }
    if (session_.fastpath_output) {
        proto::fastpath::encode_update(output_, proto::fastpath::update_code::bitmap, update_data);
        return;
    }
    Writer pdu;
    proto::write_data_pdu(pdu, session_.share_id, mcs::server_channel_id, proto::pdu_type2::update, update_data);
    send_io(pdu.view());
}

void Connection::reactivate(std::uint16_t width, std::uint16_t height)
{
    if (state_ != State::active) {
        return;
    }
    Writer pdu;
    proto::encode_deactivate_all(pdu, mcs::server_channel_id, proto::DeactivateAll{session_.share_id});
    send_io(pdu.view());
    session_.desktop_width = std::clamp(width, config_.min_desktop_size, config_.max_desktop_size);
    session_.desktop_height = std::clamp(height, config_.min_desktop_size, config_.max_desktop_size);
    session_.share_id += 0x00010000U;  // a new share, as Windows servers do ([MS-RDPBCGR] 4.2)
    reactivating_ = true;
    send_demand_active();
}

void Connection::disconnect(std::uint32_t error_info)
{
    if (state_ == State::closed) {
        return;
    }
    const bool share_exists =
        state_ == State::wait_confirm_active || state_ == State::finalizing || state_ == State::active;
    if (share_exists && session_.supports_error_info && error_info != proto::errinfo::none) {
        send_data_pdu(proto::SetErrorInfo{error_info});
    }
    if (state_ != State::wait_connect_initial) {
        const std::size_t tpkt = proto::begin_data_tpdu(output_);
        mcs::encode(output_, mcs::DisconnectProviderUltimatum{mcs::DisconnectReason::user_requested});
        proto::end_tpkt(output_, tpkt);
    }
    close("server disconnected the session");
}

void Connection::send_io(std::span<const std::byte> payload)
{
    const std::size_t tpkt = proto::begin_data_tpdu(output_);
    mcs::encode(output_, mcs::SendDataIndication{mcs::server_channel_id, mcs::io_channel_id, payload});
    proto::end_tpkt(output_, tpkt);
}

void Connection::send_channel_data(std::uint16_t channel_id, std::span<const std::byte> chunk)
{
    if (state_ == State::closed) {
        return;
    }
    FARLAND_ASSERT(state_ == State::wait_confirm_active || state_ == State::finalizing || state_ == State::active);
    FARLAND_ASSERT(std::ranges::any_of(session_.static_channels,
                                       [channel_id](const auto& channel) { return channel.second == channel_id; }));
    const std::size_t tpkt = proto::begin_data_tpdu(output_);
    mcs::encode(output_, mcs::SendDataIndication{mcs::server_channel_id, channel_id, chunk});
    proto::end_tpkt(output_, tpkt);
}

template <class Pdu>
void Connection::send_data_pdu(const Pdu& pdu)
{
    Writer payload;
    proto::encode(payload, pdu);
    Writer data;
    proto::write_data_pdu(data, session_.share_id, mcs::server_channel_id, proto::type2_of(proto::DataPdu{pdu}),
                          payload.view());
    send_io(data.view());
}

void Connection::fail(std::string reason)
{
    log::warn(log_component, "closing connection in state {}: {}", to_string(state_), reason);
    state_ = State::closed;
    events_.emplace_back(event::Closed{std::move(reason), true});
}

void Connection::close(std::string reason)
{
    log::info(log_component, "{}", reason);
    state_ = State::closed;
    events_.emplace_back(event::Closed{std::move(reason), false});
}

}  // namespace farland::server
