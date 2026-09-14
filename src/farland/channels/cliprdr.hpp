// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

/// The Clipboard Virtual Channel Extension ([MS-RDPECLIP] 2.2): the PDUs of
/// the "cliprdr" static virtual channel and the Packed File List. One
/// reassembled channel message holds exactly one PDU.
namespace farland::channels::cliprdr {

/// The static virtual channel's name ([MS-RDPECLIP] 2.1).
inline constexpr const char* channel_name = "cliprdr";
/// Size of CLIPRDR_HEADER.
inline constexpr std::size_t header_size = 8;

/// CLIPRDR_HEADER msgType, [MS-RDPECLIP] 2.2.1.
enum class MsgType : std::uint16_t {
    monitor_ready = 0x0001,
    format_list = 0x0002,
    format_list_response = 0x0003,
    format_data_request = 0x0004,
    format_data_response = 0x0005,
    temp_directory = 0x0006,
    clip_caps = 0x0007,
    file_contents_request = 0x0008,
    file_contents_response = 0x0009,
    lock_clipdata = 0x000A,
    unlock_clipdata = 0x000B,
};

/// CLIPRDR_HEADER msgFlags, [MS-RDPECLIP] 2.2.1.
namespace msg_flag {
inline constexpr std::uint16_t response_ok = 0x0001;
inline constexpr std::uint16_t response_fail = 0x0002;
inline constexpr std::uint16_t ascii_names = 0x0004;
}  // namespace msg_flag

/// CLIPRDR_GENERAL_CAPABILITY generalFlags, [MS-RDPECLIP] 2.2.2.1.1.1.
namespace general_flag {
inline constexpr std::uint32_t use_long_format_names = 0x00000002;
inline constexpr std::uint32_t stream_fileclip_enabled = 0x00000004;
inline constexpr std::uint32_t fileclip_no_file_paths = 0x00000008;
inline constexpr std::uint32_t can_lock_clipdata = 0x00000010;
inline constexpr std::uint32_t huge_file_support_enabled = 0x00000020;
}  // namespace general_flag

inline constexpr std::uint16_t capstype_general = 0x0001;
inline constexpr std::uint32_t caps_version_1 = 1;
inline constexpr std::uint32_t caps_version_2 = 2;

/// Standard Windows clipboard format IDs (winuser.h) that farland maps.
namespace cf {
inline constexpr std::uint32_t text = 1;
inline constexpr std::uint32_t bitmap = 2;
inline constexpr std::uint32_t metafile_pict = 3;  ///< [MS-RDPECLIP] 1.3.1.2
inline constexpr std::uint32_t oem_text = 7;
inline constexpr std::uint32_t dib = 8;
inline constexpr std::uint32_t palette = 9;  ///< [MS-RDPECLIP] 1.3.1.2
inline constexpr std::uint32_t unicode_text = 13;
inline constexpr std::uint32_t hdrop = 15;
inline constexpr std::uint32_t locale = 16;
inline constexpr std::uint32_t dibv5 = 17;
}  // namespace cf

/// Registered format names farland maps (by name, their IDs differ per system).
inline constexpr const char* file_group_descriptor_w = "FileGroupDescriptorW";  ///< [MS-RDPECLIP] 1.3.1.2
inline constexpr const char* html_format = "HTML Format";
inline constexpr const char* png_format = "PNG";

/// farland limits on what a peer may send.
inline constexpr std::size_t max_capability_sets = 16;
inline constexpr std::size_t max_formats = 256;
/// Long format names: characters per name, without the terminator.
inline constexpr std::size_t max_format_name_length = 256;
inline constexpr std::size_t max_file_list_items = 16384;

/// CLIPRDR_HEADER, [MS-RDPECLIP] 2.2.1.
struct Header {
    MsgType type{};
    std::uint16_t flags = 0;
    std::uint32_t data_length = 0;

    friend bool operator==(const Header&, const Header&) = default;
};

/// CLIPRDR_CAPS with its only defined set, CLIPRDR_GENERAL_CAPABILITY
/// ([MS-RDPECLIP] 2.2.2.1, 2.2.2.1.1.1). Sets of other types are skipped on
/// decoding; without a general set the defaults (no flags) apply.
struct Capabilities {
    std::optional<std::uint32_t> version;        ///< informational only
    std::optional<std::uint32_t> general_flags;  ///< general_flag bits

    friend bool operator==(const Capabilities&, const Capabilities&) = default;
};

/// CLIPRDR_MONITOR_READY, [MS-RDPECLIP] 2.2.2.2.
struct MonitorReady {
    friend bool operator==(const MonitorReady&, const MonitorReady&) = default;
};

/// CLIPRDR_TEMP_DIRECTORY, [MS-RDPECLIP] 2.2.2.3. farland never uses the
/// path (it has no direct access to the client's files, 3.1.1.3).
struct TempDirectory {
    std::string path;  ///< UTF-8

    friend bool operator==(const TempDirectory&, const TempDirectory&) = default;
};

/// One Clipboard Format ID and name pair ([MS-RDPECLIP] 2.2.3.1.1.1, 2.2.3.1.2.1).
struct Format {
    std::uint32_t id = 0;
    std::string name;  ///< UTF-8; empty for formats without a name

    friend bool operator==(const Format&, const Format&) = default;
};

/// CLIPRDR_FORMAT_LIST, [MS-RDPECLIP] 2.2.3.1. Whether it travels with long
/// or short names depends on the capabilities of both sides, so encoding and
/// decoding take that as a parameter. Short names are 32 bytes: 15 UTF-16
/// characters, or 31 ASCII ones with CB_ASCII_NAMES; longer names are
/// truncated on encoding (2.2.3.1.1.1).
struct FormatList {
    std::vector<Format> formats;

    friend bool operator==(const FormatList&, const FormatList&) = default;
};

/// FORMAT_LIST_RESPONSE, [MS-RDPECLIP] 2.2.3.2.
struct FormatListResponse {
    bool ok = true;

    friend bool operator==(const FormatListResponse&, const FormatListResponse&) = default;
};

/// CLIPRDR_LOCK_CLIPDATA, [MS-RDPECLIP] 2.2.4.1.
struct LockClipData {
    std::uint32_t clip_data_id = 0;

    friend bool operator==(const LockClipData&, const LockClipData&) = default;
};

/// CLIPRDR_UNLOCK_CLIPDATA, [MS-RDPECLIP] 2.2.4.2.
struct UnlockClipData {
    std::uint32_t clip_data_id = 0;

    friend bool operator==(const UnlockClipData&, const UnlockClipData&) = default;
};

/// CLIPRDR_FORMAT_DATA_REQUEST, [MS-RDPECLIP] 2.2.5.1.
struct FormatDataRequest {
    std::uint32_t format_id = 0;

    friend bool operator==(const FormatDataRequest&, const FormatDataRequest&) = default;
};

/// CLIPRDR_FORMAT_DATA_RESPONSE, [MS-RDPECLIP] 2.2.5.2. A failed response
/// carries no data.
struct FormatDataResponse {
    bool ok = true;
    std::vector<std::byte> data;

    friend bool operator==(const FormatDataResponse&, const FormatDataResponse&) = default;
};

/// dwFlags of CLIPRDR_FILECONTENTS_REQUEST, [MS-RDPECLIP] 2.2.5.3.
namespace file_contents {
inline constexpr std::uint32_t size = 0x00000001;
inline constexpr std::uint32_t range = 0x00000002;
}  // namespace file_contents

/// CLIPRDR_FILECONTENTS_REQUEST, [MS-RDPECLIP] 2.2.5.3. Exactly one of
/// FILECONTENTS_SIZE and FILECONTENTS_RANGE is set; a size request asks for
/// 8 bytes at position 0.
struct FileContentsRequest {
    std::uint32_t stream_id = 0;
    std::int32_t index = 0;  ///< lindex: the file's position in the file list
    std::uint32_t flags = file_contents::range;
    std::uint64_t position = 0;   ///< nPositionHigh:nPositionLow
    std::uint32_t requested = 0;  ///< cbRequested
    std::optional<std::uint32_t> clip_data_id;

    friend bool operator==(const FileContentsRequest&, const FileContentsRequest&) = default;
};

/// CLIPRDR_FILECONTENTS_RESPONSE, [MS-RDPECLIP] 2.2.5.4. The data of a size
/// response is the 64-bit little-endian size. Windows sends failed responses
/// with the streamId only, and some clients with no body at all; the latter
/// decode with stream_id 0.
struct FileContentsResponse {
    bool ok = true;
    std::uint32_t stream_id = 0;
    std::vector<std::byte> data;

    friend bool operator==(const FileContentsResponse&, const FileContentsResponse&) = default;
};

using Pdu =
    std::variant<Capabilities, MonitorReady, TempDirectory, FormatList, FormatListResponse, LockClipData,
                 UnlockClipData, FormatDataRequest, FormatDataResponse, FileContentsRequest, FileContentsResponse>;

void encode(Writer& w, const Header& header);
[[nodiscard]] Result<Header> decode_header(Reader& r);

/// Encodes one PDU. `long_names` picks the Format List variant: long names
/// when both sides set CB_USE_LONG_FORMAT_NAMES, short (Unicode) names
/// otherwise.
[[nodiscard]] std::vector<std::byte> encode(const Pdu& pdu, bool long_names = true);

/// Decodes one reassembled channel message. The header's dataLen must match
/// the message, except that 4 trailing bytes are allowed (and ignored): some
/// Windows versions append them ([MS-RDPECLIP] 2.2.1, note 1). `long_names`
/// as for encode(). msgType values the specification does not define are
/// Errc::unsupported; callers may ignore those PDUs (3.1.5.1).
[[nodiscard]] Result<Pdu> decode(std::span<const std::byte> message, bool long_names = true);

/// FD_* flags of CLIPRDR_FILEDESCRIPTOR, [MS-RDPECLIP] 2.2.5.2.3.1.
namespace fd_flag {
inline constexpr std::uint32_t attributes = 0x00000004;
inline constexpr std::uint32_t write_time = 0x00000020;
inline constexpr std::uint32_t file_size = 0x00000040;
inline constexpr std::uint32_t show_progress_ui = 0x00004000;
}  // namespace fd_flag

/// FILE_ATTRIBUTE_* values of CLIPRDR_FILEDESCRIPTOR fileAttributes.
namespace file_attribute {
inline constexpr std::uint32_t readonly = 0x00000001;
inline constexpr std::uint32_t hidden = 0x00000002;
inline constexpr std::uint32_t system = 0x00000004;
inline constexpr std::uint32_t directory = 0x00000010;
inline constexpr std::uint32_t archive = 0x00000020;
inline constexpr std::uint32_t normal = 0x00000080;
}  // namespace file_attribute

/// CLIPRDR_FILEDESCRIPTOR, [MS-RDPECLIP] 2.2.5.2.3.1.
struct FileDescriptor {
    std::uint32_t flags = 0;  ///< fd_flag bits
    std::uint32_t attributes = 0;
    std::uint64_t last_write_time = 0;  ///< FILETIME: 100 ns intervals since 1601-01-01
    std::uint64_t size = 0;
    /// UTF-8, at most 259 UTF-16 code units on the wire. Components are
    /// separated by backslashes: "dir\file.txt".
    std::string name;

    friend bool operator==(const FileDescriptor&, const FileDescriptor&) = default;
};

inline constexpr std::size_t file_descriptor_size = 592;

/// CLIPRDR_FILELIST, [MS-RDPECLIP] 2.2.5.2.3: the data of FileGroupDescriptorW.
[[nodiscard]] std::vector<std::byte> encode_file_list(std::span<const FileDescriptor> files);
/// At most max_file_list_items entries, and the list must fill `data`.
[[nodiscard]] Result<std::vector<FileDescriptor>> decode_file_list(std::span<const std::byte> data);

}  // namespace farland::channels::cliprdr
