// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

/// The Display Control Virtual Channel Extension ([MS-RDPEDISP]): the PDUs of
/// the "Microsoft::Windows::RDS::DisplayControl" dynamic virtual channel, over
/// which a client asks the server for another monitor layout (a resized
/// window, a monitor added or rotated). Each PDU is one DVC message.
namespace farland::channels::disp {

inline constexpr std::string_view channel_name = "Microsoft::Windows::RDS::DisplayControl";

/// DISPLAYCONTROL_HEADER Type, [MS-RDPEDISP] 2.2.1.1.
namespace pdu_type {
inline constexpr std::uint32_t monitor_layout = 0x00000002;
inline constexpr std::uint32_t caps = 0x00000005;
}  // namespace pdu_type

/// Size of DISPLAYCONTROL_HEADER.
inline constexpr std::size_t header_size = 8;
/// MonitorLayoutSize: the size of one DISPLAYCONTROL_MONITOR_LAYOUT, which
/// the specification fixes at 40 ([MS-RDPEDISP] 2.2.2.2).
inline constexpr std::uint32_t monitor_layout_size = 40;
/// DISPLAYCONTROL_MONITOR_LAYOUT Flags ([MS-RDPEDISP] 2.2.2.2.1).
inline constexpr std::uint32_t monitor_primary = 0x00000001;

/// DISPLAYCONTROL_MONITOR_LAYOUT Orientation, in degrees ([MS-RDPEDISP] 2.2.2.2.1).
namespace orientation {
inline constexpr std::uint32_t landscape = 0;
inline constexpr std::uint32_t portrait = 90;
inline constexpr std::uint32_t landscape_flipped = 180;
inline constexpr std::uint32_t portrait_flipped = 270;
}  // namespace orientation

/// Most monitors a layout may carry at all: farland never offers more than
/// this in MaxNumMonitors, and GFX's ResetGraphics has room for no more
/// ([MS-RDPEGFX] 2.2.2.14).
inline constexpr std::uint32_t max_monitors = 16;

/// DISPLAYCONTROL_CAPS_PDU, [MS-RDPEDISP] 2.2.2.1 (server to client). The
/// largest layout area the server takes is max_num_monitors *
/// max_monitor_area_factor_a * max_monitor_area_factor_b pixels.
struct CapsPdu {
    std::uint32_t max_num_monitors = 0;
    std::uint32_t max_monitor_area_factor_a = 0;
    std::uint32_t max_monitor_area_factor_b = 0;

    friend bool operator==(const CapsPdu&, const CapsPdu&) = default;
};

/// DISPLAYCONTROL_MONITOR_LAYOUT, [MS-RDPEDISP] 2.2.2.2.1. The decoder takes
/// every value as sent; server::DisplayLayout applies the specification's
/// ranges (width and height, and which optional values count).
struct MonitorLayout {
    std::uint32_t flags = 0;
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t physical_width = 0;        ///< millimetres
    std::uint32_t physical_height = 0;       ///< millimetres
    std::uint32_t orientation = 0;           ///< degrees, see `orientation`
    std::uint32_t desktop_scale_factor = 0;  ///< percent
    std::uint32_t device_scale_factor = 0;   ///< percent

    [[nodiscard]] bool primary() const noexcept { return (flags & monitor_primary) != 0; }
    friend bool operator==(const MonitorLayout&, const MonitorLayout&) = default;
};

/// DISPLAYCONTROL_MONITOR_LAYOUT_PDU, [MS-RDPEDISP] 2.2.2.2 (client to server).
struct MonitorLayoutPdu {
    std::vector<MonitorLayout> monitors;

    friend bool operator==(const MonitorLayoutPdu&, const MonitorLayoutPdu&) = default;
};

using Pdu = std::variant<CapsPdu, MonitorLayoutPdu>;

/// Encodes one PDU with its header. A layout carries 1 to max_monitors monitors.
[[nodiscard]] std::vector<std::byte> encode(const Pdu& pdu);
void encode(Writer& w, const Pdu& pdu);

/// Decodes one DVC message, which holds exactly one PDU. Strict: the header's
/// Length must be the message size, MonitorLayoutSize must be 40, and a
/// layout must carry at least one and at most `monitor_limit` (itself capped
/// at max_monitors) monitors, filling the PDU exactly. Unknown PDU types are
/// Errc::unsupported.
[[nodiscard]] Result<Pdu> decode(std::span<const std::byte> message, std::uint32_t monitor_limit = max_monitors);

}  // namespace farland::channels::disp
