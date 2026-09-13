// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/base/per.hpp>
#include <farland/base/text.hpp>
#include <farland/proto/gcc.hpp>

#include <algorithm>
#include <array>
#include <string_view>

namespace farland::proto::gcc {

namespace {

constexpr std::uint8_t choice_object_key = 0x00;
constexpr std::uint8_t choice_conference_create_request = 0x00;
constexpr std::uint8_t choice_conference_create_response = 0x14;
constexpr std::uint8_t selection_user_data_present = 0x08;
constexpr std::uint8_t choice_h221_non_standard_value = 0xC0;
constexpr std::string_view h221_client_key = "Duca";
constexpr std::string_view h221_server_key = "McDn";
constexpr std::size_t h221_key_size = 4;
constexpr std::uint16_t response_node_id = 0x79F3;
constexpr std::uint16_t node_id_min = 1001;
constexpr std::uint8_t gcc_result_alternatives = 16;

constexpr std::size_t block_header_size = 4;
constexpr std::size_t client_name_size = 32;
constexpr std::size_t ime_file_name_size = 64;
constexpr std::size_t dig_product_id_size = 64;
constexpr std::size_t channel_name_size = 8;
constexpr std::uint32_t monitor_attribute_size = 20;

std::span<const std::byte> as_bytes(std::string_view text)
{
    return std::as_bytes(std::span(text));
}

Result<void> expect_key(Reader& r, std::string_view key)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const auto value, per::read_octet_string(r, h221_key_size));
    if (!std::ranges::equal(value, as_bytes(key))) {
        return fail(Errc::invalid_value, "unexpected H.221 non-standard key", start);
    }
    return {};
}

/// Writes a UTF-16LE string into a fixed-size, zero-padded field, truncating
/// so that at least one terminating NUL remains.
void write_fixed_utf16(Writer& w, std::string_view text, std::size_t size)
{
    auto encoded = utf8_to_utf16le(text);
    if (encoded.size() > size - 2) {
        encoded.resize(size - 2);
    }
    w.bytes(encoded);
    w.zeros(size - encoded.size());
}

std::string ascii_name(std::span<const std::byte> field)
{
    std::string name;
    for (const std::byte b : field) {
        const auto c = std::to_integer<unsigned char>(b);
        if (c == 0) {
            break;
        }
        name.push_back(static_cast<char>(c));
    }
    return name;
}

// Client Core Data -----------------------------------------------------------

template <class T, class Read>
Result<bool> read_optional(Reader& r, std::optional<T>& field, Read read)
{
    if (r.empty()) {
        return false;
    }
    FARLAND_TRY(field, read(r));
    return true;
}

Result<ClientCoreData> decode_core(Reader& r)
{
    ClientCoreData c;
    FARLAND_TRY(c.version, r.u32le());
    FARLAND_TRY(c.desktop_width, r.u16le());
    FARLAND_TRY(c.desktop_height, r.u16le());
    FARLAND_TRY(c.color_depth, r.u16le());
    FARLAND_TRY(c.sas_sequence, r.u16le());
    FARLAND_TRY(c.keyboard_layout, r.u32le());
    FARLAND_TRY(c.client_build, r.u32le());
    FARLAND_TRY(const auto name, r.bytes(client_name_size));
    c.client_name = utf16le_to_utf8(name);
    FARLAND_TRY(c.keyboard_type, r.u32le());
    FARLAND_TRY(c.keyboard_subtype, r.u32le());
    FARLAND_TRY(c.keyboard_function_keys, r.u32le());
    FARLAND_TRY(const auto ime, r.bytes(ime_file_name_size));
    c.ime_file_name = utf16le_to_utf8(ime);

    const auto u16 = [](Reader& in) { return in.u16le(); };
    const auto u32 = [](Reader& in) { return in.u32le(); };
    FARLAND_TRY(bool more, read_optional(r, c.post_beta2_color_depth, u16));
    if (more) {
        FARLAND_TRY(more, read_optional(r, c.client_product_id, u16));
    }
    if (more) {
        FARLAND_TRY(more, read_optional(r, c.serial_number, u32));
    }
    if (more) {
        FARLAND_TRY(more, read_optional(r, c.high_color_depth, u16));
    }
    if (more) {
        FARLAND_TRY(more, read_optional(r, c.supported_color_depths, u16));
    }
    if (more) {
        FARLAND_TRY(more, read_optional(r, c.early_capability_flags, u16));
    }
    if (more) {
        FARLAND_TRY(more, read_optional(r, c.client_dig_product_id, [](Reader& in) -> Result<std::string> {
                        FARLAND_TRY(const auto id, in.bytes(dig_product_id_size));
                        return utf16le_to_utf8(id);
                    }));
    }
    if (more) {
        FARLAND_TRY(more, read_optional(r, c.connection_type, [](Reader& in) { return in.u8(); }));
    }
    if (more && !r.empty()) {
        FARLAND_TRY_VOID(r.skip(1));  // pad1octet
    }
    if (more) {
        FARLAND_TRY(more, read_optional(r, c.server_selected_protocol, u32));
    }
    if (more) {
        FARLAND_TRY(more, read_optional(r, c.desktop_physical_width, u32));
    }
    if (more) {
        FARLAND_TRY(more, read_optional(r, c.desktop_physical_height, u32));
    }
    if (more) {
        FARLAND_TRY(more, read_optional(r, c.desktop_orientation, u16));
    }
    if (more) {
        FARLAND_TRY(more, read_optional(r, c.desktop_scale_factor, u32));
    }
    if (more) {
        FARLAND_TRY(more, read_optional(r, c.device_scale_factor, u32));
    }
    // Newer clients may append fields farland does not know yet.
    return c;
}

void encode_core(Writer& w, const ClientCoreData& c)
{
    w.u32le(c.version);
    w.u16le(c.desktop_width);
    w.u16le(c.desktop_height);
    w.u16le(c.color_depth);
    w.u16le(c.sas_sequence);
    w.u32le(c.keyboard_layout);
    w.u32le(c.client_build);
    write_fixed_utf16(w, c.client_name, client_name_size);
    w.u32le(c.keyboard_type);
    w.u32le(c.keyboard_subtype);
    w.u32le(c.keyboard_function_keys);
    write_fixed_utf16(w, c.ime_file_name, ime_file_name_size);

    // Optional fields in order, stopping at the first absent one.
    bool stop = false;
    const auto opt16 = [&](const std::optional<std::uint16_t>& v) {
        stop = stop || !v;
        if (!stop) {
            w.u16le(*v);
        }
    };
    const auto opt32 = [&](const std::optional<std::uint32_t>& v) {
        stop = stop || !v;
        if (!stop) {
            w.u32le(*v);
        }
    };
    opt16(c.post_beta2_color_depth);
    opt16(c.client_product_id);
    opt32(c.serial_number);
    opt16(c.high_color_depth);
    opt16(c.supported_color_depths);
    opt16(c.early_capability_flags);
    stop = stop || !c.client_dig_product_id;
    if (!stop) {
        write_fixed_utf16(w, *c.client_dig_product_id, dig_product_id_size);
    }
    stop = stop || !c.connection_type;
    if (!stop) {
        w.u8(*c.connection_type);
        w.u8(0);  // pad1octet
    }
    opt32(c.server_selected_protocol);
    opt32(c.desktop_physical_width);
    opt32(c.desktop_physical_height);
    opt16(c.desktop_orientation);
    opt32(c.desktop_scale_factor);
    opt32(c.device_scale_factor);
}

// Other client blocks --------------------------------------------------------

Result<ClientNetworkData> decode_network(Reader& r)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint32_t count, r.u32le());
    if (count > max_static_channels) {
        return fail(Errc::limit_exceeded, "more than 31 static virtual channels", start);
    }
    ClientNetworkData net;
    for (std::uint32_t i = 0; i < count; ++i) {
        FARLAND_TRY(const auto name, r.bytes(channel_name_size));
        FARLAND_TRY(const std::uint32_t options, r.u32le());
        net.channels.push_back(ChannelDef{ascii_name(name), options});
    }
    return net;
}

Result<ClientMonitorData> decode_monitor(Reader& r)
{
    ClientMonitorData data;
    FARLAND_TRY(data.flags, r.u32le());
    const std::size_t count_offset = r.offset();
    FARLAND_TRY(const std::uint32_t count, r.u32le());
    if (count > max_monitors) {
        return fail(Errc::limit_exceeded, "more than 16 monitors", count_offset);
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        MonitorDef m;
        FARLAND_TRY(const std::uint32_t left, r.u32le());
        FARLAND_TRY(const std::uint32_t top, r.u32le());
        FARLAND_TRY(const std::uint32_t right, r.u32le());
        FARLAND_TRY(const std::uint32_t bottom, r.u32le());
        FARLAND_TRY(m.flags, r.u32le());
        m.left = static_cast<std::int32_t>(left);
        m.top = static_cast<std::int32_t>(top);
        m.right = static_cast<std::int32_t>(right);
        m.bottom = static_cast<std::int32_t>(bottom);
        data.monitors.push_back(m);
    }
    return data;
}

Result<ClientMonitorExtendedData> decode_monitor_ex(Reader& r)
{
    ClientMonitorExtendedData data;
    FARLAND_TRY(data.flags, r.u32le());
    const std::size_t size_offset = r.offset();
    FARLAND_TRY(const std::uint32_t attribute_size, r.u32le());
    if (attribute_size != monitor_attribute_size) {
        return fail(Errc::invalid_value, "monitorAttributeSize is not 20", size_offset);
    }
    const std::size_t count_offset = r.offset();
    FARLAND_TRY(const std::uint32_t count, r.u32le());
    if (count > max_monitors) {
        return fail(Errc::limit_exceeded, "more than 16 monitors", count_offset);
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        MonitorAttributes m;
        FARLAND_TRY(m.physical_width, r.u32le());
        FARLAND_TRY(m.physical_height, r.u32le());
        FARLAND_TRY(m.orientation, r.u32le());
        FARLAND_TRY(m.desktop_scale_factor, r.u32le());
        FARLAND_TRY(m.device_scale_factor, r.u32le());
        data.monitors.push_back(m);
    }
    return data;
}

template <class T>
Result<void> store_once(std::optional<T>& slot, Result<T> value, std::size_t offset)
{
    if (!value) {
        return std::unexpected(value.error());
    }
    if (slot) {
        return fail(Errc::invalid_value, "duplicate GCC data block", offset);
    }
    slot = std::move(*value);
    return {};
}

std::size_t begin_block(Writer& w, std::uint16_t type)
{
    const std::size_t start = w.size();
    w.u16le(type);
    w.u16le(0);  // length, patched by end_block
    return start;
}

void end_block(Writer& w, std::size_t start)
{
    const std::size_t length = w.size() - start;
    FARLAND_ASSERT(length <= 0xFFFF);
    w.patch_u16le(start + 2, static_cast<std::uint16_t>(length));
}

}  // namespace

// Conference Create Request / Response ----------------------------------------

Result<std::span<const std::byte>> decode_conference_create_request(Reader& r)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint8_t key_choice, per::read_choice(r));
    FARLAND_TRY(const auto oid, per::read_object_identifier(r));
    if (key_choice != choice_object_key || oid != per::t124_02_98_oid) {
        return fail(Errc::invalid_value, "GCC ConnectData key is not the T.124 object identifier", start);
    }
    // The connectPDU length is not reliable: Windows servers write 0x2a
    // whatever follows ([MS-RDPBCGR] 4.1.4), and FreeRDP does the same. It is
    // read and ignored; the PDU extends to the end of the MCS user data.
    FARLAND_TRY_VOID(per::read_length(r).transform([](auto) {}));
    Reader& pdu = r;

    const std::size_t pdu_start = pdu.offset();
    FARLAND_TRY(const std::uint8_t choice, per::read_choice(pdu));
    FARLAND_TRY(const std::uint8_t selection, per::read_selection(pdu));
    if (choice != choice_conference_create_request || (selection & selection_user_data_present) == 0) {
        return fail(Errc::invalid_value, "not a Conference Create Request with user data", pdu_start);
    }
    FARLAND_TRY_VOID(per::read_numeric_string(pdu, 1).transform([](auto) {}));  // conferenceName
    FARLAND_TRY_VOID(per::read_padding(pdu, 1));
    const std::size_t sets_offset = pdu.offset();
    FARLAND_TRY(const std::uint8_t sets, per::read_number_of_sets(pdu));
    FARLAND_TRY(const std::uint8_t value_choice, per::read_choice(pdu));
    if (sets < 1 || value_choice != choice_h221_non_standard_value) {
        return fail(Errc::invalid_value, "Conference Create Request user data is not H.221 non-standard", sets_offset);
    }
    FARLAND_TRY_VOID(expect_key(pdu, h221_client_key));
    FARLAND_TRY(const std::size_t data_length, per::read_length(pdu));
    FARLAND_TRY(const auto data, pdu.bytes(data_length));
    // Further user data sets, if any, carry nothing RDP uses.
    return data;
}

void encode_conference_create_request(Writer& w, std::span<const std::byte> client_data)
{
    Writer pdu;
    per::write_choice(pdu, choice_conference_create_request);
    per::write_selection(pdu, selection_user_data_present);
    per::write_numeric_string(pdu, "1", 1);
    per::write_padding(pdu, 1);
    per::write_number_of_sets(pdu, 1);
    per::write_choice(pdu, choice_h221_non_standard_value);
    per::write_octet_string(pdu, as_bytes(h221_client_key), h221_key_size);
    per::write_length(pdu, client_data.size());
    pdu.bytes(client_data);

    per::write_choice(w, choice_object_key);
    per::write_object_identifier(w, per::t124_02_98_oid);
    per::write_length(w, pdu.size());
    w.bytes(pdu.view());
}

Result<std::span<const std::byte>> decode_conference_create_response(Reader& r)
{
    const std::size_t start = r.offset();
    FARLAND_TRY(const std::uint8_t key_choice, per::read_choice(r));
    FARLAND_TRY(const auto oid, per::read_object_identifier(r));
    if (key_choice != choice_object_key || oid != per::t124_02_98_oid) {
        return fail(Errc::invalid_value, "GCC ConnectData key is not the T.124 object identifier", start);
    }
    // The connectPDU length is not reliable: Windows servers write 0x2a
    // whatever follows ([MS-RDPBCGR] 4.1.4), and FreeRDP does the same. It is
    // read and ignored; the PDU extends to the end of the MCS user data.
    FARLAND_TRY_VOID(per::read_length(r).transform([](auto) {}));
    Reader& pdu = r;

    const std::size_t pdu_start = pdu.offset();
    FARLAND_TRY(const std::uint8_t choice, per::read_choice(pdu));
    if (choice != choice_conference_create_response) {
        return fail(Errc::invalid_value, "not a Conference Create Response", pdu_start);
    }
    FARLAND_TRY_VOID(per::read_integer16(pdu, node_id_min).transform([](auto) {}));  // nodeID
    FARLAND_TRY_VOID(per::read_integer(pdu).transform([](auto) {}));                 // tag
    const std::size_t result_offset = pdu.offset();
    FARLAND_TRY(const std::uint8_t result, per::read_enumerated(pdu, gcc_result_alternatives));
    if (result != 0) {
        return fail(Errc::invalid_value, "Conference Create Response reports failure", result_offset);
    }
    FARLAND_TRY(const std::uint8_t sets, per::read_number_of_sets(pdu));
    FARLAND_TRY(const std::uint8_t value_choice, per::read_choice(pdu));
    if (sets < 1 || value_choice != choice_h221_non_standard_value) {
        return fail(Errc::invalid_value, "Conference Create Response user data is not H.221 non-standard",
                    result_offset);
    }
    FARLAND_TRY_VOID(expect_key(pdu, h221_server_key));
    FARLAND_TRY(const std::size_t data_length, per::read_length(pdu));
    return pdu.bytes(data_length);
}

void encode_conference_create_response(Writer& w, std::span<const std::byte> server_data)
{
    // ConnectGCCPDU conferenceCreateResponse, [MS-RDPBCGR] 2.2.1.4 and the
    // annotated example in 4.1.4.
    Writer pdu;
    per::write_choice(pdu, choice_conference_create_response);
    per::write_integer16(pdu, response_node_id, node_id_min);
    per::write_integer(pdu, 1);     // tag
    per::write_enumerated(pdu, 0);  // result: success
    per::write_number_of_sets(pdu, 1);
    per::write_choice(pdu, choice_h221_non_standard_value);
    per::write_octet_string(pdu, as_bytes(h221_server_key), h221_key_size);
    per::write_length(pdu, server_data.size());
    pdu.bytes(server_data);

    per::write_choice(w, choice_object_key);
    per::write_object_identifier(w, per::t124_02_98_oid);
    per::write_length(w, pdu.size());
    w.bytes(pdu.view());
}

// Data blocks -------------------------------------------------------------------

Result<ClientData> decode_client_data(Reader& r)
{
    ClientData data;
    bool have_core = false;
    while (!r.empty()) {
        const std::size_t start = r.offset();
        FARLAND_TRY(const std::uint16_t type, r.u16le());
        FARLAND_TRY(const std::uint16_t length, r.u16le());
        if (length < block_header_size) {
            return fail(Errc::invalid_length, "GCC data block shorter than its header", start);
        }
        FARLAND_TRY(Reader block, r.sub(length - block_header_size));
        switch (type) {
        case block_type::cs_core: {
            if (have_core) {
                return fail(Errc::invalid_value, "duplicate GCC data block", start);
            }
            FARLAND_TRY(data.core, decode_core(block));
            have_core = true;
            break;
        }
        case block_type::cs_security: {
            auto parse = [&block]() -> Result<ClientSecurityData> {
                ClientSecurityData s;
                FARLAND_TRY(s.encryption_methods, block.u32le());
                FARLAND_TRY(s.ext_encryption_methods, block.u32le());
                return s;
            };
            FARLAND_TRY_VOID(store_once(data.security, parse(), start));
            break;
        }
        case block_type::cs_net:
            FARLAND_TRY_VOID(store_once(data.network, decode_network(block), start));
            break;
        case block_type::cs_cluster: {
            auto parse = [&block]() -> Result<ClientClusterData> {
                ClientClusterData c;
                FARLAND_TRY(c.flags, block.u32le());
                FARLAND_TRY(c.redirected_session_id, block.u32le());
                return c;
            };
            FARLAND_TRY_VOID(store_once(data.cluster, parse(), start));
            break;
        }
        case block_type::cs_monitor:
            FARLAND_TRY_VOID(store_once(data.monitor, decode_monitor(block), start));
            break;
        case block_type::cs_mcs_msgchannel: {
            auto parse = [&block]() -> Result<ClientMessageChannelData> {
                ClientMessageChannelData m;
                FARLAND_TRY(m.flags, block.u32le());
                return m;
            };
            FARLAND_TRY_VOID(store_once(data.message_channel, parse(), start));
            break;
        }
        case block_type::cs_monitor_ex:
            FARLAND_TRY_VOID(store_once(data.monitor_ex, decode_monitor_ex(block), start));
            break;
        case block_type::cs_multitransport: {
            auto parse = [&block]() -> Result<ClientMultitransportData> {
                ClientMultitransportData m;
                FARLAND_TRY(m.flags, block.u32le());
                return m;
            };
            FARLAND_TRY_VOID(store_once(data.multitransport, parse(), start));
            break;
        }
        default:
            break;  // Unknown blocks are skipped; the length makes that safe.
        }
    }
    if (!have_core) {
        return fail(Errc::invalid_value, "client data has no core block", r.offset());
    }
    return data;
}

void encode_client_data(Writer& w, const ClientData& data)
{
    std::size_t block = begin_block(w, block_type::cs_core);
    encode_core(w, data.core);
    end_block(w, block);

    if (data.cluster) {
        block = begin_block(w, block_type::cs_cluster);
        w.u32le(data.cluster->flags);
        w.u32le(data.cluster->redirected_session_id);
        end_block(w, block);
    }
    if (data.security) {
        block = begin_block(w, block_type::cs_security);
        w.u32le(data.security->encryption_methods);
        w.u32le(data.security->ext_encryption_methods);
        end_block(w, block);
    }
    if (data.network) {
        FARLAND_ASSERT(data.network->channels.size() <= max_static_channels);
        block = begin_block(w, block_type::cs_net);
        w.u32le(static_cast<std::uint32_t>(data.network->channels.size()));
        for (const auto& channel : data.network->channels) {
            FARLAND_ASSERT(channel.name.size() < channel_name_size);
            w.bytes(as_bytes(channel.name));
            w.zeros(channel_name_size - channel.name.size());
            w.u32le(channel.options);
        }
        end_block(w, block);
    }
    if (data.monitor) {
        FARLAND_ASSERT(data.monitor->monitors.size() <= max_monitors);
        block = begin_block(w, block_type::cs_monitor);
        w.u32le(data.monitor->flags);
        w.u32le(static_cast<std::uint32_t>(data.monitor->monitors.size()));
        for (const auto& m : data.monitor->monitors) {
            w.u32le(static_cast<std::uint32_t>(m.left));
            w.u32le(static_cast<std::uint32_t>(m.top));
            w.u32le(static_cast<std::uint32_t>(m.right));
            w.u32le(static_cast<std::uint32_t>(m.bottom));
            w.u32le(m.flags);
        }
        end_block(w, block);
    }
    if (data.message_channel) {
        block = begin_block(w, block_type::cs_mcs_msgchannel);
        w.u32le(data.message_channel->flags);
        end_block(w, block);
    }
    if (data.monitor_ex) {
        FARLAND_ASSERT(data.monitor_ex->monitors.size() <= max_monitors);
        block = begin_block(w, block_type::cs_monitor_ex);
        w.u32le(data.monitor_ex->flags);
        w.u32le(monitor_attribute_size);
        w.u32le(static_cast<std::uint32_t>(data.monitor_ex->monitors.size()));
        for (const auto& m : data.monitor_ex->monitors) {
            w.u32le(m.physical_width);
            w.u32le(m.physical_height);
            w.u32le(m.orientation);
            w.u32le(m.desktop_scale_factor);
            w.u32le(m.device_scale_factor);
        }
        end_block(w, block);
    }
    if (data.multitransport) {
        block = begin_block(w, block_type::cs_multitransport);
        w.u32le(data.multitransport->flags);
        end_block(w, block);
    }
}

Result<ServerData> decode_server_data(Reader& r)
{
    ServerData data;
    bool have_core = false;
    bool have_security = false;
    bool have_network = false;
    while (!r.empty()) {
        const std::size_t start = r.offset();
        FARLAND_TRY(const std::uint16_t type, r.u16le());
        FARLAND_TRY(const std::uint16_t length, r.u16le());
        if (length < block_header_size) {
            return fail(Errc::invalid_length, "GCC data block shorter than its header", start);
        }
        FARLAND_TRY(Reader block, r.sub(length - block_header_size));
        switch (type) {
        case block_type::sc_core: {
            FARLAND_TRY(data.core.version, block.u32le());
            if (!block.empty()) {
                FARLAND_TRY(data.core.client_requested_protocols, block.u32le());
            }
            if (!block.empty()) {
                FARLAND_TRY(data.core.early_capability_flags, block.u32le());
            }
            have_core = true;
            break;
        }
        case block_type::sc_security: {
            // Server random and certificate follow only under Standard RDP
            // Security; the block length lets us skip them.
            FARLAND_TRY(data.security.encryption_method, block.u32le());
            FARLAND_TRY(data.security.encryption_level, block.u32le());
            have_security = true;
            break;
        }
        case block_type::sc_net: {
            FARLAND_TRY(data.network.io_channel_id, block.u16le());
            const std::size_t count_offset = block.offset();
            FARLAND_TRY(const std::uint16_t count, block.u16le());
            if (count > max_static_channels) {
                return fail(Errc::limit_exceeded, "more than 31 static virtual channels", count_offset);
            }
            for (std::uint16_t i = 0; i < count; ++i) {
                FARLAND_TRY(const std::uint16_t id, block.u16le());
                data.network.channel_ids.push_back(id);
            }
            have_network = true;
            break;
        }
        case block_type::sc_mcs_msgchannel: {
            FARLAND_TRY(data.message_channel_id, block.u16le());
            break;
        }
        case block_type::sc_multitransport: {
            FARLAND_TRY(data.multitransport_flags, block.u32le());
            break;
        }
        default:
            break;
        }
    }
    if (!have_core || !have_security || !have_network) {
        return fail(Errc::invalid_value, "server data lacks a core, security or network block", r.offset());
    }
    return data;
}

void encode_server_data(Writer& w, const ServerData& data)
{
    std::size_t block = begin_block(w, block_type::sc_core);
    w.u32le(data.core.version);
    FARLAND_ASSERT(data.core.client_requested_protocols || !data.core.early_capability_flags);
    if (data.core.client_requested_protocols) {
        w.u32le(*data.core.client_requested_protocols);
    }
    if (data.core.early_capability_flags) {
        w.u32le(*data.core.early_capability_flags);
    }
    end_block(w, block);

    block = begin_block(w, block_type::sc_security);
    w.u32le(data.security.encryption_method);
    w.u32le(data.security.encryption_level);
    end_block(w, block);

    FARLAND_ASSERT(data.network.channel_ids.size() <= max_static_channels);
    block = begin_block(w, block_type::sc_net);
    w.u16le(data.network.io_channel_id);
    w.u16le(static_cast<std::uint16_t>(data.network.channel_ids.size()));
    for (const std::uint16_t id : data.network.channel_ids) {
        w.u16le(id);
    }
    if (data.network.channel_ids.size() % 2 != 0) {
        w.u16le(0);  // Pad to a multiple of four bytes, [MS-RDPBCGR] 2.2.1.4.4.
    }
    end_block(w, block);

    if (data.message_channel_id) {
        block = begin_block(w, block_type::sc_mcs_msgchannel);
        w.u16le(*data.message_channel_id);
        end_block(w, block);
    }
    if (data.multitransport_flags) {
        block = begin_block(w, block_type::sc_multitransport);
        w.u32le(*data.multitransport_flags);
        end_block(w, block);
    }
}

}  // namespace farland::proto::gcc
