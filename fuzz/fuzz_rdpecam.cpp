// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Camera channel messages ([MS-RDPECAM]). Input: records of
// [u16le length][PDU]. Every PDU that decodes must survive re-encoding, and a
// Media Type List Response that decodes goes through the server's choice of a
// media type, which must pick one of the offered types, one farland can carry
// and one inside the limits it was given.

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/channels/rdpecam.hpp>
#include <farland/server/camera.hpp>

#include "fuzz.hpp"

#include <algorithm>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>

using namespace farland;
namespace cam = farland::channels::rdpecam;

namespace {

void check_choice(const cam::MediaTypeListResponse& list)
{
    const server::CameraOptions options{.max_width = 1280, .max_height = 720, .preferred_fps = 30};
    const auto chosen = server::CameraServer::choose_media_type(list.media_types, options);
    if (!chosen) {
        return;
    }
    FARLAND_ASSERT(std::ranges::find(list.media_types, *chosen) != list.media_types.end());
    FARLAND_ASSERT(!cam::needs_decoding(chosen->format));
    FARLAND_ASSERT((chosen->flags & cam::media_flag::decoding_required) == 0);
    FARLAND_ASSERT(chosen->width > 0 && chosen->width <= options.max_width);
    FARLAND_ASSERT(chosen->height > 0 && chosen->height <= options.max_height);
    FARLAND_ASSERT(cam::frame_size(chosen->format, chosen->width, chosen->height) > 0);
}

/// A SampleResponse points into its message, so it is checked directly
/// instead of being re-encoded and compared.
void check_round_trip(std::span<const std::byte> message)
{
    const auto version = std::to_integer<std::uint8_t>(message[0]);
    if (const auto pdu = cam::decode_client_pdu(message)) {
        if (const auto* sample = std::get_if<cam::SampleResponse>(&*pdu)) {
            FARLAND_ASSERT(sample->sample.size() + 3 == message.size());
        } else {
            const auto again = cam::decode_client_pdu(cam::encode_client_pdu(*pdu, version));
            FARLAND_ASSERT(again.has_value());
            FARLAND_ASSERT(std::visit(
                [&pdu](const auto& a) {
                    using T = std::decay_t<decltype(a)>;
                    if constexpr (std::is_same_v<T, cam::SampleResponse>) {
                        return false;  // a re-encoded PDU never becomes a sample
                    } else {
                        const auto* b = std::get_if<T>(&*pdu);
                        return b != nullptr && a == *b;
                    }
                },
                *again));
        }
        if (const auto* list = std::get_if<cam::MediaTypeListResponse>(&*pdu)) {
            check_choice(*list);
        }
    }
    if (const auto pdu = cam::decode_server_pdu(message)) {
        const auto again = cam::decode_server_pdu(cam::encode_server_pdu(*pdu, version));
        FARLAND_ASSERT(again.has_value() && *again == *pdu);
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    Reader r(std::as_bytes(std::span(data, size)));
    while (!r.empty()) {
        const auto length = r.u16le();
        if (!length || *length == 0) {
            break;
        }
        const auto message = r.bytes(std::min<std::size_t>(*length, r.remaining())).value();
        if (message.size() >= 2) {
            check_round_trip(message);
        }
    }
    return 0;
}
