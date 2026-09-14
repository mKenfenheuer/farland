// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/channels/disp.hpp>

#include <algorithm>
#include <limits>

namespace farland::channels::disp {

namespace {

// [MS-RDPEDISP] 2.2.1.1 DISPLAYCONTROL_HEADER
void write_header(Writer& w, std::uint32_t type, std::size_t length)
{
    FARLAND_ASSERT(length <= std::numeric_limits<std::uint32_t>::max());
    w.u32le(type);
    w.u32le(static_cast<std::uint32_t>(length));
}

// [MS-RDPEDISP] 2.2.2.2.1 DISPLAYCONTROL_MONITOR_LAYOUT
Result<MonitorLayout> decode_monitor(Reader& r)
{
    MonitorLayout m;
    FARLAND_TRY(m.flags, r.u32le());
    FARLAND_TRY(const auto left, r.u32le());
    FARLAND_TRY(const auto top, r.u32le());
    m.left = static_cast<std::int32_t>(left);
    m.top = static_cast<std::int32_t>(top);
    FARLAND_TRY(m.width, r.u32le());
    FARLAND_TRY(m.height, r.u32le());
    FARLAND_TRY(m.physical_width, r.u32le());
    FARLAND_TRY(m.physical_height, r.u32le());
    FARLAND_TRY(m.orientation, r.u32le());
    FARLAND_TRY(m.desktop_scale_factor, r.u32le());
    FARLAND_TRY(m.device_scale_factor, r.u32le());
    return m;
}

void encode_monitor(Writer& w, const MonitorLayout& m)
{
    w.u32le(m.flags);
    w.u32le(static_cast<std::uint32_t>(m.left));
    w.u32le(static_cast<std::uint32_t>(m.top));
    w.u32le(m.width);
    w.u32le(m.height);
    w.u32le(m.physical_width);
    w.u32le(m.physical_height);
    w.u32le(m.orientation);
    w.u32le(m.desktop_scale_factor);
    w.u32le(m.device_scale_factor);
}

// [MS-RDPEDISP] 2.2.2.2 DISPLAYCONTROL_MONITOR_LAYOUT_PDU, after the header.
Result<MonitorLayoutPdu> decode_layout(Reader& r, std::uint32_t monitor_limit)
{
    FARLAND_TRY(const auto size, r.u32le());
    if (size != monitor_layout_size) {
        return fail(Errc::invalid_value, "MonitorLayoutSize is not 40", r.offset() - 4);
    }
    FARLAND_TRY(const auto count, r.u32le());
    if (count == 0) {
        return fail(Errc::invalid_value, "a monitor layout without monitors", r.offset() - 4);
    }
    if (count > std::min(monitor_limit, max_monitors)) {
        return fail(Errc::limit_exceeded, "more monitors than the server offered", r.offset() - 4);
    }
    if (r.remaining() != std::size_t{count} * monitor_layout_size) {
        return fail(Errc::invalid_length, "NumMonitors does not match the PDU length", r.offset() - 4);
    }
    MonitorLayoutPdu pdu;
    pdu.monitors.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        FARLAND_TRY(auto monitor, decode_monitor(r));
        pdu.monitors.push_back(monitor);
    }
    return pdu;
}

}  // namespace

void encode(Writer& w, const Pdu& pdu)
{
    if (const auto* caps = std::get_if<CapsPdu>(&pdu)) {
        // [MS-RDPEDISP] 2.2.2.1 DISPLAYCONTROL_CAPS_PDU
        write_header(w, pdu_type::caps, header_size + 12);
        w.u32le(caps->max_num_monitors);
        w.u32le(caps->max_monitor_area_factor_a);
        w.u32le(caps->max_monitor_area_factor_b);
        return;
    }
    const auto& layout = std::get<MonitorLayoutPdu>(pdu);
    FARLAND_ASSERT(!layout.monitors.empty() && layout.monitors.size() <= max_monitors);
    write_header(w, pdu_type::monitor_layout, header_size + 8 + (layout.monitors.size() * monitor_layout_size));
    w.u32le(monitor_layout_size);
    w.u32le(static_cast<std::uint32_t>(layout.monitors.size()));
    for (const auto& monitor : layout.monitors) {
        encode_monitor(w, monitor);
    }
}

std::vector<std::byte> encode(const Pdu& pdu)
{
    Writer w;
    encode(w, pdu);
    return std::move(w).take();
}

Result<Pdu> decode(std::span<const std::byte> message, std::uint32_t monitor_limit)
{
    Reader r(message);
    FARLAND_TRY(const auto type, r.u32le());
    FARLAND_TRY(const auto length, r.u32le());
    if (length != message.size()) {
        return fail(Errc::invalid_length, "the DISPLAYCONTROL_HEADER Length is not the message size", 4);
    }
    switch (type) {
    case pdu_type::caps: {
        CapsPdu caps;
        FARLAND_TRY(caps.max_num_monitors, r.u32le());
        FARLAND_TRY(caps.max_monitor_area_factor_a, r.u32le());
        FARLAND_TRY(caps.max_monitor_area_factor_b, r.u32le());
        FARLAND_TRY_VOID(r.expect_end("DISPLAYCONTROL_CAPS_PDU"));
        return caps;
    }
    case pdu_type::monitor_layout: {
        FARLAND_TRY(auto layout, decode_layout(r, monitor_limit));
        return layout;
    }
    default:
        return fail(Errc::unsupported, "unknown DISPLAYCONTROL PDU type", 0);
    }
}

}  // namespace farland::channels::disp
