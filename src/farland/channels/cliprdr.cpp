// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/base/text.hpp>
#include <farland/channels/cliprdr.hpp>

#include <algorithm>
#include <limits>
#include <type_traits>

namespace farland::channels::cliprdr {

namespace {

/// Size of wszTempDir and of CLIPRDR_FILEDESCRIPTOR fileName.
constexpr std::size_t path_field_size = 520;
/// Size of CLIPRDR_SHORT_FORMAT_NAME formatName.
constexpr std::size_t short_name_size = 32;
constexpr std::size_t short_format_size = 4 + short_name_size;
constexpr std::size_t general_capability_size = 12;

/// Writes `text` as UTF-16LE into a zero-filled field of `size` bytes,
/// truncated so that a terminator fits and no surrogate pair is split.
void write_fixed_utf16(Writer& w, std::string_view text, std::size_t size)
{
    auto units = utf8_to_utf16le(text);
    std::size_t length = std::min(units.size(), size - 2) & ~std::size_t{1};
    if (length >= 2 && length < units.size()) {
        const auto last = std::to_integer<std::uint32_t>(units[length - 2]) |
                          (std::to_integer<std::uint32_t>(units[length - 1]) << 8U);
        if (last >= 0xD800 && last <= 0xDBFF) {
            length -= 2;  // the high surrogate's low half was cut off
        }
    }
    w.bytes(std::span(units).first(length));
    w.zeros(size - length);
}

/// ASCII 8 names ([MS-RDPECLIP] 2.2.3.1.1.1) up to the first NUL; bytes
/// above 0x7F are taken as Latin-1.
std::string latin1_to_utf8(std::span<const std::byte> bytes)
{
    std::string out;
    for (const std::byte b : bytes) {
        const auto c = std::to_integer<std::uint8_t>(b);
        if (c == 0) {
            break;
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back(static_cast<char>(0xC0U | (c >> 6U)));
            out.push_back(static_cast<char>(0x80U | (c & 0x3FU)));
        }
    }
    return out;
}

/// CB_RESPONSE_OK or CB_RESPONSE_FAIL, exactly one of them.
Result<bool> response_flag(const Header& header)
{
    const bool ok = (header.flags & msg_flag::response_ok) != 0;
    const bool failed = (header.flags & msg_flag::response_fail) != 0;
    if (ok == failed) {
        return fail(Errc::invalid_value, "clipboard response without exactly one of CB_RESPONSE_OK and FAIL", 2);
    }
    return ok;
}

std::uint16_t response_flags(bool ok)
{
    return ok ? msg_flag::response_ok : msg_flag::response_fail;
}

// --- decoding the bodies

Result<Capabilities> decode_capabilities(Reader& r)
{
    FARLAND_TRY(const auto count, r.u16le());
    FARLAND_TRY_VOID(r.skip(2));  // pad1
    if (count > max_capability_sets) {
        return fail(Errc::limit_exceeded, "too many clipboard capability sets", r.offset() - 4);
    }
    Capabilities caps;
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::size_t start = r.offset();
        FARLAND_TRY(const auto type, r.u16le());
        FARLAND_TRY(const auto length, r.u16le());
        if (length < 4) {
            return fail(Errc::invalid_length, "clipboard capability set shorter than its header", start + 2);
        }
        FARLAND_TRY(auto set, r.sub(length - 4U));
        if (type != capstype_general) {
            continue;  // no other type is defined; skip it
        }
        if (caps.general_flags) {
            return fail(Errc::invalid_value, "second clipboard general capability set", start);
        }
        if (length < general_capability_size) {
            return fail(Errc::invalid_length, "clipboard general capability set too short", start + 2);
        }
        FARLAND_TRY(caps.version, set.u32le());
        FARLAND_TRY(caps.general_flags, set.u32le());
    }
    FARLAND_TRY_VOID(r.expect_end("clipboard capabilities"));
    return caps;
}

Result<FormatList> decode_long_names(Reader& r)
{
    FormatList list;
    while (!r.empty()) {
        if (list.formats.size() == max_formats) {
            return fail(Errc::limit_exceeded, "too many clipboard formats", r.offset());
        }
        Format format;
        FARLAND_TRY(format.id, r.u32le());
        const auto rest = r.rest();
        std::size_t units = 0;
        for (;;) {
            if ((2 * units) + 2 > rest.size()) {
                return fail(Errc::truncated, "clipboard format name without a terminator", r.offset());
            }
            if (rest[2 * units] == std::byte{0} && rest[(2 * units) + 1] == std::byte{0}) {
                break;
            }
            if (++units > max_format_name_length) {
                return fail(Errc::limit_exceeded, "clipboard format name too long", r.offset());
            }
        }
        FARLAND_TRY(const auto name, r.bytes(2 * units));
        FARLAND_TRY_VOID(r.skip(2));
        format.name = utf16le_to_utf8(name);
        list.formats.push_back(std::move(format));
    }
    return list;
}

Result<FormatList> decode_short_names(Reader& r, bool ascii)
{
    if (r.remaining() % short_format_size != 0) {
        return fail(Errc::invalid_length, "short clipboard format names do not fill the format list", r.offset());
    }
    if (r.remaining() / short_format_size > max_formats) {
        return fail(Errc::limit_exceeded, "too many clipboard formats", r.offset());
    }
    FormatList list;
    while (!r.empty()) {
        Format format;
        FARLAND_TRY(format.id, r.u32le());
        FARLAND_TRY(const auto name, r.bytes(short_name_size));
        format.name = ascii ? latin1_to_utf8(name) : utf16le_to_utf8(name);
        list.formats.push_back(std::move(format));
    }
    return list;
}

Result<FileContentsRequest> decode_file_contents_request(Reader& r)
{
    const std::size_t start = r.offset();
    if (r.remaining() != 24 && r.remaining() != 28) {
        return fail(Errc::invalid_length, "file contents request of the wrong size", start);
    }
    FileContentsRequest request;
    FARLAND_TRY(request.stream_id, r.u32le());
    FARLAND_TRY(const auto index, r.u32le());
    request.index = static_cast<std::int32_t>(index);
    FARLAND_TRY(request.flags, r.u32le());
    FARLAND_TRY(const auto low, r.u32le());
    FARLAND_TRY(const auto high, r.u32le());
    request.position = (std::uint64_t{high} << 32U) | low;
    FARLAND_TRY(request.requested, r.u32le());
    if (!r.empty()) {
        FARLAND_TRY(request.clip_data_id, r.u32le());
    }
    if (request.flags != file_contents::size && request.flags != file_contents::range) {
        return fail(Errc::invalid_value, "file contents request must ask for either the size or a range", start + 8);
    }
    if (request.flags == file_contents::size && (request.requested != 8 || request.position != 0)) {
        return fail(Errc::invalid_value, "file size request must ask for 8 bytes at position 0", start + 12);
    }
    return request;
}

Result<Pdu> decode_body(const Header& header, Reader& body, bool long_names)
{
    const auto four_bytes = [&body](std::string_view what) -> Result<std::uint32_t> {
        if (body.remaining() != 4) {
            return fail(Errc::invalid_length, what, body.offset());
        }
        return body.u32le();
    };
    switch (header.type) {
    case MsgType::monitor_ready:
        FARLAND_TRY_VOID(body.expect_end("monitor ready"));
        return MonitorReady{};
    case MsgType::clip_caps: {
        FARLAND_TRY(auto caps, decode_capabilities(body));
        return caps;
    }
    case MsgType::temp_directory: {
        if (body.remaining() != path_field_size) {
            return fail(Errc::invalid_length, "temporary directory PDU of the wrong size", body.offset());
        }
        FARLAND_TRY(const auto path, body.bytes(path_field_size));
        return TempDirectory{utf16le_to_utf8(path)};
    }
    case MsgType::format_list: {
        FARLAND_TRY(auto list, long_names ? decode_long_names(body)
                                          : decode_short_names(body, (header.flags & msg_flag::ascii_names) != 0));
        return list;
    }
    case MsgType::format_list_response: {
        FARLAND_TRY(const bool ok, response_flag(header));
        FARLAND_TRY_VOID(body.expect_end("format list response"));
        return FormatListResponse{ok};
    }
    case MsgType::lock_clipdata: {
        FARLAND_TRY(const auto id, four_bytes("lock clipboard data PDU of the wrong size"));
        return LockClipData{id};
    }
    case MsgType::unlock_clipdata: {
        FARLAND_TRY(const auto id, four_bytes("unlock clipboard data PDU of the wrong size"));
        return UnlockClipData{id};
    }
    case MsgType::format_data_request: {
        FARLAND_TRY(const auto id, four_bytes("format data request of the wrong size"));
        return FormatDataRequest{id};
    }
    case MsgType::format_data_response: {
        FARLAND_TRY(const bool ok, response_flag(header));
        FormatDataResponse response{ok, {}};
        if (ok) {
            const auto data = body.rest();
            response.data.assign(data.begin(), data.end());
        }
        return response;
    }
    case MsgType::file_contents_request: {
        FARLAND_TRY(auto request, decode_file_contents_request(body));
        return request;
    }
    case MsgType::file_contents_response: {
        FARLAND_TRY(const bool ok, response_flag(header));
        FileContentsResponse response{ok, 0, {}};
        if (!ok && body.empty()) {
            return response;
        }
        FARLAND_TRY(response.stream_id, body.u32le());
        if (ok) {
            const auto data = body.rest();
            response.data.assign(data.begin(), data.end());
        }
        return response;
    }
    }
    return fail(Errc::unsupported, "unknown clipboard PDU type", 0);
}

// --- encoding the bodies

struct BodyEncoder {
    bool long_names = true;
    Writer w;
    std::uint16_t flags = 0;

    void operator()(const Capabilities& caps)
    {
        w.u16le(caps.general_flags ? 1 : 0);
        w.u16le(0);
        if (caps.general_flags) {
            w.u16le(capstype_general);
            w.u16le(general_capability_size);
            w.u32le(caps.version.value_or(caps_version_2));
            w.u32le(*caps.general_flags);
        }
    }
    void operator()(const MonitorReady& /*pdu*/) const {}
    void operator()(const TempDirectory& pdu) { write_fixed_utf16(w, pdu.path, path_field_size); }
    void operator()(const FormatList& pdu)
    {
        for (const auto& format : pdu.formats) {
            w.u32le(format.id);
            if (long_names) {
                w.bytes(utf8_to_utf16le(format.name));
                w.u16le(0);
            } else {
                write_fixed_utf16(w, format.name, short_name_size);
            }
        }
    }
    void operator()(const FormatListResponse& pdu) { flags = response_flags(pdu.ok); }
    void operator()(const LockClipData& pdu) { w.u32le(pdu.clip_data_id); }
    void operator()(const UnlockClipData& pdu) { w.u32le(pdu.clip_data_id); }
    void operator()(const FormatDataRequest& pdu) { w.u32le(pdu.format_id); }
    void operator()(const FormatDataResponse& pdu)
    {
        flags = response_flags(pdu.ok);
        if (pdu.ok) {
            w.bytes(pdu.data);
        }
    }
    void operator()(const FileContentsRequest& pdu)
    {
        w.u32le(pdu.stream_id);
        w.u32le(static_cast<std::uint32_t>(pdu.index));
        w.u32le(pdu.flags);
        w.u32le(static_cast<std::uint32_t>(pdu.position));
        w.u32le(static_cast<std::uint32_t>(pdu.position >> 32U));
        w.u32le(pdu.requested);
        if (pdu.clip_data_id) {
            w.u32le(*pdu.clip_data_id);
        }
    }
    void operator()(const FileContentsResponse& pdu)
    {
        flags = response_flags(pdu.ok);
        w.u32le(pdu.stream_id);
        if (pdu.ok) {
            w.bytes(pdu.data);
        }
    }
};

MsgType type_of(const Pdu& pdu)
{
    return std::visit(
        [](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, Capabilities>) {
                return MsgType::clip_caps;
            } else if constexpr (std::is_same_v<T, MonitorReady>) {
                return MsgType::monitor_ready;
            } else if constexpr (std::is_same_v<T, TempDirectory>) {
                return MsgType::temp_directory;
            } else if constexpr (std::is_same_v<T, FormatList>) {
                return MsgType::format_list;
            } else if constexpr (std::is_same_v<T, FormatListResponse>) {
                return MsgType::format_list_response;
            } else if constexpr (std::is_same_v<T, LockClipData>) {
                return MsgType::lock_clipdata;
            } else if constexpr (std::is_same_v<T, UnlockClipData>) {
                return MsgType::unlock_clipdata;
            } else if constexpr (std::is_same_v<T, FormatDataRequest>) {
                return MsgType::format_data_request;
            } else if constexpr (std::is_same_v<T, FormatDataResponse>) {
                return MsgType::format_data_response;
            } else if constexpr (std::is_same_v<T, FileContentsRequest>) {
                return MsgType::file_contents_request;
            } else {
                static_assert(std::is_same_v<T, FileContentsResponse>);
                return MsgType::file_contents_response;
            }
        },
        pdu);
}

}  // namespace

void encode(Writer& w, const Header& header)
{
    w.u16le(static_cast<std::uint16_t>(header.type));
    w.u16le(header.flags);
    w.u32le(header.data_length);
}

Result<Header> decode_header(Reader& r)
{
    Header header;
    FARLAND_TRY(const auto type, r.u16le());
    header.type = static_cast<MsgType>(type);
    FARLAND_TRY(header.flags, r.u16le());
    FARLAND_TRY(header.data_length, r.u32le());
    return header;
}

std::vector<std::byte> encode(const Pdu& pdu, bool long_names)
{
    BodyEncoder body{long_names, {}, 0};
    encode(body.w, Header{type_of(pdu), 0, 0});
    std::visit(body, pdu);
    auto& w = body.w;
    FARLAND_ASSERT(w.size() - header_size <= std::numeric_limits<std::uint32_t>::max());
    w.patch_u16le(2, body.flags);
    w.patch_u32le(4, static_cast<std::uint32_t>(w.size() - header_size));
    return std::move(w).take();
}

// [MS-RDPECLIP] 3.1.5.1
Result<Pdu> decode(std::span<const std::byte> message, bool long_names)
{
    Reader r(message);
    FARLAND_TRY(const auto header, decode_header(r));
    // Windows Server 2003 to 2012 R2 append 4 bytes that dataLen leaves out (note 1).
    if (header.data_length != r.remaining() && (r.remaining() < 4 || header.data_length != r.remaining() - 4)) {
        return fail(Errc::invalid_length, "clipboard PDU length disagrees with the channel message", 4);
    }
    FARLAND_TRY(auto body, r.sub(header.data_length));
    return decode_body(header, body, long_names);
}

std::vector<std::byte> encode_file_list(std::span<const FileDescriptor> files)
{
    FARLAND_ASSERT(files.size() <= std::numeric_limits<std::uint32_t>::max());
    Writer w(4 + (files.size() * file_descriptor_size));
    w.u32le(static_cast<std::uint32_t>(files.size()));
    for (const auto& file : files) {
        w.u32le(file.flags);
        w.zeros(32);  // reserved1
        w.u32le(file.attributes);
        w.zeros(16);  // reserved2
        w.u64le(file.last_write_time);
        w.u32le(static_cast<std::uint32_t>(file.size >> 32U));
        w.u32le(static_cast<std::uint32_t>(file.size));
        write_fixed_utf16(w, file.name, path_field_size);
    }
    return std::move(w).take();
}

// [MS-RDPECLIP] 2.2.5.2.3
Result<std::vector<FileDescriptor>> decode_file_list(std::span<const std::byte> data)
{
    Reader r(data);
    FARLAND_TRY(const auto count, r.u32le());
    if (count > max_file_list_items) {
        return fail(Errc::limit_exceeded, "too many files in the clipboard file list", 0);
    }
    if (r.remaining() != std::size_t{count} * file_descriptor_size) {
        return fail(Errc::invalid_length, "clipboard file list size disagrees with cItems", 0);
    }
    std::vector<FileDescriptor> files;
    files.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        FileDescriptor file;
        FARLAND_TRY(file.flags, r.u32le());
        FARLAND_TRY_VOID(r.skip(32));
        FARLAND_TRY(file.attributes, r.u32le());
        FARLAND_TRY_VOID(r.skip(16));
        FARLAND_TRY(file.last_write_time, r.u64le());
        FARLAND_TRY(const auto high, r.u32le());
        FARLAND_TRY(const auto low, r.u32le());
        file.size = (std::uint64_t{high} << 32U) | low;
        FARLAND_TRY(const auto name, r.bytes(path_field_size));
        file.name = utf16le_to_utf8(name);
        files.push_back(std::move(file));
    }
    return files;
}

}  // namespace farland::channels::cliprdr
