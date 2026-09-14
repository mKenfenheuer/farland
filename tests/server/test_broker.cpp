// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/writer.hpp>
#include <farland/proto/share.hpp>
#include <farland/proto/x224.hpp>
#include <farland/server/broker.hpp>

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string_view>

namespace broker = farland::server::broker;
namespace proto = farland::proto;
using farland::Errc;
using farland::Writer;

namespace {

broker::Token token_of(std::uint8_t seed)
{
    broker::Token token{};
    for (std::size_t i = 0; i < token.size(); ++i) {
        token.at(i) = static_cast<std::byte>(seed + i);
    }
    return token;
}

broker::NewConnection full_connection()
{
    broker::NewConnection m;
    m.connection_id = 7;
    m.negotiation = {"alice", proto::protocol::hybrid_ex | proto::protocol::ssl, proto::protocol::hybrid_ex,
                     farland::auth::Identity{"alice", "LAB"}};
    m.peer = "192.0.2.10:50123";
    m.elapsed_ms = 850;
    broker::ClientSummary client;
    client.desktop_width = 2560;
    client.desktop_height = 1440;
    client.keyboard_layout = 0x0407;
    client.keyboard_type = 4;
    client.client_build = 26100;
    client.client_name = "WORKSTATION";
    client.desktop_scale_factor = 150;
    client.monitors = {{0, 0, 2559, 1439, true}, {-1920, 0, -1, 1079, false}};
    m.client = client;
    broker::ReconnectCookie cookie;
    cookie.logon_id = 0x1234;
    cookie.security_verifier.fill(std::byte{0x5A});
    m.auto_reconnect = cookie;
    m.pending_input = {std::byte{0x03}, std::byte{0x00}, std::byte{0x01}, std::byte{0x2C}};
    return m;
}

/// A frame around whatever `body` writes.
std::vector<std::byte> frame(const std::function<void(Writer&)>& body)
{
    Writer w;
    w.u32le(0);
    body(w);
    w.patch_u32le(0, static_cast<std::uint32_t>(w.size() - 4));
    return std::move(w).take();
}

/// NewConnection up to and including elapsed_ms.
void connection_prefix(Writer& w, std::uint32_t selected = proto::protocol::ssl, bool identity = false)
{
    w.u8(2);     // NewConnection
    w.u64le(1);  // connection id
    w.u16le(0);  // cookie
    w.u32le(selected);
    w.u32le(selected);
    w.u8(identity ? 1 : 0);
    if (identity) {
        w.u16le(5);
        w.bytes(std::as_bytes(std::span(std::string_view("alice"))));
        w.u16le(0);
    }
    w.u16le(0);  // peer
    w.u32le(0);  // elapsed
}

/// A client summary up to the monitor count.
void client_prefix(Writer& w)
{
    w.u16le(1024);
    w.u16le(768);
    w.u32le(0x0409);
    w.u32le(4);
    w.u32le(0);
    w.u32le(22000);
    w.u16le(0);  // client name
    w.u8(0);     // no scale factor
}

void check_error(const std::vector<std::byte>& bytes, Errc code)
{
    const auto decoded = broker::decode(bytes);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == code);
}

}  // namespace

TEST_CASE("Broker messages round-trip")
{
    const auto hello_frame = broker::encode(broker::Hello{broker::protocol_version, token_of(1)});
    const auto hello = std::get<broker::Hello>(broker::decode(hello_frame).value());
    CHECK(hello.version == broker::protocol_version);
    CHECK(hello.token == token_of(1));

    const auto connection_frame = broker::encode(full_connection());
    const auto connection = std::get<broker::NewConnection>(broker::decode(connection_frame).value());
    CHECK(connection.connection_id == 7);
    CHECK(connection.negotiation.cookie == "alice");
    CHECK(connection.negotiation.selected_protocol == proto::protocol::hybrid_ex);
    REQUIRE(connection.negotiation.identity.has_value());
    CHECK(connection.negotiation.identity->user == "alice");
    CHECK(connection.negotiation.identity->domain == "LAB");
    CHECK(connection.peer == "192.0.2.10:50123");
    CHECK(connection.elapsed_ms == 850);
    REQUIRE(connection.client.has_value());
    CHECK(connection.client->desktop_width == 2560);
    CHECK(connection.client->keyboard_layout == 0x0407);
    CHECK(connection.client->client_name == "WORKSTATION");
    CHECK(connection.client->desktop_scale_factor == 150U);
    REQUIRE(connection.client->monitors.size() == 2);
    CHECK(connection.client->monitors[1].left == -1920);
    CHECK(connection.client->monitors[0].primary);
    REQUIRE(connection.auto_reconnect.has_value());
    CHECK(connection.auto_reconnect->logon_id == 0x1234);
    CHECK(connection.pending_input == full_connection().pending_input);
    CHECK(broker::encode(connection) == connection_frame);

    const auto disconnect = std::get<broker::Disconnect>(
        broker::decode(broker::encode(broker::Disconnect{7, proto::errinfo::disconnected_by_other_connection}))
            .value());
    CHECK(disconnect.connection_id == 7);
    CHECK(disconnect.error_info == proto::errinfo::disconnected_by_other_connection);

    const auto ended = std::get<broker::SessionEnded>(
        broker::decode(broker::encode(broker::SessionEnded{broker::EndReason::desktop_failed, "gnome-shell crashed"}))
            .value());
    CHECK(ended.reason == broker::EndReason::desktop_failed);
    CHECK(ended.detail == "gnome-shell crashed");

    broker::Stats stats;
    stats.connection_id = 7;
    stats.idle_seconds = 12;
    stats.desktop_width = 1920;
    stats.bytes_sent = std::uint64_t{1} << 40;
    stats.rtt_ms = 23;
    const auto decoded_stats = std::get<broker::Stats>(broker::decode(broker::encode(stats)).value());
    CHECK(decoded_stats.idle_seconds == 12);
    CHECK(decoded_stats.bytes_sent == std::uint64_t{1} << 40);
    CHECK(decoded_stats.rtt_ms == 23);

    // A connection without NLA, summary, cookie or input.
    broker::NewConnection bare;
    bare.connection_id = 9;
    bare.negotiation.selected_protocol = proto::protocol::ssl;
    const auto decoded_bare = std::get<broker::NewConnection>(broker::decode(broker::encode(bare)).value());
    CHECK_FALSE(decoded_bare.negotiation.identity.has_value());
    CHECK_FALSE(decoded_bare.client.has_value());
    CHECK(decoded_bare.pending_input.empty());
}

TEST_CASE("Broker frames are length-prefixed like privsep")
{
    const auto bytes = broker::encode(broker::Hello{broker::protocol_version, token_of(0)});
    REQUIRE(bytes.size() == 4 + 1 + 2 + 32);
    CHECK(bytes[0] == std::byte{35});
    CHECK(bytes[3] == std::byte{0});
    CHECK(bytes[4] == std::byte{1});  // Hello
    CHECK(bytes[5] == std::byte{broker::protocol_version});
    CHECK(broker::message_length(bytes).value() == bytes.size());
    CHECK_FALSE(broker::message_length(std::span(bytes).first(3)).value().has_value());
    CHECK_FALSE(broker::message_length(std::span(bytes).first(bytes.size() - 1)).value().has_value());

    const std::vector<std::byte> huge{std::byte{0x01}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00}};
    CHECK(broker::message_length(huge).error().code == Errc::limit_exceeded);
    const std::vector<std::byte> empty{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
    CHECK(broker::message_length(empty).error().code == Errc::limit_exceeded);
}

TEST_CASE("Malformed broker messages are rejected")
{
    // The smallest valid NewConnection, as a baseline for the variants below.
    const auto minimal = frame([](Writer& w) {
        connection_prefix(w);
        w.u8(0);     // no client summary
        w.u8(0);     // no cookie
        w.u32le(0);  // no pending input
    });
    REQUIRE(broker::decode(minimal).has_value());

    check_error(frame([](Writer& w) { w.u8(99); }), Errc::invalid_value);  // unknown type
    check_error(frame([](Writer& w) {
                    connection_prefix(w);
                    w.u8(2);  // presence byte other than 0 or 1
                }),
                Errc::invalid_value);
    check_error(frame([](Writer& w) {
                    connection_prefix(w);
                    w.u8(0);
                    w.u8(0);
                }),
                Errc::truncated);
    auto trailing = minimal;
    trailing.push_back(std::byte{0});
    trailing[0] = static_cast<std::byte>(std::to_integer<unsigned>(trailing[0]) + 1);
    check_error(trailing, Errc::trailing_data);
    auto short_length = minimal;
    short_length.pop_back();
    check_error(short_length, Errc::invalid_length);

    check_error(frame([](Writer& w) {
                    connection_prefix(w);
                    w.u8(1);
                    client_prefix(w);
                    w.u8(17);  // monitors
                }),
                Errc::limit_exceeded);
    check_error(frame([](Writer& w) {
                    connection_prefix(w);
                    w.u8(1);
                    client_prefix(w);
                    w.u8(1);
                    for (const std::uint32_t v : {10U, 0U, 5U, 100U}) {  // left > right
                        w.u32le(v);
                    }
                    w.u8(1);
                    w.u8(0);
                    w.u32le(0);
                }),
                Errc::invalid_value);
    check_error(frame([](Writer& w) {
                    connection_prefix(w);
                    w.u8(0);
                    w.u8(0);
                    w.u32le(broker::max_pending_input + 1);
                }),
                Errc::limit_exceeded);
    // An identity exactly when NLA was selected.
    check_error(frame([](Writer& w) {
                    connection_prefix(w, proto::protocol::ssl, true);
                    w.u8(0);
                    w.u8(0);
                    w.u32le(0);
                }),
                Errc::invalid_value);
    check_error(frame([](Writer& w) {
                    connection_prefix(w, proto::protocol::hybrid, false);
                    w.u8(0);
                    w.u8(0);
                    w.u32le(0);
                }),
                Errc::invalid_value);
    check_error(frame([](Writer& w) {
                    connection_prefix(w, 0x40, false);  // not a protocol farland selects
                    w.u8(0);
                    w.u8(0);
                    w.u32le(0);
                }),
                Errc::invalid_value);
    check_error(frame([](Writer& w) {
                    w.u8(3);  // Disconnect
                    w.u64le(0);
                    w.u32le(0);
                }),
                Errc::invalid_value);
    check_error(frame([](Writer& w) {
                    w.u8(4);  // SessionEnded
                    w.u8(7);
                    w.u16le(0);
                }),
                Errc::invalid_value);
    check_error(frame([](Writer& w) {
                    w.u8(4);
                    w.u8(1);
                    w.u16le(1025);  // detail too long
                }),
                Errc::limit_exceeded);
}

TEST_CASE("Each side may send only its own broker messages, with a descriptor only on NewConnection")
{
    const auto connection = broker::encode(full_connection());
    CHECK(broker::decode_from(broker::Sender::daemon, connection, true).has_value());
    CHECK_FALSE(broker::decode_from(broker::Sender::daemon, connection, false).has_value());
    CHECK_FALSE(broker::decode_from(broker::Sender::agent, connection, true).has_value());

    const auto disconnect = broker::encode(broker::Disconnect{7, 0});
    CHECK(broker::decode_from(broker::Sender::daemon, disconnect, false).has_value());
    CHECK(broker::decode_from(broker::Sender::agent, disconnect, false).has_value());
    CHECK_FALSE(broker::decode_from(broker::Sender::agent, disconnect, true).has_value());

    const auto stats = broker::encode(broker::Stats{});
    CHECK(broker::decode_from(broker::Sender::agent, stats, false).has_value());
    CHECK_FALSE(broker::decode_from(broker::Sender::daemon, stats, false).has_value());
}

TEST_CASE("farlandd accepts an agent only after a hello with its token")
{
    const auto hello = broker::encode(broker::Hello{broker::protocol_version, token_of(1)});
    const auto stats = broker::encode(broker::Stats{});

    {
        broker::AgentLink link(token_of(1));
        CHECK_FALSE(link.receive(stats, false).has_value());  // no hello yet
    }
    {
        broker::AgentLink link(token_of(2));
        CHECK_FALSE(link.receive(hello, false).has_value());  // wrong token
        CHECK_FALSE(link.greeted());
    }
    {
        broker::AgentLink link(token_of(1));
        CHECK_FALSE(
            link.receive(broker::encode(broker::Hello{broker::protocol_version + 1, token_of(1)}), false).has_value());
    }

    broker::AgentLink link(token_of(1));
    const auto greeted = link.receive(hello, false);
    REQUIRE(greeted.has_value());
    CHECK(link.greeted());
    CHECK(std::get<broker::Hello>(*greeted).token == broker::Token{});  // wiped
    CHECK_FALSE(link.receive(hello, false).has_value());                // only once

    CHECK(link.receive(stats, false).has_value());
    CHECK_FALSE(link.receive(stats, true).has_value());
    CHECK(link.receive(broker::encode(broker::Disconnect{3, 0}), false).has_value());
    CHECK_FALSE(link.receive(broker::encode(full_connection()), true).has_value());

    CHECK(link.receive(broker::encode(broker::SessionEnded{broker::EndReason::logout, {}}), false).has_value());
    CHECK(link.ended());
    CHECK_FALSE(link.receive(stats, false).has_value());
}
