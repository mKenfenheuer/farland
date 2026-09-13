// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/channels/drdynvc.hpp>
#include <farland/channels/svc.hpp>
#include <farland/codec/zgfx.hpp>
#include <farland/server/dynamic_channels.hpp>

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <utility>
#include <vector>

namespace dyn = farland::channels::drdynvc;
namespace svc = farland::channels::svc;
namespace ev = farland::channels::dvc_event;
using farland::server::DynamicChannels;
using Bytes = std::vector<std::byte>;

namespace {

/// The client end of the drdynvc static channel.
struct Client {
    std::vector<Bytes> chunks;
    svc::Reassembler reassembler{std::size_t{1} << 20U};
    std::deque<Bytes> messages;  // keeps the bytes decoded PDUs point into

    DynamicChannels::SendChunk sink()
    {
        return [this](std::span<const std::byte> chunk) { chunks.emplace_back(chunk.begin(), chunk.end()); };
    }

    /// The drdynvc PDUs the server sent since the last call.
    std::vector<dyn::ServerPdu> take()
    {
        std::vector<dyn::ServerPdu> out;
        for (const auto& chunk : std::exchange(chunks, {})) {
            auto message = reassembler.add(chunk).value();
            if (message) {
                messages.push_back(std::move(*message));
                out.push_back(dyn::decode_server_pdu(messages.back()).value());
            }
        }
        return out;
    }
};

void send(DynamicChannels& channels, const dyn::ClientPdu& pdu, std::size_t chunk_length = svc::chunk_length)
{
    for (const auto& chunk : svc::encode_chunks(dyn::encode_client_pdu(pdu), chunk_length)) {
        REQUIRE(channels.receive(chunk).has_value());
    }
}

}  // namespace

TEST_CASE("Dynamic channels run over chunked drdynvc messages")
{
    Client client;
    DynamicChannels channels(client.sink());
    channels.start();
    auto pdus = client.take();
    REQUIRE(pdus.size() == 1);
    CHECK(std::get<dyn::CapsRequest>(pdus[0]).version == dyn::version3);
    send(channels, dyn::CapsResponse{dyn::version3});
    CHECK(std::get<ev::CapabilitiesReady>(channels.poll_event().value()).version == dyn::version3);

    const std::string name = "Microsoft::Windows::RDS::Graphics";
    const auto id = channels.open(name);
    pdus = client.take();
    REQUIRE(pdus.size() == 1);
    CHECK(std::get<dyn::CreateRequest>(pdus[0]).name == name);
    send(channels, dyn::CreateResponse{id, 0});
    CHECK(std::get<ev::ChannelOpened>(channels.poll_event().value()).id == id);

    // A 5000-byte message: DATA_FIRST, then DATA PDUs, each in its own chunk.
    Bytes message(5000);
    for (std::size_t i = 0; i < message.size(); ++i) {
        message[i] = static_cast<std::byte>(i * 7U);
    }
    REQUIRE(channels.send(id, message));
    pdus = client.take();
    REQUIRE(pdus.size() > 1);
    const auto& first = std::get<dyn::DataFirst>(pdus.front());
    CHECK(first.length == message.size());
    Bytes received(first.data.begin(), first.data.end());
    for (std::size_t i = 1; i < pdus.size(); ++i) {
        const auto& data = std::get<dyn::Data>(pdus[i]);
        received.insert(received.end(), data.data.begin(), data.data.end());
    }
    CHECK(received == message);

    // The client's message arrives split over two Virtual Channel chunks.
    const Bytes reply(1200, std::byte{0x42});
    send(channels, dyn::Data{id, reply}, 700);
    CHECK(std::get<ev::ChannelData>(channels.poll_event().value()).data == reply);

    // Version 3: compressed client data, RDP 8.0 Lite per channel.
    farland::codec::ZgfxCompressor lite(farland::codec::ZgfxVariant::rdp8_lite);
    const Bytes text(1000, std::byte{'a'});
    const auto compressed = lite.compress(text);
    CHECK(compressed.size() < text.size());
    send(channels, dyn::DataCompressed{id, compressed});
    CHECK(std::get<ev::ChannelData>(channels.poll_event().value()).data == text);
}

TEST_CASE("drdynvc protocol errors come back from receive")
{
    Client client;
    DynamicChannels channels(client.sink());
    channels.start();
    static_cast<void>(client.take());
    const Bytes payload(4, std::byte{1});
    // Data before the Capabilities Response.
    const auto chunks = svc::encode_chunks(dyn::encode_client_pdu(dyn::Data{7, payload}));
    CHECK_FALSE(channels.receive(chunks.at(0)).has_value());
}
