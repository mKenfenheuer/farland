// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Drives server::Connection through the whole connection sequence with a
// scripted client built from farland's own client-side encoders.

#include <farland/base/hexdump.hpp>
#include <farland/proto/bitmap.hpp>
#include <farland/proto/client_info.hpp>
#include <farland/proto/fastpath.hpp>
#include <farland/proto/framing.hpp>
#include <farland/proto/gcc.hpp>
#include <farland/proto/license.hpp>
#include <farland/proto/mcs.hpp>
#include <farland/proto/security.hpp>
#include <farland/proto/share.hpp>
#include <farland/proto/x224.hpp>
#include <farland/server/connection.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

namespace proto = farland::proto;
namespace mcs = farland::proto::mcs;
namespace gcc = farland::proto::gcc;
namespace caps = farland::proto::caps;
namespace ev = farland::server::event;
using farland::Reader;
using farland::Writer;
using farland::server::Connection;
using farland::server::State;

namespace {

using Bytes = std::vector<std::byte>;
constexpr std::uint16_t user_id = 1007;

template <class Encode>
Bytes x224_data(Encode&& encode)
{
    Writer w;
    const auto start = proto::begin_data_tpdu(w);
    encode(w);
    proto::end_tpkt(w, start);
    return std::move(w).take();
}

template <class Pdu>
Bytes domain(const Pdu& pdu)
{
    return x224_data([&pdu](Writer& w) { mcs::encode(w, pdu); });
}

Bytes io(std::span<const std::byte> payload, std::uint16_t channel = mcs::io_channel_id)
{
    return domain(mcs::SendDataRequest{user_id, channel, payload});
}

gcc::ClientData client_data(std::uint32_t selected_protocol, std::uint16_t early_flags)
{
    gcc::ClientData data;
    data.core.desktop_width = 1024;
    data.core.desktop_height = 768;
    data.core.client_name = "unit-test";
    data.core.post_beta2_color_depth = 0xCA01;
    data.core.client_product_id = 1;
    data.core.serial_number = 0;
    data.core.high_color_depth = 24;
    data.core.supported_color_depths = 0x0f;
    data.core.early_capability_flags = early_flags;
    data.core.client_dig_product_id = "";
    data.core.connection_type = 6;
    data.core.server_selected_protocol = selected_protocol;
    data.network = gcc::ClientNetworkData{{{"rdpdr", 0x80800000}, {"cliprdr", 0xc0a00000}}};
    data.message_channel = gcc::ClientMessageChannelData{0};
    return data;
}

Bytes connect_initial(const gcc::ClientData& data)
{
    Writer blocks;
    gcc::encode_client_data(blocks, data);
    Writer conference;
    gcc::encode_conference_create_request(conference, blocks.view());
    const std::array selector{std::byte{0x01}};
    mcs::ConnectInitial initial;
    initial.calling_domain_selector = selector;
    initial.called_domain_selector = selector;
    initial.target = {34, 2, 0, 1, 0, 1, 65535, 2};
    initial.minimum = {1, 1, 1, 1, 0, 1, 1056, 2};
    initial.maximum = {65535, 64535, 65535, 1, 0, 1, 65535, 2};
    initial.user_data = conference.view();
    return x224_data([&initial](Writer& w) { mcs::encode_connect_initial(w, initial); });
}

Bytes client_info()
{
    proto::ClientInfo info;
    info.flags = proto::info_flags::unicode | proto::info_flags::autologon;
    info.user_name = "alice";
    info.domain = "LAB";
    info.password = farland::SecretString("pw");
    Writer w;
    proto::write_basic_security_header(w, proto::sec_flags::info_pkt);
    proto::encode_client_info(w, info);
    return io(w.view());
}

Bytes confirm_active(std::uint32_t share_id, bool fastpath_output, std::uint16_t width = 1024,
                     std::uint16_t height = 768, std::optional<caps::Pointer> pointer = std::nullopt,
                     std::optional<caps::LargePointer> large_pointer = std::nullopt)
{
    proto::ConfirmActive confirm;
    confirm.share_id = share_id;
    auto& sets = confirm.capabilities;
    sets.general = caps::General{};
    sets.general->extra_flags = caps::general_extra_flags::no_bitmap_compression_hdr |
                                (fastpath_output ? caps::general_extra_flags::fastpath_output_supported : 0U);
    sets.bitmap = caps::Bitmap{};
    sets.bitmap->desktop_width = width;
    sets.bitmap->desktop_height = height;
    sets.input = caps::Input{};
    sets.input->input_flags = caps::input_flags::scancodes | caps::input_flags::fastpath_input2;
    sets.multifragment_update = caps::MultifragmentUpdate{0x100000};
    sets.pointer = pointer;
    sets.large_pointer = large_pointer;
    Writer w;
    proto::encode_confirm_active(w, user_id, confirm);
    return io(w.view());
}

template <class Pdu>
Bytes data_pdu(std::uint32_t share_id, const Pdu& pdu)
{
    Writer payload;
    proto::encode(payload, pdu);
    Writer w;
    proto::write_data_pdu(w, share_id, user_id, proto::type2_of(proto::DataPdu{pdu}), payload.view());
    return io(w.view());
}

/// Splits server output into TPKT and fast-path PDUs.
struct Output {
    std::vector<Bytes> tpkt;
    std::vector<Bytes> fastpath;
};

Output split(std::span<const std::byte> bytes)
{
    Output out;
    while (!bytes.empty()) {
        const auto frame = proto::peek_frame(bytes).value();
        REQUIRE(frame.has_value());
        REQUIRE(bytes.size() >= frame->length);
        Bytes pdu(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(frame->length));
        (frame->kind == proto::FrameKind::tpkt ? out.tpkt : out.fastpath).push_back(std::move(pdu));
        bytes = bytes.subspan(frame->length);
    }
    return out;
}

/// The Send Data Indication payload of a server TPKT.
Bytes indication(const Bytes& tpkt)
{
    Reader r(tpkt);
    Reader tpdu = proto::read_tpkt(r).value();
    Reader data = proto::decode_data_tpdu(tpdu).value();
    const auto pdu = mcs::decode_domain_pdu(data).value();
    const auto& send = std::get<mcs::SendDataIndication>(pdu);
    CHECK(send.initiator == mcs::server_channel_id);
    CHECK(send.channel_id == mcs::io_channel_id);
    return {send.data.begin(), send.data.end()};
}

mcs::DomainPdu domain_of(const Bytes& tpkt, Bytes& storage)
{
    Reader r(tpkt);
    Reader tpdu = proto::read_tpkt(r).value();
    Reader data = proto::decode_data_tpdu(tpdu).value();
    storage.assign(data.rest().begin(), data.rest().end());
    Reader d(storage);
    return mcs::decode_domain_pdu(d).value();
}

/// Server data PDUs in the output, decoded.
std::vector<proto::DataPdu> data_pdus(const Output& out, std::vector<Bytes>& storage)
{
    std::vector<proto::DataPdu> pdus;
    for (const auto& tpkt : out.tpkt) {
        storage.push_back(indication(tpkt));
        Reader r(storage.back());
        auto control = proto::read_share_control(r).value();
        REQUIRE(control.type == proto::pdu_type::data);
        auto data = proto::read_share_data(control.body).value();
        pdus.push_back(proto::decode_data_pdu(data).value());
    }
    return pdus;
}

std::vector<farland::server::Event> events(Connection& c)
{
    std::vector<farland::server::Event> out;
    while (auto e = c.poll_event()) {
        out.push_back(std::move(*e));
    }
    return out;
}

/// What PreAuth hands over for a TLS-only client (PreAuth has its own tests).
farland::server::Negotiation tls_negotiation()
{
    return {"tester", proto::protocol::ssl | proto::protocol::hybrid | proto::protocol::hybrid_ex, proto::protocol::ssl,
            std::nullopt};
}

/// Runs the connection sequence up to (not including) the Confirm Active and
/// returns the Demand Active's share ID.
std::uint32_t connect_until_demand_active(Connection& c, std::uint16_t early_flags = 0x0001,
                                          proto::DemandActive* demand = nullptr)
{
    c.receive(connect_initial(client_data(proto::protocol::ssl, early_flags)));
    static_cast<void>(c.take_output());
    Bytes both = domain(mcs::ErectDomainRequest{});
    const Bytes attach = domain(mcs::AttachUserRequest{});
    both.insert(both.end(), attach.begin(), attach.end());
    c.receive(both);
    static_cast<void>(c.take_output());
    for (const std::uint16_t id : {user_id, mcs::io_channel_id}) {
        c.receive(domain(mcs::ChannelJoinRequest{user_id, id}));
    }
    static_cast<void>(c.take_output());
    c.receive(client_info());
    const auto out = split(c.take_output());
    const auto payload = indication(out.tpkt.at(1));
    Reader r(payload);
    auto control = proto::read_share_control(r).value();
    auto decoded = proto::decode_demand_active(control.body).value();
    if (demand != nullptr) {
        decoded.capabilities.other.clear();  // raw bodies point into `payload`
        *demand = decoded;
    }
    return decoded.share_id;
}

/// An active connection whose client sent these pointer capabilities.
std::uint32_t activate_with_pointer(Connection& c, bool fastpath_output, std::optional<caps::Pointer> pointer,
                                    std::optional<caps::LargePointer> large_pointer)
{
    const auto share_id = connect_until_demand_active(c);
    c.receive(confirm_active(share_id, fastpath_output, 1024, 768, pointer, large_pointer));
    c.receive(data_pdu(share_id, proto::Synchronize{1, mcs::server_channel_id}));
    c.receive(data_pdu(share_id, proto::Control{proto::control_action::cooperate, 0, 0}));
    c.receive(data_pdu(share_id, proto::Control{proto::control_action::request_control, 0, 0}));
    c.receive(data_pdu(share_id, proto::FontList{}));
    static_cast<void>(c.take_output());
    REQUIRE(c.active());
    return share_id;
}

/// A small 32 bpp New Pointer.
proto::PointerUpdate small_pointer()
{
    const std::vector<std::byte> pixels{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{0xFF},
                                        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
    auto shape = proto::pointer::shape_from_bgra(pixels, 2, 1, 32);
    shape.cache_index = 3;
    return proto::pointer::NewPointer{shape};
}

void finalize(Connection& c, std::uint32_t share_id)
{
    c.receive(data_pdu(share_id, proto::Synchronize{1, mcs::server_channel_id}));
    c.receive(data_pdu(share_id, proto::Control{proto::control_action::cooperate, 0, 0}));
    c.receive(data_pdu(share_id, proto::Control{proto::control_action::request_control, 0, 0}));
    c.receive(data_pdu(share_id, proto::FontList{}));
}

}  // namespace

TEST_CASE("The full connection sequence, step by step")
{
    Connection c({}, tls_negotiation());

    // MCS Connect: channels 1004/1005 static, 1006 message, 1007 user.
    c.receive(connect_initial(client_data(proto::protocol::ssl, 0x0001)));
    auto out = split(c.take_output());
    REQUIRE(out.tpkt.size() == 1);
    {
        Reader r(out.tpkt[0]);
        Reader tpdu = proto::read_tpkt(r).value();
        Reader data = proto::decode_data_tpdu(tpdu).value();
        const auto response = mcs::decode_connect_response(data).value();
        CHECK(response.parameters == mcs::DomainParameters{34, 3, 0, 1, 0, 1, 65528, 2});
        Reader ud(response.user_data);
        const auto blocks = gcc::decode_conference_create_response(ud).value();
        Reader br(blocks);
        const auto server = gcc::decode_server_data(br).value();
        CHECK(server.core.client_requested_protocols ==
              (proto::protocol::ssl | proto::protocol::hybrid | proto::protocol::hybrid_ex));
        CHECK(server.core.early_capability_flags == 0);
        CHECK(server.security.encryption_method == 0);
        CHECK(server.network.io_channel_id == mcs::io_channel_id);
        CHECK(server.network.channel_ids == std::vector<std::uint16_t>{1004, 1005});
        CHECK(server.message_channel_id == 1006);
    }
    CHECK(c.session().user_channel_id == user_id);
    CHECK(c.session().bits_per_pixel == 32);

    // Erect Domain and Attach User in one TCP segment.
    Bytes both = domain(mcs::ErectDomainRequest{});
    const Bytes attach = domain(mcs::AttachUserRequest{});
    both.insert(both.end(), attach.begin(), attach.end());
    c.receive(both);
    out = split(c.take_output());
    REQUIRE(out.tpkt.size() == 1);
    Bytes storage;
    CHECK(std::get<mcs::AttachUserConfirm>(domain_of(out.tpkt[0], storage)).initiator == user_id);
    // mstsc needs the optional-field-present bit set (docs/PLAN.md §4.1).
    CHECK(std::to_integer<unsigned>(out.tpkt[0].at(7)) == 0x2E);

    // Channel joins, including one for a channel that does not exist.
    for (const std::uint16_t id :
         {std::uint16_t{1007}, std::uint16_t{1003}, std::uint16_t{1004}, std::uint16_t{1005}, std::uint16_t{1006}}) {
        c.receive(domain(mcs::ChannelJoinRequest{user_id, id}));
        out = split(c.take_output());
        const auto confirm = std::get<mcs::ChannelJoinConfirm>(domain_of(out.tpkt.at(0), storage));
        CHECK(confirm.result == mcs::ResultCode::successful);
        CHECK(confirm.channel_id == id);
    }
    c.receive(domain(mcs::ChannelJoinRequest{user_id, 1999}));
    out = split(c.take_output());
    CHECK(std::get<mcs::ChannelJoinConfirm>(domain_of(out.tpkt.at(0), storage)).result ==
          mcs::ResultCode::no_such_channel);

    // Client Info: license, then Demand Active.
    c.receive(client_info());
    auto evs = events(c);
    REQUIRE(evs.size() == 1);
    const auto& info = std::get<ev::ClientInfo>(evs[0]);
    CHECK(info.user_name == "alice");
    CHECK(info.domain == "LAB");
    CHECK(info.password.view() == "pw");
    out = split(c.take_output());
    REQUIRE(out.tpkt.size() == 2);
    {
        const auto license = indication(out.tpkt[0]);
        Reader r(license);
        CHECK(proto::decode_license_valid_client(r).has_value());
    }
    std::uint32_t share_id = 0;
    {
        const auto payload = indication(out.tpkt[1]);
        Reader r(payload);
        auto control = proto::read_share_control(r).value();
        CHECK(control.type == proto::pdu_type::demand_active);
        CHECK(control.source == mcs::server_channel_id);
        const auto demand = proto::decode_demand_active(control.body).value();
        share_id = demand.share_id;
        CHECK(share_id == 0x000103ef);
        CHECK(demand.capabilities.bitmap->desktop_width == 1024);
        CHECK(demand.capabilities.bitmap->preferred_bits_per_pixel == 32);
        CHECK((demand.capabilities.general->extra_flags & caps::general_extra_flags::fastpath_output_supported) != 0);
        CHECK((demand.capabilities.input->input_flags & caps::input_flags::fastpath_input2) != 0);
        CHECK(demand.capabilities.share->node_id == mcs::server_channel_id);
        // mstsc refuses servers without a MultifragmentUpdate capability (0x1204, §4.1).
        CHECK(demand.capabilities.multifragment_update.has_value());
    }
    CHECK(c.state() == State::wait_confirm_active);

    // Confirm Active: the server answers with Synchronize and Control Cooperate.
    c.receive(confirm_active(share_id, true));
    out = split(c.take_output());
    std::vector<Bytes> keep;
    auto pdus = data_pdus(out, keep);
    REQUIRE(pdus.size() == 2);
    CHECK(std::get<proto::Synchronize>(pdus[0]).target_user == user_id);
    CHECK(std::get<proto::Control>(pdus[1]).action == proto::control_action::cooperate);
    CHECK(c.state() == State::finalizing);

    // Client finalization: Control Granted, then Font Map, then active.
    c.receive(data_pdu(share_id, proto::Synchronize{1, mcs::server_channel_id}));
    c.receive(data_pdu(share_id, proto::Control{proto::control_action::cooperate, 0, 0}));
    c.receive(data_pdu(share_id, proto::Control{proto::control_action::request_control, 0, 0}));
    out = split(c.take_output());
    pdus = data_pdus(out, keep);
    REQUIRE(pdus.size() == 1);
    const auto granted = std::get<proto::Control>(pdus[0]);
    CHECK(granted.action == proto::control_action::granted_control);
    CHECK(granted.grant_id == user_id);
    CHECK(granted.control_id == mcs::server_channel_id);
    c.receive(data_pdu(share_id, proto::FontList{}));
    out = split(c.take_output());
    pdus = data_pdus(out, keep);
    REQUIRE(pdus.size() == 1);
    CHECK(std::holds_alternative<proto::FontMap>(pdus[0]));
    evs = events(c);
    REQUIRE(evs.size() == 1);
    CHECK_FALSE(std::get<ev::Activated>(evs[0]).reactivation);
    CHECK(c.active());
    CHECK(c.session().fastpath_output);
    CHECK(c.session().no_bitmap_compression_header);
    CHECK(c.max_update_size() == 0x100000);

    // Fast-path input becomes an Input event.
    const std::array input{proto::InputEvent{proto::MouseEvent{proto::ptr_flags::move, 10, 20}}};
    Writer fp;
    proto::encode_fastpath_input(fp, input);
    c.receive(fp.view());
    evs = events(c);
    REQUIRE(evs.size() == 1);
    const auto mouse = std::get<proto::MouseEvent>(std::get<ev::Input>(evs[0]).events.at(0));
    CHECK(mouse.x == 10);
    CHECK(mouse.y == 20);

    // Bitmap updates go out as fast-path PDUs.
    const std::array update{std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    c.send_bitmap_update(update);
    out = split(c.take_output());
    REQUIRE(out.fastpath.size() == 1);
    {
        Reader r(out.fastpath[0]);
        const auto fragments = proto::fastpath::decode_output_pdu(r).value();
        CHECK(fragments.at(0).code == proto::fastpath::update_code::bitmap);
    }

    // Refresh Rect and Suppress Output become events.
    c.receive(data_pdu(share_id, proto::RefreshRect{{{0, 0, 63, 63}}}));
    c.receive(data_pdu(share_id, proto::SuppressOutput{false, std::nullopt}));
    evs = events(c);
    REQUIRE(evs.size() == 2);
    CHECK(std::get<ev::RefreshRequested>(evs[0]).areas.size() == 1);
    CHECK(std::get<ev::OutputSuppressed>(evs[1]).suppressed);

    // Reactivation with a new size and a new share.
    c.reactivate(800, 600);
    out = split(c.take_output());
    REQUIRE(out.tpkt.size() == 2);
    std::uint32_t new_share = 0;
    {
        const auto deactivate = indication(out.tpkt[0]);
        Reader r(deactivate);
        CHECK(proto::read_share_control(r).value().type == proto::pdu_type::deactivate_all);
        const auto demand_payload = indication(out.tpkt[1]);
        Reader d(demand_payload);
        auto control = proto::read_share_control(d).value();
        const auto demand = proto::decode_demand_active(control.body).value();
        new_share = demand.share_id;
        CHECK(new_share == share_id + 0x10000);
        CHECK(demand.capabilities.bitmap->desktop_width == 800);
    }
    c.receive(confirm_active(new_share, true, 800, 600));
    static_cast<void>(c.take_output());
    finalize(c, new_share);
    static_cast<void>(c.take_output());  // Control Granted and Font Map
    evs = events(c);
    REQUIRE(evs.size() == 1);
    CHECK(std::get<ev::Activated>(evs[0]).reactivation);
    CHECK(c.session().desktop_width == 800);

    // Orderly disconnect: Set Error Info, then the Disconnect Provider Ultimatum.
    c.disconnect(proto::errinfo::rpc_initiated_disconnect);
    out = split(c.take_output());
    REQUIRE(out.tpkt.size() == 2);
    pdus = data_pdus(Output{{out.tpkt[0]}, {}}, keep);
    CHECK(std::get<proto::SetErrorInfo>(pdus[0]).error_info == proto::errinfo::rpc_initiated_disconnect);
    CHECK(std::holds_alternative<mcs::DisconnectProviderUltimatum>(domain_of(out.tpkt[1], storage)));
    evs = events(c);
    REQUIRE(evs.size() == 1);
    CHECK_FALSE(std::get<ev::Closed>(evs[0]).error);
    CHECK(c.state() == State::closed);
}

TEST_CASE("A serverSelectedProtocol that differs from the negotiation closes the connection")
{
    Connection c({}, tls_negotiation());
    c.receive(connect_initial(client_data(proto::protocol::hybrid, 0)));
    CHECK(c.state() == State::closed);
    const auto evs = events(c);
    REQUIRE(evs.size() == 1);
    CHECK(std::holds_alternative<ev::Closed>(evs[0]));
}

TEST_CASE("Garbage and out-of-order PDUs close the connection with an error")
{
    Connection garbage({}, tls_negotiation());
    const std::array junk{std::byte{0xff}, std::byte{0xff}};
    garbage.receive(junk);
    CHECK(garbage.state() == State::closed);
    CHECK(std::get<ev::Closed>(garbage.poll_event().value()).error);

    Connection skipped({}, tls_negotiation());
    connect_until_demand_active(skipped);
    const std::array input{proto::InputEvent{proto::KeyboardEvent{0, 0x1e}}};
    Writer fp;
    proto::encode_fastpath_input(fp, input);
    skipped.receive(fp.view());  // input before the Confirm Active is ignored, not fatal
    CHECK(skipped.state() == State::wait_confirm_active);
}

TEST_CASE("Skip-channel-join clients go straight from Attach User to Client Info")
{
    Connection c({}, tls_negotiation());
    c.receive(connect_initial(client_data(proto::protocol::ssl, gcc::cs_early_flags::support_skip_channeljoin)));
    const auto out = split(c.take_output());
    {
        Reader r(out.tpkt.at(0));
        Reader tpdu = proto::read_tpkt(r).value();
        Reader data = proto::decode_data_tpdu(tpdu).value();
        const auto response = mcs::decode_connect_response(data).value();
        Reader ud(response.user_data);
        const auto blocks = gcc::decode_conference_create_response(ud).value();
        Reader br(blocks);
        CHECK(gcc::decode_server_data(br).value().core.early_capability_flags ==
              gcc::sc_early_flags::skip_channeljoin_supported);
    }
    c.receive(domain(mcs::ErectDomainRequest{}));
    c.receive(domain(mcs::AttachUserRequest{}));
    static_cast<void>(c.take_output());
    c.receive(client_info());
    CHECK(c.state() == State::wait_confirm_active);
}

TEST_CASE("Without fast-path output, bitmap updates use slow-path Update PDUs")
{
    Connection c({}, tls_negotiation());
    const auto share_id = connect_until_demand_active(c);
    c.receive(confirm_active(share_id, false));
    finalize(c, share_id);
    static_cast<void>(c.take_output());
    REQUIRE(c.active());
    CHECK_FALSE(c.session().fastpath_output);
    CHECK(c.max_update_size() < 0x3FFF);

    const std::array update{std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    c.send_bitmap_update(update);
    const auto out = split(c.take_output());
    REQUIRE(out.tpkt.size() == 1);
    const auto payload = indication(out.tpkt[0]);
    Reader r(payload);
    auto control = proto::read_share_control(r).value();
    const auto data = proto::read_share_data(control.body).value();
    CHECK(data.type2 == proto::pdu_type2::update);
}

TEST_CASE("A client Shutdown Request is reported, and the client's ultimatum closes")
{
    Connection c({}, tls_negotiation());
    const auto share_id = connect_until_demand_active(c);
    c.receive(confirm_active(share_id, true));
    finalize(c, share_id);
    static_cast<void>(events(c));
    c.receive(data_pdu(share_id, proto::ShutdownRequest{}));
    CHECK(std::holds_alternative<ev::ShutdownRequested>(c.poll_event().value()));
    c.receive(domain(mcs::DisconnectProviderUltimatum{}));
    const auto closed = std::get<ev::Closed>(c.poll_event().value());
    CHECK_FALSE(closed.error);
}

TEST_CASE("Static channel data goes out on the channel's own MCS ID")
{
    Connection c({}, tls_negotiation());
    const auto share_id = connect_until_demand_active(c);
    c.receive(confirm_active(share_id, true));
    finalize(c, share_id);
    static_cast<void>(c.take_output());
    REQUIRE(c.active());

    const auto rdpdr = c.session().static_channel_id("RDPDR");  // names compare without regard to case
    REQUIRE(rdpdr.has_value());
    CHECK_FALSE(c.session().static_channel_id("drdynvc").has_value());
    CHECK_FALSE(c.session().supports_gfx());

    const std::array chunk{std::byte{0x04}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                           std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    c.send_channel_data(*rdpdr, chunk);
    const auto out = split(c.take_output());
    REQUIRE(out.tpkt.size() == 1);
    Bytes storage;
    const auto pdu = domain_of(out.tpkt[0], storage);
    const auto& send = std::get<mcs::SendDataIndication>(pdu);
    CHECK(send.initiator == mcs::server_channel_id);
    CHECK(send.channel_id == *rdpdr);
    CHECK(Bytes(send.data.begin(), send.data.end()) == Bytes(chunk.begin(), chunk.end()));
}

TEST_CASE("GFX needs the early flag, the drdynvc channel and 32 bpp")
{
    farland::server::Session s;
    s.static_channels = {{"rdpdr", 1004}, {"drdynvc", 1005}};
    s.client_data.core.early_capability_flags = gcc::cs_early_flags::support_dynvc_gfx_protocol;
    s.bits_per_pixel = 32;
    CHECK(s.supports_gfx());
    CHECK(s.static_channel_id("DRDYNVC") == 1005);
    s.bits_per_pixel = 16;
    CHECK_FALSE(s.supports_gfx());
    s.bits_per_pixel = 32;
    s.client_data.core.early_capability_flags = 0;
    CHECK_FALSE(s.supports_gfx());
}

TEST_CASE("The server advertises pointer caches and large pointers; the client's pointer caps are negotiated")
{
    Connection c({}, tls_negotiation());
    proto::DemandActive demand;
    static_cast<void>(connect_until_demand_active(c, 0x0001, &demand));
    REQUIRE(demand.capabilities.pointer.has_value());
    CHECK(demand.capabilities.pointer->color_pointer_flag == 1);
    CHECK(demand.capabilities.pointer->color_pointer_cache_size == 25);
    CHECK(demand.capabilities.pointer->pointer_cache_size == 25);
    REQUIRE(demand.capabilities.large_pointer.has_value());
    CHECK(demand.capabilities.large_pointer->support_flags ==
          (caps::large_pointer_flags::size_96x96 | caps::large_pointer_flags::size_384x384));

    // mstsc's values ([MS-RDPBCGR] 4.1.13): 20 color, 21 pointer slots.
    Connection mstsc({}, tls_negotiation());
    activate_with_pointer(mstsc, true, caps::Pointer{1, 20, 21}, caps::LargePointer{0x0001});
    CHECK(mstsc.session().pointer.color_pointer_cache_size == 20);
    CHECK(mstsc.session().pointer.pointer_cache_size == 21);
    CHECK(mstsc.session().pointer.large_pointer_flags == caps::large_pointer_flags::size_96x96);

    // More slots than the server offers, unknown flags, no pointerCacheSize.
    Connection greedy({}, tls_negotiation());
    activate_with_pointer(greedy, true, caps::Pointer{1, 200, std::nullopt}, caps::LargePointer{0x0007});
    CHECK(greedy.session().pointer.color_pointer_cache_size == 25);
    CHECK(greedy.session().pointer.pointer_cache_size == 0);
    CHECK(greedy.session().pointer.large_pointer_flags == 0x0003);

    Connection none({}, tls_negotiation());
    activate_with_pointer(none, true, std::nullopt, std::nullopt);
    CHECK(none.session().pointer.color_pointer_cache_size == 0);
    CHECK(none.session().pointer.pointer_cache_size == 0);
    CHECK(none.session().pointer.large_pointer_flags == 0);
}

TEST_CASE("Pointer updates go out as fast-path updates when negotiated")
{
    Connection c({}, tls_negotiation());
    activate_with_pointer(c, true, caps::Pointer{1, 25, 25}, caps::LargePointer{0x0003});

    for (const proto::PointerUpdate& update :
         {proto::PointerUpdate{proto::pointer::Hidden{}}, proto::PointerUpdate{proto::pointer::Position{3, 4}},
          proto::PointerUpdate{proto::pointer::CachedPointer{2}}, small_pointer()}) {
        c.send_pointer(update);
        const auto out = split(c.take_output());
        REQUIRE(out.tpkt.empty());
        REQUIRE(out.fastpath.size() == 1);
        Reader r(out.fastpath[0]);
        const auto fragments = proto::fastpath::decode_output_pdu(r).value();
        REQUIRE(fragments.size() == 1);
        CHECK(fragments[0].code == proto::pointer::fastpath_code(update));
        Reader data(fragments[0].data);
        CHECK(proto::pointer::decode_fastpath(fragments[0].code, data).value() == update);
    }

    // A 384x384 pointer spans several fragments and reassembles.
    const std::vector<std::byte> pixels(std::size_t{384} * 384 * 4, std::byte{0x7F});
    auto shape = proto::pointer::shape_from_bgra(pixels, 384, 384, 32);
    const proto::PointerUpdate large = proto::pointer::LargePointer{shape};
    c.send_pointer(large);
    const auto out = split(c.take_output());
    CHECK(out.fastpath.size() > 1);
    proto::fastpath::Reassembler reassembler(c.max_update_size());
    std::optional<proto::fastpath::Reassembler::Update> whole;
    for (const auto& pdu : out.fastpath) {
        Reader r(pdu);
        const auto fragments = proto::fastpath::decode_output_pdu(r).value();
        for (const auto& fragment : fragments) {
            if (auto update = reassembler.add(fragment).value()) {
                whole = std::move(update);
            }
        }
    }
    REQUIRE(whole.has_value());
    CHECK(whole->code == proto::fastpath::update_code::large_pointer);
    Reader data(whole->data);
    CHECK(proto::pointer::decode_fastpath(whole->code, data).value() == large);
}

TEST_CASE("Without fast-path output, pointer updates are slow-path Pointer Update PDUs")
{
    Connection c({}, tls_negotiation());
    const auto share_id = activate_with_pointer(c, false, caps::Pointer{1, 25, 25}, std::nullopt);
    for (const proto::PointerUpdate& update : {proto::PointerUpdate{proto::pointer::Default{}},
                                               proto::PointerUpdate{proto::pointer::Position{3, 4}}, small_pointer()}) {
        c.send_pointer(update);
        const auto out = split(c.take_output());
        REQUIRE(out.fastpath.empty());
        REQUIRE(out.tpkt.size() == 1);
        const auto payload = indication(out.tpkt[0]);
        Reader r(payload);
        auto control = proto::read_share_control(r).value();
        CHECK(control.type == proto::pdu_type::data);
        auto data = proto::read_share_data(control.body).value();
        CHECK(data.share_id == share_id);
        CHECK(data.type2 == proto::pdu_type2::pointer);
        CHECK(proto::pointer::decode_slow_path(data.payload).value() == update);
    }
}

TEST_CASE("Pointer updates before activation are dropped")
{
    Connection c({}, tls_negotiation());
    static_cast<void>(connect_until_demand_active(c));
    c.send_pointer(proto::pointer::Hidden{});
    CHECK(c.take_output().empty());
}
