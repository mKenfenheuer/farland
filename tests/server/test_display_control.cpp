// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/channels/disp.hpp>
#include <farland/channels/drdynvc.hpp>
#include <farland/channels/svc.hpp>
#include <farland/server/display_control.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <deque>
#include <utility>
#include <vector>

namespace dyn = farland::channels::drdynvc;
namespace disp = farland::channels::disp;
namespace svc = farland::channels::svc;
using farland::server::DisplayControl;
using farland::server::DisplayLayout;
using farland::server::DynamicChannels;
using Bytes = std::vector<std::byte>;
using namespace std::chrono_literals;

namespace {

struct Client {
    std::vector<Bytes> chunks;
    svc::Reassembler reassembler{std::size_t{1} << 20U};
    std::deque<Bytes> messages;

    DynamicChannels::SendChunk sink()
    {
        return [this](std::span<const std::byte> chunk) { chunks.emplace_back(chunk.begin(), chunk.end()); };
    }

    std::vector<dyn::ServerPdu> take()
    {
        std::vector<dyn::ServerPdu> out;
        for (const auto& chunk : std::exchange(chunks, {})) {
            if (auto message = reassembler.add(chunk).value()) {
                messages.push_back(std::move(*message));
                out.push_back(dyn::decode_server_pdu(messages.back()).value());
            }
        }
        return out;
    }
};

void send(DynamicChannels& channels, const dyn::ClientPdu& pdu)
{
    for (const auto& chunk : svc::encode_chunks(dyn::encode_client_pdu(pdu))) {
        REQUIRE(channels.receive(chunk).has_value());
    }
}

Bytes layout(std::uint32_t width, std::uint32_t height)
{
    return disp::encode(disp::MonitorLayoutPdu{{{.flags = disp::monitor_primary, .width = width, .height = height}}});
}

struct Fixture {
    Client client;
    DynamicChannels channels{client.sink()};
    std::optional<DisplayControl> control;
    std::uint32_t id = 0;
    DisplayControl::Clock::time_point t0{std::chrono::seconds(100)};

    Fixture()
    {
        channels.start();
        static_cast<void>(client.take());
        send(channels, dyn::CapsResponse{dyn::version3});
        static_cast<void>(channels.poll_event());
        control.emplace(channels, DisplayLayout::single(1024, 768));
        const auto pdus = client.take();
        REQUIRE(pdus.size() == 1);
        const auto& create = std::get<dyn::CreateRequest>(pdus[0]);
        CHECK(create.name == disp::channel_name);
        id = create.channel_id;
    }

    void deliver(const dyn::ClientPdu& pdu, DisplayControl::Clock::time_point now)
    {
        send(channels, pdu);
        while (auto event = channels.poll_event()) {
            CHECK(control->handle(*event, now));
        }
    }
};

}  // namespace

TEST_CASE("Display control: [MS-RDPEDISP] 3.2.5.1 capabilities first, then debounced layouts")
{
    Fixture f;
    f.deliver(dyn::CreateResponse{f.id, 0}, f.t0);
    CHECK(f.control->open());
    auto pdus = f.client.take();
    REQUIRE(pdus.size() == 1);
    const auto caps = disp::decode(std::get<dyn::Data>(pdus[0]).data).value();
    CHECK(std::get<disp::CapsPdu>(caps) == farland::server::DisplayLimits{}.caps());

    // The current size again: nothing to do.
    f.deliver(dyn::Data{f.id, layout(1024, 768)}, f.t0);
    CHECK_FALSE(f.control->deadline().has_value());

    // A drag: layouts every 50 ms; only the last is handed out, once it settled.
    auto t = f.t0;
    for (std::uint32_t w = 1100; w <= 1300; w += 50) {
        f.deliver(dyn::Data{f.id, layout(w, 800)}, t);
        CHECK_FALSE(f.control->poll_layout(t).has_value());
        t += 50ms;
    }
    const auto last = t - 50ms;
    CHECK(f.control->deadline() == last + 300ms);
    CHECK_FALSE(f.control->poll_layout(last + 299ms).has_value());
    const auto applied = f.control->poll_layout(last + 300ms);
    REQUIRE(applied.has_value());
    CHECK(applied->width() == 1300);
    CHECK_FALSE(f.control->poll_layout(last + 1s).has_value());

    // A drag that goes on and on still resizes once a second.
    t = last + 2s;
    const auto start = t;
    std::optional<DisplayLayout> during;
    for (std::uint32_t w = 400; w < 1000 && !during; w += 2) {
        f.deliver(dyn::Data{f.id, layout(w, 600)}, t);
        t += 20ms;
        during = f.control->poll_layout(t);
    }
    REQUIRE(during.has_value());
    CHECK(t - start >= 1s);
    CHECK(t - start < 1s + 40ms);
}

TEST_CASE("Display control: invalid layouts are ignored, broken PDUs close the channel")
{
    Fixture f;
    f.deliver(dyn::CreateResponse{f.id, 0}, f.t0);
    static_cast<void>(f.client.take());
    f.deliver(dyn::Data{f.id, layout(1025, 768)}, f.t0);  // odd width
    CHECK_FALSE(f.control->deadline().has_value());
    CHECK(f.control->open());

    auto broken = layout(1280, 720);
    broken.pop_back();
    f.deliver(dyn::Data{f.id, broken}, f.t0);
    CHECK_FALSE(f.control->open());
    const auto pdus = f.client.take();
    REQUIRE(pdus.size() == 1);
    CHECK(std::get<dyn::Close>(pdus[0]).channel_id == f.id);
}

TEST_CASE("Display control: a client without the listener refuses the channel")
{
    Fixture f;
    f.deliver(dyn::CreateResponse{f.id, static_cast<std::int32_t>(0x80070002)}, f.t0);
    CHECK_FALSE(f.control->open());
    CHECK(f.client.take().empty());
}
