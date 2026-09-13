// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/channels/drdynvc.hpp>

#include <bit>
#include <limits>
#include <utility>

namespace farland::channels::drdynvc {

namespace {

/// The DVC header byte: cbId (bits 0-1), Sp/Pri/Len (bits 2-3), Cmd (bits
/// 4-7), [MS-RDPEDYC] 2.2.
struct Header {
    std::uint8_t command = 0;
    std::uint8_t sp = 0;
    std::uint8_t cb_id = 0;
};

std::uint8_t header_byte(std::uint8_t command, std::uint8_t sp, std::uint8_t cb_id)
{
    FARLAND_ASSERT(command <= 0x0F && sp <= 3 && cb_id <= 3);
    return static_cast<std::uint8_t>((command << 4U) | (sp << 2U) | cb_id);
}

Result<Header> read_header(Reader& r)
{
    FARLAND_TRY(const auto byte, r.u8());
    return Header{static_cast<std::uint8_t>(byte >> 4U), static_cast<std::uint8_t>((byte >> 2U) & 0x03U),
                  static_cast<std::uint8_t>(byte & 0x03U)};
}

// [MS-RDPEDYC] 2.2.2.1: a null-terminated ANSI string.
Result<std::string> read_channel_name(Reader& r)
{
    std::string name;
    for (;;) {
        FARLAND_TRY(const auto c, r.u8());
        if (c == 0) {
            break;
        }
        if (name.size() == max_channel_name_size) {
            return fail(Errc::limit_exceeded, "DVC channel name too long", r.offset());
        }
        name.push_back(static_cast<char>(c));
    }
    if (name.empty()) {
        return fail(Errc::invalid_value, "empty DVC channel name", r.offset());
    }
    return name;
}

// [MS-RDPEDYC] 2.2.5.1
Result<SoftSyncRequest> read_soft_sync_request(Reader& r)
{
    FARLAND_TRY_VOID(r.skip(1));  // Pad
    FARLAND_TRY(const auto length, r.u32le());
    // Length covers itself, Flags, NumberOfTunnels and the channel lists.
    if (length < 8 || std::size_t{length} - 4 != r.remaining()) {
        return fail(Errc::invalid_length, "Soft-Sync Request length disagrees with the PDU", r.offset());
    }
    SoftSyncRequest pdu;
    FARLAND_TRY(pdu.flags, r.u16le());
    FARLAND_TRY(pdu.number_of_tunnels, r.u16le());
    if ((pdu.flags & soft_sync_flag::channel_list_present) != 0) {
        for (std::uint16_t i = 0; i < pdu.number_of_tunnels; ++i) {
            SoftSyncChannelList list;
            FARLAND_TRY(list.tunnel_type, r.u32le());
            FARLAND_TRY(const auto count, r.u16le());
            if (std::size_t{count} * 4 > r.remaining()) {
                return fail(Errc::truncated, "Soft-Sync channel list exceeds the PDU", r.offset());
            }
            list.channel_ids.reserve(count);
            for (std::uint16_t j = 0; j < count; ++j) {
                FARLAND_TRY(const auto id, r.u32le());
                list.channel_ids.push_back(id);
            }
            pdu.channel_lists.push_back(std::move(list));
        }
    }
    FARLAND_TRY_VOID(r.expect_end("Soft-Sync Request"));
    return pdu;
}

// [MS-RDPEDYC] 2.2.5.2
Result<SoftSyncResponse> read_soft_sync_response(Reader& r)
{
    FARLAND_TRY_VOID(r.skip(1));  // Pad
    FARLAND_TRY(const auto count, r.u32le());
    if (count > r.remaining() / 4 || std::size_t{count} * 4 != r.remaining()) {
        return fail(Errc::invalid_length, "Soft-Sync Response tunnel count disagrees with the PDU", r.offset());
    }
    SoftSyncResponse pdu;
    pdu.tunnels.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        FARLAND_TRY(const auto tunnel, r.u32le());
        pdu.tunnels.push_back(tunnel);
    }
    return pdu;
}

/// The PDUs that are the same in both directions: data and close.
template <class Pdu>
Result<Pdu> decode_common(const Header& header, Reader& r)
{
    switch (header.command) {
    case cmd::data_first:
    case cmd::data_first_compressed: {
        // [MS-RDPEDYC] 2.2.3.1, 2.2.3.3: Sp is Len, the size of Length.
        FARLAND_TRY(const auto id, read_var(r, header.cb_id));
        FARLAND_TRY(const auto length, read_var(r, header.sp));
        const auto data = r.rest();
        if (header.command == cmd::data_first_compressed) {
            return DataFirstCompressed{id, length, data};
        }
        if (data.size() > length) {
            return fail(Errc::invalid_length, "DVC Data First holds more than its Length", r.offset());
        }
        return DataFirst{id, length, data};
    }
    case cmd::data:
    case cmd::data_compressed: {
        // [MS-RDPEDYC] 2.2.3.2, 2.2.3.4
        FARLAND_TRY(const auto id, read_var(r, header.cb_id));
        if (header.command == cmd::data_compressed) {
            return DataCompressed{id, r.rest()};
        }
        return Data{id, r.rest()};
    }
    case cmd::close: {
        // [MS-RDPEDYC] 2.2.4
        FARLAND_TRY(const auto id, read_var(r, header.cb_id));
        FARLAND_TRY_VOID(r.expect_end("DVC Close"));
        return Close{id};
    }
    default:
        return fail(Errc::invalid_value, "unknown or misdirected drdynvc command", 0);
    }
}

void write_id_pdu(Writer& w, std::uint8_t command, std::uint8_t sp, std::uint32_t channel_id)
{
    w.u8(header_byte(command, sp, size_code(channel_id)));
    write_var(w, channel_id);
}

template <class T>
std::vector<std::byte> encode_one(const T& pdu)
{
    Writer w;
    encode(w, pdu);
    return std::move(w).take();
}

}  // namespace

std::uint8_t size_code(std::uint32_t value) noexcept
{
    if (value <= 0xFF) {
        return 0;
    }
    if (value <= 0xFFFF) {
        return 1;
    }
    return 2;
}

std::size_t field_size(std::uint8_t code) noexcept
{
    switch (code) {
    case 0:
        return 1;
    case 1:
        return 2;
    default:
        return 4;
    }
}

Result<std::uint32_t> read_var(Reader& r, std::uint8_t code)
{
    switch (code) {
    case 0:
        return r.u8();
    case 1:
        return r.u16le();
    case 2:
        return r.u32le();
    default:
        return fail(Errc::invalid_value, "drdynvc field size code 3", r.offset());
    }
}

void write_var(Writer& w, std::uint32_t value)
{
    switch (size_code(value)) {
    case 0:
        w.u8(static_cast<std::uint8_t>(value));
        break;
    case 1:
        w.u16le(static_cast<std::uint16_t>(value));
        break;
    default:
        w.u32le(value);
        break;
    }
}

Result<ClientPdu> decode_client_pdu(std::span<const std::byte> message)
{
    Reader r(message);
    FARLAND_TRY(const auto header, read_header(r));
    switch (header.command) {
    case cmd::capabilities: {
        // [MS-RDPEDYC] 2.2.1.2
        FARLAND_TRY_VOID(r.skip(1));  // Pad
        CapsResponse pdu;
        FARLAND_TRY(pdu.version, r.u16le());
        FARLAND_TRY_VOID(r.expect_end("DVC Capabilities Response"));
        return pdu;
    }
    case cmd::create: {
        // [MS-RDPEDYC] 2.2.2.2
        CreateResponse pdu;
        FARLAND_TRY(pdu.channel_id, read_var(r, header.cb_id));
        FARLAND_TRY(const auto status, r.u32le());
        pdu.creation_status = std::bit_cast<std::int32_t>(status);
        FARLAND_TRY_VOID(r.expect_end("DVC Create Response"));
        return pdu;
    }
    case cmd::soft_sync_response: {
        FARLAND_TRY(auto pdu, read_soft_sync_response(r));
        return pdu;
    }
    default:
        return decode_common<ClientPdu>(header, r);
    }
}

Result<ServerPdu> decode_server_pdu(std::span<const std::byte> message)
{
    Reader r(message);
    FARLAND_TRY(const auto header, read_header(r));
    switch (header.command) {
    case cmd::capabilities: {
        // [MS-RDPEDYC] 2.2.1.1.1 - 2.2.1.1.3
        FARLAND_TRY_VOID(r.skip(1));  // Pad
        CapsRequest pdu;
        FARLAND_TRY(pdu.version, r.u16le());
        pdu.priority_charges = {};
        if (pdu.version == version2 || pdu.version == version3) {
            for (auto& charge : pdu.priority_charges) {
                FARLAND_TRY(charge, r.u16le());
            }
        } else if (pdu.version != version1) {
            return fail(Errc::invalid_value, "unknown DVC capabilities version", 2);
        }
        FARLAND_TRY_VOID(r.expect_end("DVC Capabilities Request"));
        return pdu;
    }
    case cmd::create: {
        // [MS-RDPEDYC] 2.2.2.1: Sp is Pri.
        CreateRequest pdu;
        pdu.priority = header.sp;
        FARLAND_TRY(pdu.channel_id, read_var(r, header.cb_id));
        FARLAND_TRY(pdu.name, read_channel_name(r));
        FARLAND_TRY_VOID(r.expect_end("DVC Create Request"));
        return pdu;
    }
    case cmd::soft_sync_request: {
        FARLAND_TRY(auto pdu, read_soft_sync_request(r));
        return pdu;
    }
    default:
        return decode_common<ServerPdu>(header, r);
    }
}

void encode(Writer& w, const CapsRequest& pdu)
{
    FARLAND_ASSERT(pdu.version >= version1 && pdu.version <= version3);
    w.u8(header_byte(cmd::capabilities, 0, 0));
    w.u8(0);  // Pad
    w.u16le(pdu.version);
    if (pdu.version >= version2) {
        for (const auto charge : pdu.priority_charges) {
            w.u16le(charge);
        }
    }
}

void encode(Writer& w, const CapsResponse& pdu)
{
    w.u8(header_byte(cmd::capabilities, 0, 0));
    w.u8(0);  // Pad
    w.u16le(pdu.version);
}

void encode(Writer& w, const CreateRequest& pdu)
{
    FARLAND_ASSERT(pdu.priority <= 3);
    FARLAND_ASSERT(!pdu.name.empty() && pdu.name.size() <= max_channel_name_size);
    FARLAND_ASSERT(pdu.name.find('\0') == std::string::npos);
    write_id_pdu(w, cmd::create, pdu.priority, pdu.channel_id);
    for (const char c : pdu.name) {
        w.u8(static_cast<std::uint8_t>(c));
    }
    w.u8(0);
}

void encode(Writer& w, const CreateResponse& pdu)
{
    write_id_pdu(w, cmd::create, 0, pdu.channel_id);
    w.u32le(std::bit_cast<std::uint32_t>(pdu.creation_status));
}

void encode(Writer& w, const DataFirst& pdu)
{
    FARLAND_ASSERT(pdu.data.size() <= pdu.length);
    write_id_pdu(w, cmd::data_first, size_code(pdu.length), pdu.channel_id);
    write_var(w, pdu.length);
    w.bytes(pdu.data);
}

void encode(Writer& w, const Data& pdu)
{
    write_id_pdu(w, cmd::data, 0, pdu.channel_id);
    w.bytes(pdu.data);
}

void encode(Writer& w, const DataFirstCompressed& pdu)
{
    write_id_pdu(w, cmd::data_first_compressed, size_code(pdu.length), pdu.channel_id);
    write_var(w, pdu.length);
    w.bytes(pdu.data);
}

void encode(Writer& w, const DataCompressed& pdu)
{
    write_id_pdu(w, cmd::data_compressed, 0, pdu.channel_id);
    w.bytes(pdu.data);
}

void encode(Writer& w, const Close& pdu)
{
    write_id_pdu(w, cmd::close, 0, pdu.channel_id);
}

void encode(Writer& w, const SoftSyncRequest& pdu)
{
    const bool present = (pdu.flags & soft_sync_flag::channel_list_present) != 0;
    FARLAND_ASSERT(present ? pdu.channel_lists.size() == pdu.number_of_tunnels : pdu.channel_lists.empty());
    std::size_t length = 8;
    for (const auto& list : pdu.channel_lists) {
        FARLAND_ASSERT(list.channel_ids.size() <= std::numeric_limits<std::uint16_t>::max());
        length += 6 + (4 * list.channel_ids.size());
    }
    FARLAND_ASSERT(length <= std::numeric_limits<std::uint32_t>::max());
    w.u8(header_byte(cmd::soft_sync_request, 0, 0));
    w.u8(0);  // Pad
    w.u32le(static_cast<std::uint32_t>(length));
    w.u16le(pdu.flags);
    w.u16le(pdu.number_of_tunnels);
    for (const auto& list : pdu.channel_lists) {
        w.u32le(list.tunnel_type);
        w.u16le(static_cast<std::uint16_t>(list.channel_ids.size()));
        for (const auto id : list.channel_ids) {
            w.u32le(id);
        }
    }
}

void encode(Writer& w, const SoftSyncResponse& pdu)
{
    FARLAND_ASSERT(pdu.tunnels.size() <= std::numeric_limits<std::uint32_t>::max());
    w.u8(header_byte(cmd::soft_sync_response, 0, 0));
    w.u8(0);  // Pad
    w.u32le(static_cast<std::uint32_t>(pdu.tunnels.size()));
    for (const auto tunnel : pdu.tunnels) {
        w.u32le(tunnel);
    }
}

std::vector<std::byte> encode_client_pdu(const ClientPdu& pdu)
{
    return std::visit([](const auto& p) { return encode_one(p); }, pdu);
}

std::vector<std::byte> encode_server_pdu(const ServerPdu& pdu)
{
    return std::visit([](const auto& p) { return encode_one(p); }, pdu);
}

std::vector<std::vector<std::byte>> encode_data(std::uint32_t channel_id, std::span<const std::byte> message)
{
    FARLAND_ASSERT(message.size() <= std::numeric_limits<std::uint32_t>::max());
    std::vector<std::vector<std::byte>> pdus;
    if (message.size() <= max_single_data_size) {
        pdus.push_back(encode_one(Data{channel_id, message}));
        return pdus;
    }
    // [MS-RDPEDYC] 2.2.3.1: DATA_FIRST carries 1600 minus its header, or the
    // whole message when that is shorter.
    const auto total = static_cast<std::uint32_t>(message.size());
    const std::size_t id_size = field_size(size_code(channel_id));
    const std::size_t first_block = max_pdu_size - (1 + id_size + field_size(size_code(total)));
    const std::size_t block = max_pdu_size - (1 + id_size);
    pdus.reserve(2 + ((message.size() - std::min(first_block, message.size())) / block));

    std::size_t offset = std::min(first_block, message.size());
    pdus.push_back(encode_one(DataFirst{channel_id, total, message.first(offset)}));
    while (offset < message.size()) {
        const std::size_t size = std::min(block, message.size() - offset);
        pdus.push_back(encode_one(Data{channel_id, message.subspan(offset, size)}));
        offset += size;
    }
    return pdus;
}

void MessageReassembler::reset() noexcept
{
    total_.reset();
    std::vector<std::byte>().swap(buffer_);
}

Result<std::optional<std::vector<std::byte>>> MessageReassembler::first(std::uint32_t length,
                                                                        std::span<const std::byte> block)
{
    if (total_) {
        reset();
        return fail(Errc::invalid_value, "DVC Data First inside a fragmented message", 0);
    }
    if (length > max_size_) {
        return fail(Errc::limit_exceeded, "DVC message too large", 0);
    }
    if (block.size() > length) {
        return fail(Errc::invalid_length, "DVC Data First block exceeds the message length", 0);
    }
    if (block.size() == length) {
        return std::vector<std::byte>(block.begin(), block.end());
    }
    total_ = length;
    buffer_.assign(block.begin(), block.end());
    return std::nullopt;
}

Result<std::optional<std::vector<std::byte>>> MessageReassembler::next(std::span<const std::byte> block)
{
    if (!total_) {
        if (block.size() > max_size_) {
            return fail(Errc::limit_exceeded, "DVC message too large", 0);
        }
        return std::vector<std::byte>(block.begin(), block.end());
    }
    if (block.size() > *total_ - buffer_.size()) {
        reset();
        return fail(Errc::invalid_length, "DVC data overruns the message length", 0);
    }
    buffer_.insert(buffer_.end(), block.begin(), block.end());
    if (buffer_.size() < *total_) {
        return std::nullopt;
    }
    total_.reset();
    return std::exchange(buffer_, {});
}

}  // namespace farland::channels::drdynvc
