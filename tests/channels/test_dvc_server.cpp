// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/channels/drdynvc.hpp>
#include <farland/channels/dvc_server.hpp>
#include <farland/channels/svc.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using farland::Errc;
using farland::Result;
using farland::channels::DvcChannelOptions;
using farland::channels::DvcDecompressor;
using farland::channels::DvcEvent;
using farland::channels::DvcServer;
using farland::channels::DvcServerConfig;
using farland::test::hex;
namespace dyn = farland::channels::drdynvc;
namespace svc = farland::channels::svc;
namespace ev = farland::channels::dvc_event;

namespace {

using Bytes = std::vector<std::byte>;

Bytes filled(std::size_t size, std::uint8_t value = 0x71)
{
    return Bytes(size, std::byte{value});
}

Bytes pattern(std::size_t size)
{
    Bytes bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>((i * 13U) & 0xFFU);
    }
    return bytes;
}

Bytes concat(Bytes a, const Bytes& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

std::vector<DvcEvent> events(DvcServer& server)
{
    std::vector<DvcEvent> out;
    while (auto event = server.poll_event()) {
        out.push_back(std::move(*event));
    }
    return out;
}

template <class T>
T only_event(DvcServer& server)
{
    auto all = events(server);
    REQUIRE(all.size() == 1);
    const auto* event = std::get_if<T>(&all[0]);
    REQUIRE(event != nullptr);
    return *event;
}

void feed(DvcServer& server, const dyn::ClientPdu& pdu)
{
    const auto result = server.receive(dyn::encode_client_pdu(pdu));
    INFO((result.has_value() ? "" : result.error().message()));
    REQUIRE(result.has_value());
}

Errc feed_error(DvcServer& server, const Bytes& message)
{
    const auto result = server.receive(message);
    REQUIRE_FALSE(result.has_value());
    CHECK(server.failed());
    return result.error().code;
}

/// The client side of the script: decodes the server's output like a DVC
/// client manager and reassembles its data per channel.
struct Client {
    std::vector<dyn::CapsRequest> caps;
    std::vector<dyn::CreateRequest> creates;
    std::vector<std::uint32_t> closes;
    std::vector<std::pair<std::uint32_t, Bytes>> messages;
    std::vector<Bytes> raw;
    std::map<std::uint32_t, dyn::MessageReassembler> reassemblers;

    void read(DvcServer& server)
    {
        raw = server.take_output();
        for (const auto& message : raw) {
            REQUIRE(message.size() <= dyn::max_pdu_size);
            // Every drdynvc message fits one static channel chunk.
            REQUIRE(svc::encode_chunks(message).size() == 1);
            const auto pdu = dyn::decode_server_pdu(message);
            REQUIRE(pdu.has_value());
            if (const auto* c = std::get_if<dyn::CapsRequest>(&*pdu)) {
                caps.push_back(*c);
            } else if (const auto* create = std::get_if<dyn::CreateRequest>(&*pdu)) {
                creates.push_back(*create);
            } else if (const auto* close = std::get_if<dyn::Close>(&*pdu)) {
                closes.push_back(close->channel_id);
            } else if (const auto* first = std::get_if<dyn::DataFirst>(&*pdu)) {
                auto& r = reassemblers.try_emplace(first->channel_id, std::size_t{1} << 24U).first->second;
                if (auto done = r.first(first->length, first->data).value()) {
                    messages.emplace_back(first->channel_id, std::move(*done));
                }
            } else if (const auto* data = std::get_if<dyn::Data>(&*pdu)) {
                auto& r = reassemblers.try_emplace(data->channel_id, std::size_t{1} << 24U).first->second;
                if (auto done = r.next(data->data).value()) {
                    messages.emplace_back(data->channel_id, std::move(*done));
                }
            } else {
                FAIL("unexpected PDU from the server");
            }
        }
    }
};

/// Starts the server, answers its capabilities with `version`, opens
/// `count` channels and accepts them.
std::vector<std::uint32_t> ready(DvcServer& server, std::uint16_t version, int count)
{
    server.start();
    feed(server, dyn::CapsResponse{version});
    std::vector<std::uint32_t> ids;
    for (int i = 0; i < count; ++i) {
        const auto id = server.open("channel" + std::to_string(i));
        feed(server, dyn::CreateResponse{id, 0});
        ids.push_back(id);
    }
    static_cast<void>(server.take_output());
    static_cast<void>(events(server));
    return ids;
}

/// Decompresses the RDP 8.0 Lite blocks of the [MS-RDPEDYC] 4.3.3 and 4.3.4
/// examples and nothing else; counts the contexts created.
DvcServerConfig spec_decompressor_config(int& contexts)
{
    DvcServerConfig config;
    config.make_decompressor = [&contexts]() -> DvcDecompressor {
        ++contexts;
        return [](std::span<const std::byte> block) -> Result<Bytes> {
            if (std::ranges::equal(block, hex("e0 26 38 c4 3f f4 74 01"))) {
                return filled(1595);
            }
            if (std::ranges::equal(block, hex("e0 26 88 7f e8 f4 02"))) {
                return filled(1597);
            }
            if (std::ranges::equal(block, hex("06 71 71 71"))) {
                return filled(3);
            }
            return farland::fail(Errc::invalid_value, "unknown test block");
        };
    };
    return config;
}

}  // namespace

TEST_CASE("Scripted client: capabilities, create, data both ways, close")
{
    DvcServer server;
    Client client;

    server.start();
    client.read(server);
    // Without a decompressor, version 2 is offered.
    REQUIRE(client.caps.size() == 1);
    CHECK(server.offered_version() == 2);
    CHECK(client.raw[0] == hex("50 00 02 00 a8 03 cc 0c 92 24 55 55"));

    // Channels opened during the capabilities exchange wait for it.
    const auto id = server.open("testdvc");
    CHECK(id == 1);
    client.read(server);
    CHECK(client.raw.empty());
    CHECK_FALSE(server.send(id, hex("aa")));

    feed(server, dyn::CapsResponse{2});
    CHECK(only_event<ev::CapabilitiesReady>(server).version == 2);
    CHECK(server.version() == 2);
    client.read(server);
    REQUIRE(client.creates.size() == 1);
    CHECK(client.raw[0] == hex("10 01 74 65 73 74 64 76 63 00"));
    CHECK_FALSE(server.is_open(id));

    feed(server, dyn::CreateResponse{id, 0});
    const auto opened = only_event<ev::ChannelOpened>(server);
    CHECK(opened.id == id);
    CHECK(opened.name == "testdvc");
    CHECK(server.is_open(id));

    // Server to client: a small and a fragmented message.
    const auto big = pattern(5000);
    REQUIRE(server.send(id, hex("01 02 03")));
    REQUIRE(server.send(id, big));
    client.read(server);
    CHECK(client.raw.size() == 1 + 4);
    CHECK(client.raw[0] == hex("30 01 01 02 03"));
    REQUIRE(client.messages.size() == 2);
    CHECK(client.messages[0] == std::pair{id, hex("01 02 03")});
    CHECK(client.messages[1] == std::pair{id, big});

    // Client to server: a single Data and a fragmented message.
    feed(server, dyn::Data{id, hex("aa bb")});
    CHECK(only_event<ev::ChannelData>(server).data == hex("aa bb"));
    const auto upload = pattern(3000);
    for (const auto& pdu : dyn::encode_data(id, upload)) {
        REQUIRE(server.receive(pdu).has_value());
    }
    const auto data = only_event<ev::ChannelData>(server);
    CHECK(data.id == id);
    CHECK(data.data == upload);

    // The client closes; the server does not answer ([MS-RDPEDYC] 3.3.5.2).
    feed(server, dyn::Close{id});
    CHECK(only_event<ev::ChannelClosed>(server).id == id);
    CHECK_FALSE(server.is_open(id));
    CHECK_FALSE(server.send(id, hex("aa")));
    client.read(server);
    CHECK(client.raw.empty());

    // Late data for the closed channel is ignored.
    feed(server, dyn::Data{id, hex("aa")});
    CHECK(events(server).empty());
}

TEST_CASE("The server adjusts to the client's version ([MS-RDPEDYC] 3.3.3.1)")
{
    int contexts = 0;
    SECTION("v3 offered with a decompressor, client answers 3")
    {
        DvcServer server(spec_decompressor_config(contexts));
        server.start();
        CHECK(server.offered_version() == 3);
        feed(server, dyn::CapsResponse{3});
        CHECK(only_event<ev::CapabilitiesReady>(server).version == 3);
    }
    SECTION("client answers 1")
    {
        DvcServer server(spec_decompressor_config(contexts));
        server.start();
        feed(server, dyn::CapsResponse{1});
        CHECK(server.version() == 1);
    }
    SECTION("client answers above the offer")
    {
        DvcServer server;
        server.start();
        feed(server, dyn::CapsResponse{3});
        CHECK(server.version() == 2);
    }
    SECTION("configured for version 1")
    {
        DvcServerConfig config;
        config.max_version = 1;
        DvcServer server(config);
        server.start();
        CHECK(server.take_output() == std::vector<Bytes>{hex("50 00 01 00")});
        feed(server, dyn::CapsResponse{2});
        CHECK(server.version() == 1);
    }
}

TEST_CASE("Channel ids count up, are not reused, and use 2- and 4-byte fields")
{
    DvcServer server;
    ready(server, 2, 0);
    Client client;

    const auto a = server.open("a", DvcChannelOptions{2, std::nullopt});
    const auto b = server.open("b");
    CHECK(a == 1);
    CHECK(b == 2);
    client.read(server);
    CHECK(client.raw == std::vector<Bytes>{hex("18 01 61 00"), hex("10 02 62 00")});

    server.close(a);
    CHECK(server.take_output() == std::vector<Bytes>{hex("40 01")});
    CHECK(server.open("c") == 3);
    static_cast<void>(server.take_output());

    // Walk the ids up to 2- and 4-byte values.
    for (std::uint32_t expected = 4; expected <= 0x10000; ++expected) {
        const auto id = server.open("x");
        REQUIRE(id == expected);
        if (id == 300) {
            CHECK(server.take_output() == std::vector<Bytes>{hex("11 2c 01 78 00")});
        }
        server.close(id);
        static_cast<void>(server.take_output());
    }
    const auto wide = server.open("big");
    CHECK(wide == 0x10001);
    CHECK(server.take_output() == std::vector<Bytes>{hex("12 01 00 01 00 62 69 67 00")});
    REQUIRE(server.receive(hex("12 01 00 01 00 00 00 00 00")).has_value());
    CHECK(only_event<ev::ChannelOpened>(server).id == wide);

    // Fragmented data with a 4-byte id in both directions.
    const auto message = pattern(3000);
    REQUIRE(server.send(wide, message));
    client.read(server);
    REQUIRE(client.raw.size() == 2);
    CHECK(client.raw[0].size() == 1600);
    CHECK(std::ranges::equal(std::span(client.raw[0]).first(7), hex("26 01 00 01 00 b8 0b")));
    CHECK(std::ranges::equal(std::span(client.raw[1]).first(5), hex("32 01 00 01 00")));
    REQUIRE(client.messages.size() == 1);
    CHECK(client.messages[0] == std::pair{wide, message});

    for (const auto& pdu : dyn::encode_data(wide, message)) {
        REQUIRE(server.receive(pdu).has_value());
    }
    CHECK(only_event<ev::ChannelData>(server).data == message);

    // Clients may use wider fields than needed.
    REQUIRE(server.receive(hex("11 02 00 00 00 00 00")).has_value());
    CHECK(only_event<ev::ChannelOpened>(server).id == b);
    REQUIRE(server.receive(concat(hex("25 02 00 0a 00"), filled(4))).has_value());
    CHECK(events(server).empty());
    REQUIRE(server.receive(concat(hex("32 02 00 00 00"), filled(6))).has_value());
    const auto data = only_event<ev::ChannelData>(server);
    CHECK(data.id == b);
    CHECK(data.data == filled(10));
}

TEST_CASE("A refused channel reports the HRESULT and frees its id")
{
    DvcServer server;
    ready(server, 2, 0);
    const auto id = server.open("Microsoft::Windows::RDS::Graphics");
    static_cast<void>(server.take_output());
    const auto status = std::bit_cast<std::int32_t>(std::uint32_t{0x80070490});
    feed(server, dyn::CreateResponse{id, status});
    const auto failed = only_event<ev::ChannelOpenFailed>(server);
    CHECK(failed.id == id);
    CHECK(failed.name == "Microsoft::Windows::RDS::Graphics");
    CHECK(failed.status == status);
    CHECK_FALSE(server.is_open(id));
    CHECK_FALSE(server.send(id, hex("aa")));
    // No Close is sent for the failed channel.
    CHECK(server.take_output().empty());
    // A positive status is success.
    const auto other = server.open("x");
    feed(server, dyn::CreateResponse{other, 1});
    CHECK(only_event<ev::ChannelOpened>(server).id == other);
}

TEST_CASE("Server-side close forgets the id at once")
{
    DvcServer server;
    const auto ids = ready(server, 2, 1);
    server.close(ids[0]);
    CHECK(server.take_output() == std::vector<Bytes>{hex("40 01")});
    CHECK(events(server).empty());
    // The client's Close reply and data still in flight are ignored.
    feed(server, dyn::Data{ids[0], hex("aa")});
    feed(server, dyn::Close{ids[0]});
    CHECK(events(server).empty());
    server.close(ids[0]);
    server.close(12345);
    CHECK(server.take_output().empty());

    // Closing a channel that waits for its Create Response sends a Close too,
    // and its late Create Response is ignored.
    const auto opening = server.open("x");
    static_cast<void>(server.take_output());
    server.close(opening);
    CHECK(server.take_output() == std::vector<Bytes>{hex("40 02")});
    feed(server, dyn::CreateResponse{opening, 0});
    CHECK(events(server).empty());

    // A channel closed before the capabilities exchange is never requested.
    DvcServer early;
    early.start();
    const auto never = early.open("never");
    const auto kept = early.open("kept");
    early.close(never);
    static_cast<void>(early.take_output());
    feed(early, dyn::CapsResponse{2});
    CHECK(early.take_output() == std::vector<Bytes>{hex("10 02 6b 65 70 74 00")});
    CHECK(kept == 2);
}

TEST_CASE("Compressed data needs version 3 and a per-channel decompressor ([MS-RDPEDYC] 4.3.3, 4.3.4)")
{
    int contexts = 0;
    DvcServer server(spec_decompressor_config(contexts));
    const auto ids = ready(server, 3, 3);
    REQUIRE(ids.back() == 3);

    REQUIRE(server.receive(hex("64 03 7b 0c e0 26 38 c4 3f f4 74 01")).has_value());
    REQUIRE(server.receive(hex("70 03 e0 26 88 7f e8 f4 02")).has_value());
    CHECK(events(server).empty());
    REQUIRE(server.receive(hex("70 03 06 71 71 71")).has_value());
    const auto data = only_event<ev::ChannelData>(server);
    CHECK(data.id == 3);
    CHECK(data.data == filled(3195));
    CHECK(contexts == 1);

    // Compressed and uncompressed blocks can mix; channel 1 gets its own context.
    REQUIRE(server.receive(hex("64 01 7b 0c e0 26 38 c4 3f f4 74 01")).has_value());
    REQUIRE(server.receive(concat(hex("30 01"), filled(1597))).has_value());
    REQUIRE(server.receive(hex("70 01 06 71 71 71")).has_value());
    CHECK(only_event<ev::ChannelData>(server).data == filled(3195));
    CHECK(contexts == 2);

    // Decompression failures are fatal.
    CHECK(feed_error(server, hex("70 02 e0 00")) == Errc::invalid_value);
}

TEST_CASE("Compressed data without version 3 is a protocol error")
{
    DvcServer server;
    ready(server, 2, 3);
    const auto message = hex("70 03 06 71 71 71");
    CHECK(feed_error(server, message) == Errc::invalid_value);
    // The error sticks.
    CHECK_FALSE(server.receive(hex("30 01 aa")).has_value());
    CHECK_FALSE(server.send(1, hex("aa")));
}

TEST_CASE("Out-of-sequence client PDUs are fatal ([MS-RDPEDYC] 3.1.5.2.4)")
{
    DvcServer server;

    SECTION("anything before start()")
    {
        CHECK(feed_error(server, hex("50 00 02 00")) == Errc::invalid_value);
    }
    SECTION("data before the capabilities exchange")
    {
        server.start();
        CHECK(feed_error(server, hex("30 01 aa")) == Errc::invalid_value);
    }
    SECTION("Create Response before the capabilities exchange")
    {
        server.start();
        static_cast<void>(server.open("a"));
        CHECK(feed_error(server, hex("10 01 00 00 00 00")) == Errc::invalid_value);
    }
    SECTION("a second Capabilities Response")
    {
        ready(server, 2, 0);
        CHECK(feed_error(server, hex("50 00 02 00")) == Errc::invalid_value);
    }
    SECTION("version 0")
    {
        server.start();
        CHECK(feed_error(server, hex("50 00 00 00")) == Errc::invalid_value);
    }
    SECTION("a second Create Response")
    {
        ready(server, 2, 1);
        CHECK(feed_error(server, hex("10 01 00 00 00 00")) == Errc::invalid_value);
    }
    SECTION("data on a channel waiting for its Create Response")
    {
        ready(server, 2, 0);
        static_cast<void>(server.open("a"));
        CHECK(feed_error(server, hex("30 01 aa")) == Errc::invalid_value);
    }
    SECTION("Close for a channel waiting for its Create Response")
    {
        ready(server, 2, 0);
        static_cast<void>(server.open("a"));
        CHECK(feed_error(server, hex("40 01")) == Errc::invalid_value);
    }
    SECTION("Data First inside a fragmented message")
    {
        ready(server, 2, 1);
        REQUIRE(server.receive(hex("20 01 04 aa")).has_value());
        CHECK(feed_error(server, hex("20 01 04 aa")) == Errc::invalid_value);
    }
    SECTION("data beyond the announced length")
    {
        ready(server, 2, 1);
        REQUIRE(server.receive(hex("20 01 04 aa")).has_value());
        CHECK(feed_error(server, hex("30 01 bb cc dd ee")) == Errc::invalid_length);
    }
    SECTION("malformed PDU")
    {
        ready(server, 2, 1);
        CHECK(feed_error(server, hex("33 01")) == Errc::invalid_value);
    }
    SECTION("empty message")
    {
        ready(server, 2, 1);
        CHECK(feed_error(server, {}) == Errc::truncated);
    }
}

TEST_CASE("Unknown channel ids and Soft-Sync Responses are ignored")
{
    DvcServer server;
    ready(server, 2, 1);
    REQUIRE(server.receive(hex("10 09 00 00 00 00")).has_value());
    REQUIRE(server.receive(hex("30 09 aa")).has_value());
    REQUIRE(server.receive(hex("20 09 04 aa")).has_value());
    REQUIRE(server.receive(hex("40 09")).has_value());
    REQUIRE(server.receive(hex("90 00 01 00 00 00 01 00 00 00")).has_value());
    CHECK(events(server).empty());
    CHECK(server.take_output().empty());
    CHECK_FALSE(server.failed());
}

TEST_CASE("Per-channel reassembly limits")
{
    DvcServerConfig config;
    config.max_message_size = 1000;
    DvcServer server(config);
    ready(server, 2, 0);
    const auto small = server.open("small", DvcChannelOptions{0, 100});
    const auto normal = server.open("normal");
    feed(server, dyn::CreateResponse{small, 0});
    feed(server, dyn::CreateResponse{normal, 0});
    static_cast<void>(events(server));

    feed(server, dyn::Data{small, filled(100)});
    CHECK(only_event<ev::ChannelData>(server).data.size() == 100);
    feed(server, dyn::DataFirst{normal, 1000, filled(10)});
    feed(server, dyn::Data{normal, filled(990)});
    CHECK(only_event<ev::ChannelData>(server).data.size() == 1000);

    SECTION("Data First above the limit")
    {
        CHECK(feed_error(server, dyn::encode_client_pdu(dyn::DataFirst{small, 101, filled(10)})) ==
              Errc::limit_exceeded);
    }
    SECTION("single Data above the limit")
    {
        CHECK(feed_error(server, dyn::encode_client_pdu(dyn::Data{small, filled(101)})) == Errc::limit_exceeded);
    }
    SECTION("the server-wide default")
    {
        CHECK(feed_error(server, dyn::encode_client_pdu(dyn::DataFirst{normal, 1001, filled(10)})) ==
              Errc::limit_exceeded);
    }
}

TEST_CASE("DvcServer output travels through the drdynvc static channel")
{
    DvcServer server;
    server.start();
    const auto id = server.open("echo");
    std::vector<Bytes> wire;
    const auto flush = [&] {
        for (const auto& message : server.take_output()) {
            for (auto& chunk : svc::encode_chunks(message)) {
                wire.push_back(std::move(chunk));
            }
        }
    };
    flush();

    // The client answers through its own static channel chunks.
    svc::Reassembler from_client(1U << 20U);
    const auto client_sends = [&](const dyn::ClientPdu& pdu) {
        for (const auto& chunk : svc::encode_chunks(dyn::encode_client_pdu(pdu), 1000)) {
            if (auto message = from_client.add(chunk).value()) {
                REQUIRE(server.receive(*message).has_value());
            }
        }
    };
    client_sends(dyn::CapsResponse{2});
    client_sends(dyn::CreateResponse{id, 0});
    flush();
    REQUIRE(server.send(id, pattern(4000)));
    flush();

    svc::Reassembler to_client(1U << 20U);
    Client decoded;
    std::size_t messages = 0;
    for (const auto& chunk : wire) {
        CHECK(chunk.size() <= svc::header_size + svc::chunk_length);
        if (to_client.add(chunk).value()) {
            ++messages;
        }
    }
    CHECK(messages == wire.size());  // one chunk per drdynvc message
    CHECK(messages == 1 + 1 + 3);    // caps, create, data first + 2 data
}
