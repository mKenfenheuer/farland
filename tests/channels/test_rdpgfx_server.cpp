// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/hexdump.hpp>
#include <farland/channels/rdpgfx_server.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <initializer_list>
#include <utility>

namespace gfx = farland::channels::rdpgfx;
using farland::to_hex;
using farland::test::hex;
using namespace gfx::cap_version;
using namespace gfx::caps_flag;

namespace {

using Sets = std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>;

std::vector<std::byte> advertise(Sets sets)
{
    gfx::CapsAdvertise pdu;
    for (const auto& [version, flags] : sets) {
        pdu.caps_sets.push_back(gfx::make_capability_set(version, flags));
    }
    return gfx::encode(pdu);
}

std::vector<std::byte> ack(std::uint32_t frame_id, std::uint32_t queue_depth = 0, std::uint32_t total = 0)
{
    return gfx::encode(
        gfx::FrameAcknowledge{.queue_depth = queue_depth, .frame_id = frame_id, .total_frames_decoded = total});
}

std::vector<gfx::GfxEvent> drain(gfx::GfxServer& server)
{
    std::vector<gfx::GfxEvent> out;
    while (auto event = server.poll_event()) {
        out.push_back(std::move(*event));
    }
    return out;
}

/// Decoded output; the PDUs' spans refer into `bytes`.
struct Output {
    std::vector<std::vector<std::byte>> bytes;
    std::vector<gfx::Pdu> pdus;

    [[nodiscard]] std::vector<std::uint16_t> commands() const
    {
        std::vector<std::uint16_t> out;
        for (const auto& pdu : pdus) {
            out.push_back(gfx::cmd_id(pdu));
        }
        return out;
    }
};

Output take(gfx::GfxServer& server)
{
    Output out;
    out.bytes = server.take_output();
    for (const auto& bytes : out.bytes) {
        auto pdu = gfx::decode_pdu(bytes);
        REQUIRE(pdu.has_value());
        out.pdus.push_back(*pdu);
    }
    return out;
}

gfx::Negotiated make_ready(gfx::GfxServer& server, Sets sets)
{
    server.receive(advertise(sets));
    const auto events = drain(server);
    REQUIRE(events.size() == 1);
    const auto* ready = std::get_if<gfx::event::Ready>(&events.front());
    REQUIRE(ready != nullptr);
    CHECK_FALSE(ready->reset);
    return ready->negotiated;
}

/// Negotiates and checks the Caps Confirm against `Negotiated`.
gfx::Negotiated confirm_for(Sets sets, gfx::GfxServerConfig config = {})
{
    gfx::GfxServer server(config);
    const auto negotiated = make_ready(server, sets);
    const auto out = take(server);
    REQUIRE(out.pdus.size() == 1);
    const auto& confirm = std::get<gfx::CapsConfirm>(out.pdus.front());
    CHECK(confirm.caps_set.version == negotiated.version);
    CHECK(confirm.caps_set.flags == negotiated.flags);
    CHECK(out.bytes.front().size() == gfx::header_size + 8 + gfx::caps_data_length(negotiated.version).value());
    // docs/PLAN.md §3.2 rule 5: nothing the client did not offer, nothing undefined.
    CHECK((negotiated.flags & ~negotiated.client_flags) == 0);
    CHECK((negotiated.flags & ~gfx::defined_caps_flags(negotiated.version)) == 0);
    return negotiated;
}

/// A server with a 10.7 confirm (AVC allowed), graphics reset to 256x256,
/// one 256x256 surface (id 0) mapped at the origin, output drained.
gfx::GfxServer active_server(Sets sets = {{v10_7, 0}})
{
    gfx::GfxServer server;
    static_cast<void>(make_ready(server, sets));
    server.reset_graphics(256, 256);
    CHECK(server.create_surface(256, 256) == 0);
    server.map_surface_to_output(0, 0, 0);
    static_cast<void>(server.take_output());
    return server;
}

void frame(gfx::GfxServer& server)
{
    static_cast<void>(server.start_frame());
    server.end_frame();
}

std::string failure(gfx::GfxServer& server)
{
    const auto events = drain(server);
    REQUIRE_FALSE(events.empty());
    const auto* failed = std::get_if<gfx::event::Failed>(&events.back());
    REQUIRE(failed != nullptr);
    CHECK(server.state() == gfx::GfxState::failed);
    CHECK(server.failure_reason() == failed->reason);
    return failed->reason;
}

}  // namespace

TEST_CASE("Caps negotiation: FreeRDP 3 without H.264 gets 10.7 with AVC_DISABLED ([MS-RDPEGFX] 3.2.5.19)")
{
    gfx::GfxServer server;
    const auto negotiated = make_ready(server, {{v8, 0},
                                                {v8_1, 0},
                                                {v10, avc_disabled},
                                                {v10_1, 0},
                                                {v10_2, avc_disabled},
                                                {v10_3, avc_disabled},
                                                {v10_4, avc_disabled},
                                                {v10_5, avc_disabled},
                                                {v10_6, avc_disabled},
                                                {v10_6_err, avc_disabled},
                                                {v10_7, avc_disabled}});
    CHECK(negotiated.version == v10_7);
    CHECK(negotiated.flags == avc_disabled);
    CHECK_FALSE(negotiated.avc420);
    CHECK_FALSE(negotiated.avc444);
    CHECK_FALSE(negotiated.allows(gfx::codec::avc444));
    CHECK(negotiated.allows(gfx::codec::progressive));
    CHECK(negotiated.allows(gfx::codec::clearcodec));
    CHECK(negotiated.scaled_output);
    CHECK(negotiated.qoe);
    CHECK_FALSE(negotiated.small_cache);
    CHECK(negotiated.max_cache_slots == 25600);
    const auto out = server.take_output();
    REQUIRE(out.size() == 1);
    CHECK(to_hex(out.front()) == "13 00 00 00 14 00 00 00 01 07 0a 00 04 00 00 00 20 00 00 00");
}

TEST_CASE("Caps negotiation matrix")
{
    SECTION("only 8.0, thin client: RemoteFX instead of Progressive, small cache")
    {
        const auto n = confirm_for({{v8, thin_client}});
        CHECK(n.version == v8);
        CHECK(n.flags == thin_client);
        CHECK(n.thin_client);
        CHECK(n.small_cache);
        CHECK(n.max_cache_slots == 4096);
        CHECK(n.max_cache_bytes == 16U * 1024U * 1024U);
        CHECK_FALSE(n.allows(gfx::codec::progressive));
        CHECK(n.allows(gfx::codec::cavideo));
        CHECK_FALSE(n.allows(gfx::codec::avc420));
        CHECK_FALSE(n.qoe);
        CHECK_FALSE(n.scaled_output);
    }
    SECTION("8.0 and 8.1 with AVC420 (FreeRDP with /gfx:avc420): 8.1, AVC420 confirmed")
    {
        const auto n = confirm_for({{v8, 0}, {v8_1, avc420_enabled | small_cache}});
        CHECK(n.version == v8_1);
        CHECK(n.flags == (avc420_enabled | small_cache));
        CHECK(n.avc420);
        CHECK_FALSE(n.avc444);
        CHECK(n.small_cache);
    }
    SECTION("8.1 with AVC420, but the server has no AVC420 encoder: the flag is withdrawn")
    {
        const auto n = confirm_for({{v8_1, avc420_enabled}}, {.avc420 = false});
        CHECK(n.flags == 0);
        CHECK_FALSE(n.avc420);
    }
    SECTION("undefined bits are not echoed")
    {
        const auto n = confirm_for({{v8_1, 0xFFFF0100U | avc420_enabled}});
        CHECK(n.client_flags == (0xFFFF0100U | avc420_enabled));
        CHECK(n.flags == avc420_enabled);
    }
    SECTION("only 10.0 without AVC_DISABLED: AVC444, but not AVC420 before 10.4")
    {
        const auto n = confirm_for({{v10, 0}});
        CHECK(n.version == v10);
        CHECK(n.avc444);
        CHECK(n.avc444v2);
        CHECK_FALSE(n.avc420);
        CHECK(n.qoe);
    }
    SECTION("only 10.1: no flags, capsDataLength 16, AVC444, no QoE")
    {
        const auto n = confirm_for({{v10_1, 0}});
        CHECK(n.version == v10_1);
        CHECK(n.flags == 0);
        CHECK(n.avc444);
        CHECK_FALSE(n.qoe);
    }
    SECTION("10.3 implies the 16 MB cache; AVC_THINCLIENT is kept")
    {
        const auto n = confirm_for({{v10_3, avc_thin_client}});
        CHECK(n.small_cache);
        CHECK(n.avc_thin_client);
        CHECK(n.flags == avc_thin_client);
    }
    SECTION("10.4 without AVC_DISABLED allows AVC420 next to other codecs")
    {
        const auto n = confirm_for({{v10_4, small_cache}});
        CHECK(n.avc420);
        CHECK(n.avc444);
        CHECK(n.small_cache);
    }
    SECTION("10.7 with SCALEDMAP_DISABLE")
    {
        const auto n = confirm_for({{v10_7, scaledmap_disable | avc_disabled}});
        CHECK_FALSE(n.scaled_output);
        CHECK(n.flags == (scaledmap_disable | avc_disabled));
    }
    SECTION("10.6 wins over its errata value; the errata value alone is accepted")
    {
        CHECK(confirm_for({{v10_6_err, 0}, {v10_6, 0}}).version == v10_6);
        CHECK(confirm_for({{v10_6_err, 0}}).version == v10_6_err);
    }
    SECTION("11.x is never confirmed; the highest documented version is")
    {
        CHECK(confirm_for({{v11_3, 0}, {v11_1, 0}, {v10_7, 0}, {v10_5, 0}}).version == v10_7);
    }
    SECTION("max_version limits the choice")
    {
        CHECK(confirm_for({{v8_1, 0}, {v10_2, 0}, {v10_7, 0}}, {.max_version = v10_2}).version == v10_2);
    }
    SECTION("confirmed flags are always a subset of the offer")
    {
        for (const std::uint32_t version : {v8, v8_1, v10, v10_1, v10_2, v10_3, v10_4, v10_5, v10_6, v10_7}) {
            for (const std::uint32_t flags : {0U, 0xFFFFFFFFU, 0x13U, 0x62U, 0xE2U}) {
                CAPTURE(gfx::version_name(version), flags);
                static_cast<void>(confirm_for({{version, flags}}, {.avc420 = false}));
            }
        }
    }
}

TEST_CASE("A client offering nothing farland supports fails the channel")
{
    gfx::GfxServer server;
    server.receive(advertise({{v11_1, 0}, {0x00010000, 0}}));
    CHECK(failure(server).find("no RDPGFX capability set") != std::string::npos);
    CHECK(server.take_output().empty());
}

TEST_CASE("Initial output sequence: Caps Confirm, Reset Graphics, surface, frame")
{
    gfx::GfxServer server;
    static_cast<void>(make_ready(server, {{v10_7, 0}}));
    server.reset_graphics(1920, 1080);
    const auto surface = server.create_surface(1920, 1080);
    server.map_surface_to_output(surface, 0, 0);
    const auto frame_id = server.start_frame(gfx::make_timestamp(1, 2, 3, 4));
    const std::array<std::byte, 2> data{std::byte{1}, std::byte{2}};
    server.wire_to_surface_1(surface, gfx::codec::avc444, gfx::pixel_format::xrgb_8888, {0, 0, 1920, 1080}, data);
    server.end_frame();
    CHECK(frame_id == 1);
    CHECK(server.frames_in_flight() == 1);

    const auto out = take(server);
    using namespace gfx::cmd;
    CHECK(out.commands() == std::vector<std::uint16_t>{caps_confirm, reset_graphics, create_surface,
                                                       map_surface_to_output, start_frame, wire_to_surface_1,
                                                       end_frame});
    CHECK(out.bytes.at(1).size() == gfx::reset_graphics_pdu_size);
    // An empty monitor list becomes one primary monitor covering the output.
    CHECK(std::get<gfx::ResetGraphics>(out.pdus.at(1)).monitors ==
          std::vector<gfx::MonitorDef>{{0, 0, 1919, 1079, gfx::monitor_primary}});
    CHECK(std::get<gfx::StartFrame>(out.pdus.at(4)).timestamp == gfx::make_timestamp(1, 2, 3, 4));
    CHECK(std::get<gfx::EndFrame>(out.pdus.at(6)).frame_id == 1);
    CHECK(to_hex(std::get<gfx::WireToSurface1>(out.pdus.at(5)).bitmap_data) == "01 02");
}

TEST_CASE("Reset Graphics carries a multi-monitor layout")
{
    gfx::GfxServer server;
    static_cast<void>(make_ready(server, {{v10_7, 0}}));
    const std::array monitors{gfx::MonitorDef{0, 0, 1919, 1079, gfx::monitor_primary},
                              gfx::MonitorDef{1920, 0, 3839, 1079, 0}};
    server.reset_graphics(3840, 1080, monitors);
    const auto out = take(server);
    CHECK(std::get<gfx::ResetGraphics>(out.pdus.at(1)).monitors ==
          std::vector<gfx::MonitorDef>(monitors.begin(), monitors.end()));
}

TEST_CASE("Surface lifecycle and codec contexts")
{
    auto server = active_server();
    CHECK(server.create_surface(64, 64, gfx::pixel_format::argb_8888) == 1);
    server.delete_surface(0);
    CHECK_FALSE(server.has_surface(0));
    CHECK(server.create_surface(32, 32) == 0);  // lowest free id again
    CHECK(server.has_surface(1));

    static_cast<void>(server.start_frame());
    const std::array<std::byte, 1> data{};
    server.wire_to_surface_2(1, gfx::codec::progressive, 42, gfx::pixel_format::argb_8888, data);
    server.end_frame();
    server.delete_encoding_context(1, 42);
    server.delete_surface(1);

    const auto out = take(server);
    using namespace gfx::cmd;
    CHECK(out.commands() == std::vector<std::uint16_t>{create_surface, delete_surface, create_surface, start_frame,
                                                       wire_to_surface_2, end_frame, delete_encoding_context,
                                                       delete_surface});
    CHECK(std::get<gfx::CreateSurface>(out.pdus.at(0)) ==
          gfx::CreateSurface{.surface_id = 1, .width = 64, .height = 64, .pixel_format = gfx::pixel_format::argb_8888});
    CHECK(std::get<gfx::WireToSurface2>(out.pdus.at(4)).codec_context_id == 42);
    CHECK(std::get<gfx::DeleteEncodingContext>(out.pdus.at(6)) ==
          gfx::DeleteEncodingContext{.surface_id = 1, .codec_context_id = 42});
}

TEST_CASE("Drawing commands inside a frame")
{
    auto server = active_server();
    CHECK(server.create_surface(64, 64) == 1);
    static_cast<void>(server.start_frame());
    const std::array rects{gfx::Rect16{0, 0, 16, 16}, gfx::Rect16{240, 240, 256, 256}};
    server.solid_fill(0, {.b = 1, .g = 2, .r = 3, .xa = 0}, rects);
    const std::array points{gfx::Point16{0, 0}, gfx::Point16{48, 48}};
    server.surface_to_surface(0, 1, {0, 0, 16, 16}, points);
    server.end_frame();
    const auto out = take(server);
    REQUIRE(out.pdus.size() == 5);
    CHECK(std::get<gfx::SolidFill>(out.pdus.at(2)).fill_rects == std::vector<gfx::Rect16>(rects.begin(), rects.end()));
    CHECK(std::get<gfx::SurfaceToSurface>(out.pdus.at(3)).dest_pts ==
          std::vector<gfx::Point16>(points.begin(), points.end()));
}

TEST_CASE("Scaled output mapping when 10.7 allows it ([MS-RDPEGFX] 2.2.2.22)")
{
    auto server = active_server();
    server.map_surface_to_scaled_output(0, 0, 0, 512, 512);
    server.map_surface_to_window(0, 7, 256, 256);
    server.map_surface_to_scaled_window(0, 7, 256, 256, 512, 512);
    const auto out = take(server);
    CHECK(std::get<gfx::MapSurfaceToScaledOutput>(out.pdus.at(0)).target_width == 512);
    CHECK(std::get<gfx::MapSurfaceToWindow>(out.pdus.at(1)).window_id == 7);
    CHECK(std::get<gfx::MapSurfaceToScaledWindow>(out.pdus.at(2)).target_height == 512);
}

TEST_CASE("Frame acknowledgement window ([MS-RDPEGFX] 3.2.5.13)")
{
    auto server = active_server();
    frame(server);
    frame(server);
    frame(server);
    CHECK(server.frames_in_flight() == 3);

    SECTION("an ack settles its frame and every older one")
    {
        server.receive(ack(2, 1234, 2));
        auto events = drain(server);
        REQUIRE(events.size() == 1);
        const auto& acked = std::get<gfx::event::FrameAcked>(events.front());
        CHECK(acked.frame_id == 2);
        CHECK(acked.known);
        CHECK(acked.queue_depth == 1234);
        CHECK(server.frames_in_flight() == 1);
        CHECK(server.queue_depth() == 1234);
        CHECK(server.total_frames_decoded() == 2);

        // Unknown and repeated frame ids are ignored, like FreeRDP does.
        server.receive(ack(99));
        server.receive(ack(2));
        events = drain(server);
        REQUIRE(events.size() == 2);
        CHECK_FALSE(std::get<gfx::event::FrameAcked>(events.at(0)).known);
        CHECK_FALSE(std::get<gfx::event::FrameAcked>(events.at(1)).known);
        CHECK(server.frames_in_flight() == 1);
        CHECK(server.ready());

        server.receive(ack(3));
        CHECK(server.frames_in_flight() == 0);
    }
    SECTION("SUSPEND_FRAME_ACKNOWLEDGEMENT empties the window until the client acks again")
    {
        server.receive(ack(1, gfx::suspend_frame_acknowledgement, 1));
        CHECK(server.acks_suspended());
        CHECK(server.frames_in_flight() == 0);
        frame(server);  // frame 4, not tracked
        frame(server);  // frame 5
        CHECK(server.frames_in_flight() == 0);

        server.receive(ack(5, gfx::queue_depth_unavailable, 5));  // opting back in
        CHECK_FALSE(server.acks_suspended());
        CHECK(server.frames_in_flight() == 0);
        frame(server);
        CHECK(server.frames_in_flight() == 1);
        const auto events = drain(server);
        REQUIRE(events.size() == 2);
        CHECK(std::get<gfx::event::FrameAcked>(events.at(0)).queue_depth == gfx::suspend_frame_acknowledgement);
        CHECK_FALSE(std::get<gfx::event::FrameAcked>(events.at(1)).known);
    }
}

TEST_CASE("QoE Frame Acknowledge surfaces as an event ([MS-RDPEGFX] 3.2.5.21)")
{
    auto server = active_server();
    frame(server);
    const gfx::QoeFrameAcknowledge qoe{.frame_id = 1, .timestamp = 5000, .time_diff_se = 3, .time_diff_edr = 7};
    auto bytes = ack(1);
    const auto qoe_bytes = gfx::encode(qoe);
    bytes.insert(bytes.end(), qoe_bytes.begin(), qoe_bytes.end());  // two PDUs in one receive
    server.receive(bytes);
    const auto events = drain(server);
    REQUIRE(events.size() == 2);
    CHECK(std::get<gfx::event::FrameAcked>(events.at(0)).known);
    CHECK(std::get<gfx::event::QoeFrameAcked>(events.at(1)).qoe == qoe);
}

TEST_CASE("Cache Import Offer gets an empty reply ([MS-RDPEGFX] 3.2.5.16)")
{
    auto server = active_server();
    server.receive(gfx::encode(gfx::CacheImportOffer{{{1, 100}, {2, 200}}}));
    const auto events = drain(server);
    REQUIRE(events.size() == 1);
    const auto& offered = std::get<gfx::event::CacheImportOffered>(events.front());
    CHECK(offered.offered == 2);
    CHECK(offered.imported == 0);
    const auto out = server.take_output();
    REQUIRE(out.size() == 1);
    CHECK(to_hex(out.front()) == "11 00 00 00 0a 00 00 00 00 00");
}

TEST_CASE("Bitmap cache slot accounting ([MS-RDPEGFX] 3.3.1.4)")
{
    SECTION("the small cache has 4096 one-based slots")
    {
        auto server = active_server({{v10_2, small_cache}});
        static_cast<void>(server.start_frame());
        for (std::uint64_t key = 0; key < 4096; ++key) {
            const auto slot = server.surface_to_cache(0, {0, 0, 1, 1}, key);
            REQUIRE(slot.has_value());
            CHECK(*slot == key + 1);
        }
        CHECK(server.cache_slots_used() == 4096);
        CHECK_FALSE(server.surface_to_cache(0, {0, 0, 1, 1}, 5000).has_value());
        static_cast<void>(server.take_output());

        server.evict_cache_entry(5);
        CHECK_FALSE(server.cache_slot(4).has_value());
        CHECK(server.surface_to_cache(0, {0, 0, 1, 1}, 5000) == 5);
        CHECK(server.cache_slot(5000) == 5);
        const std::array points{gfx::Point16{10, 10}};
        server.cache_to_surface(5, 0, points);
        server.end_frame();
        const auto out = take(server);
        using namespace gfx::cmd;
        CHECK(out.commands() ==
              std::vector<std::uint16_t>{evict_cache_entry, surface_to_cache, cache_to_surface, end_frame});
        CHECK(std::get<gfx::SurfaceToCache>(out.pdus.at(1)) ==
              gfx::SurfaceToCache{.surface_id = 0, .cache_key = 5000, .cache_slot = 5, .rect_src = {0, 0, 1, 1}});
    }
    SECTION("the 100 MB cache has 25600 slots and a byte budget")
    {
        auto server = active_server();
        CHECK(server.negotiated()->max_cache_slots == 25600);
        static_cast<void>(server.start_frame());
        // 256x256x4 = 256 KB per entry: 400 entries fill 100 MB exactly.
        for (std::uint64_t key = 0; key < 400; ++key) {
            REQUIRE(server.surface_to_cache(0, {0, 0, 256, 256}, key).has_value());
        }
        CHECK(server.cache_bytes_used() == 100U * 1024U * 1024U);
        CHECK_FALSE(server.surface_to_cache(0, {0, 0, 1, 1}, 1000).has_value());
        server.evict_cache_entry(1);
        CHECK(server.surface_to_cache(0, {0, 0, 1, 1}, 1000) == 1);
        server.end_frame();
    }
}

TEST_CASE("Caps Advertise again after 10.3+ resets the protocol ([MS-RDPEGFX] 3.2.5.18)")
{
    auto server = active_server({{v10_5, 0}});
    frame(server);
    static_cast<void>(server.start_frame());
    static_cast<void>(server.surface_to_cache(0, {0, 0, 8, 8}, 1));
    server.end_frame();
    static_cast<void>(server.take_output());

    server.receive(advertise({{v10_7, avc_disabled}}));
    const auto events = drain(server);
    REQUIRE(events.size() == 1);
    const auto& ready = std::get<gfx::event::Ready>(events.front());
    CHECK(ready.reset);
    CHECK(ready.negotiated.version == v10_7);
    CHECK_FALSE(server.has_surface(0));
    CHECK(server.frames_in_flight() == 0);
    CHECK(server.cache_slots_used() == 0);
    const auto out = take(server);
    CHECK(out.commands() == std::vector<std::uint16_t>{gfx::cmd::caps_confirm});
    // Frame ids stay unique across the reset.
    server.reset_graphics(64, 64);
    CHECK(server.start_frame() == 3);
}

TEST_CASE("Caps Advertise again after 8.x-10.2 is a protocol violation")
{
    auto server = active_server({{v10_2, 0}});
    server.receive(advertise({{v10_2, 0}}));
    CHECK(failure(server).find("repeated") != std::string::npos);
}

TEST_CASE("Client PDUs split across receive calls")
{
    gfx::GfxServer server;
    const auto bytes = advertise({{v8_1, avc420_enabled}, {v10_7, 0}});
    for (const std::byte b : bytes) {
        server.receive(std::span(&b, 1));
    }
    const auto events = drain(server);
    REQUIRE(events.size() == 1);
    CHECK(std::get<gfx::event::Ready>(events.front()).negotiated.version == v10_7);
}

TEST_CASE("Malformed or unexpected client PDUs fail the channel")
{
    SECTION("Frame Acknowledge before Caps Advertise")
    {
        gfx::GfxServer server;
        server.receive(ack(1));
        CHECK(failure(server).find("before Caps Advertise") != std::string::npos);
    }
    SECTION("a server-to-client PDU from the client")
    {
        auto server = active_server();
        server.receive(gfx::encode(gfx::EndFrame{1}));
        CHECK(failure(server).find("server-to-client") != std::string::npos);
    }
    SECTION("pduLength below the header")
    {
        gfx::GfxServer server;
        server.receive(hex("12 00 00 00 04 00 00 00"));
        static_cast<void>(failure(server));
    }
    SECTION("pduLength above the limit fails before the data arrives")
    {
        gfx::GfxServer server;
        server.receive(hex("12 00 00 00 00 00 10 00"));
        CHECK(failure(server).find("size limit") != std::string::npos);
    }
    SECTION("a truncated Frame Acknowledge")
    {
        auto server = active_server();
        server.receive(hex("0d 00 00 00 10 00 00 00 00 00 00 00 01 00 00 00"));
        CHECK(failure(server).find("malformed") != std::string::npos);
    }
    SECTION("an unknown cmdId")
    {
        auto server = active_server();
        server.receive(hex("1a 00 00 00 08 00 00 00"));
        static_cast<void>(failure(server));
    }
    SECTION("a Cache Import Offer with 5462 entries")
    {
        auto server = active_server();
        server.receive(hex("10 00 00 00 0a 00 00 00 56 15"));
        static_cast<void>(failure(server));
    }
    SECTION("after a failure, input is ignored and commands do nothing")
    {
        auto server = active_server();
        frame(server);
        static_cast<void>(server.take_output());
        server.receive(hex("ff ff 00 00 08 00 00 00"));
        static_cast<void>(failure(server));
        server.receive(ack(1));
        CHECK(drain(server).empty());
        CHECK(server.create_surface(10, 10) == 0);
        CHECK(server.start_frame() == 0);
        server.end_frame();
        CHECK_FALSE(server.surface_to_cache(0, {0, 0, 1, 1}, 1).has_value());
        CHECK(server.take_output().empty());
    }
}
