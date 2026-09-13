// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The Graphics Pipeline ([MS-RDPEGFX]): client-to-server bytes into
// GfxServer, which draws a frame with every command after each chunk so that
// acknowledgements meet frames in flight; the same input fed byte by byte
// must end in the same state. Every PDU that decodes, in either direction,
// must survive a round trip.

#include <farland/base/assert.hpp>
#include <farland/channels/rdpgfx.hpp>
#include <farland/channels/rdpgfx_server.hpp>

#include "fuzz.hpp"

#include <array>
#include <span>

using namespace farland;
namespace gfx = farland::channels::rdpgfx;

namespace {

/// Largest input also fed byte by byte.
constexpr std::size_t max_bytewise = 4096;

void check_round_trip(std::span<const std::byte> bytes)
{
    const auto pdu = gfx::decode_pdu(bytes);
    if (!pdu) {
        return;
    }
    const auto encoded = gfx::encode(*pdu);
    const auto again = gfx::decode_pdu(encoded);
    FARLAND_ASSERT(again.has_value() && *again == *pdu);
    FARLAND_ASSERT(gfx::frame_pdu(encoded, encoded.size()).value() == encoded.size());
}

/// Drives a ready server through one frame using every command.
class Driver {
public:
    void drive(gfx::GfxServer& server)
    {
        while (const auto event = server.poll_event()) {
            // A repeated Caps Advertise empties the cache: forget the slot.
            if (const auto* ready = std::get_if<gfx::event::Ready>(&*event); ready != nullptr && ready->reset) {
                last_slot_.reset();
            }
        }
        if (!server.ready()) {
            return;
        }
        const auto& negotiated = *server.negotiated();
        if (!server.has_surface(0)) {
            server.reset_graphics(64, 64);
            static_cast<void>(server.create_surface(64, 64));
            server.map_surface_to_output(0, 0, 0);
            if (negotiated.scaled_output) {
                server.map_surface_to_scaled_output(0, 0, 0, 128, 128);
            }
        }
        static_cast<void>(server.start_frame(gfx::make_timestamp(1, 2, 3, 4)));
        const std::array<std::byte, 4> data{};
        server.wire_to_surface_1(0, gfx::codec::planar, gfx::pixel_format::xrgb_8888, {0, 0, 1, 1}, data);
        if (negotiated.allows(gfx::codec::progressive)) {
            server.wire_to_surface_2(0, gfx::codec::progressive, 1, gfx::pixel_format::xrgb_8888, data);
        }
        const std::array rects{gfx::Rect16{0, 0, 8, 8}};
        server.solid_fill(0, {}, rects);
        const std::array points{gfx::Point16{8, 8}};
        server.surface_to_surface(0, 0, rects.front(), points);
        if (last_slot_) {
            server.cache_to_surface(*last_slot_, 0, points);
            server.evict_cache_entry(*last_slot_);
        }
        last_slot_ = server.surface_to_cache(0, rects.front(), next_key_++);
        server.end_frame();
        static_cast<void>(server.take_output());
    }

private:
    std::optional<std::uint16_t> last_slot_;
    std::uint64_t next_key_ = 1;
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const auto input = std::as_bytes(std::span(data, size));

    // Round trips: the whole input as one PDU, and each PDU of the stream.
    check_round_trip(input);
    for (auto rest = input; !rest.empty();) {
        const auto framed = gfx::frame_pdu(rest, rest.size());
        if (!framed || !*framed) {
            break;
        }
        check_round_trip(rest.first(**framed));
        rest = rest.subspan(**framed);
    }

    // The server, driven between two halves of the input and after a replay.
    {
        gfx::GfxServer server;
        Driver driver;
        const std::size_t half = size / 2;
        server.receive(input.first(half));
        driver.drive(server);
        server.receive(input.subspan(half));
        driver.drive(server);
        server.receive(input);
        driver.drive(server);
    }

    // Byte by byte ends where the whole input does.
    if (size <= max_bytewise) {
        gfx::GfxServer whole;
        whole.receive(input);
        gfx::GfxServer bytewise;
        for (const std::byte b : input) {
            bytewise.receive(std::span(&b, 1));
        }
        FARLAND_ASSERT(whole.state() == bytewise.state());
        FARLAND_ASSERT(whole.negotiated() == bytewise.negotiated());
        FARLAND_ASSERT(whole.take_output() == bytewise.take_output());
        FARLAND_ASSERT(whole.frames_in_flight() == bytewise.frames_in_flight());
    }
    return 0;
}
