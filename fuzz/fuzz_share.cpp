// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Everything that arrives on the MCS I/O channel after the connection is
// set up: share control and data PDUs, capability sets, Client Info and
// licensing; plus the output-side decoders a future client will run.

#include <farland/proto/bitmap.hpp>
#include <farland/proto/capabilities.hpp>
#include <farland/proto/client_info.hpp>
#include <farland/proto/fastpath.hpp>
#include <farland/proto/license.hpp>
#include <farland/proto/security.hpp>
#include <farland/proto/share.hpp>

#include "fuzz.hpp"

#include <span>

using namespace farland;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const auto input = std::as_bytes(std::span(data, size));

    Reader share(input);
    if (auto control = proto::read_share_control(share); control.has_value()) {
        Reader body = control->body;
        switch (control->type) {
        case proto::pdu_type::demand_active:
            static_cast<void>(proto::decode_demand_active(body));
            break;
        case proto::pdu_type::confirm_active:
            static_cast<void>(proto::decode_confirm_active(body));
            break;
        case proto::pdu_type::deactivate_all:
            static_cast<void>(proto::decode_deactivate_all(body));
            break;
        case proto::pdu_type::data:
            if (auto pdu = proto::read_share_data(body); pdu.has_value()) {
                static_cast<void>(proto::decode_data_pdu(*pdu));
            }
            break;
        default:
            break;
        }
    }

    if (!input.empty()) {
        Reader sets(input.subspan(1));
        static_cast<void>(proto::caps::decode_capability_sets(sets, std::to_integer<std::uint16_t>(input[0])));
    }

    Reader info(input);
    if (proto::read_basic_security_header(info).has_value()) {
        static_cast<void>(proto::decode_client_info(info));
    }
    Reader license(input);
    static_cast<void>(proto::decode_license_valid_client(license));
    Reader bitmap(input);
    static_cast<void>(proto::decode_bitmap_update(bitmap));
    Reader fastpath(input);
    static_cast<void>(proto::fastpath::decode_output_pdu(fastpath));
    return 0;
}
