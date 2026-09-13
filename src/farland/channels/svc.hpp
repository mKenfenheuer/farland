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
#include <vector>

/// Static virtual channel framing: the Channel PDU Header and the chunking of
/// one channel message into Virtual Channel PDUs, [MS-RDPBCGR] 2.2.6.1 and
/// 3.1.5.2. Each chunk is the userData of one MCS Send Data Request
/// (client to server) or Send Data Indication (server to client); the MCS
/// layer and the channel ID are the caller's business.
namespace farland::channels::svc {

/// CHANNEL_CHUNK_LENGTH, the chunk size when no VCChunkSize was negotiated
/// ([MS-RDPBCGR] 2.2.6.1).
inline constexpr std::size_t chunk_length = 1600;
/// Largest VCChunkSize the specification allows ([MS-RDPBCGR] 2.2.7.1.10).
inline constexpr std::size_t max_chunk_length = 16256;
/// Size of CHANNEL_PDU_HEADER.
inline constexpr std::size_t header_size = 8;

/// CHANNEL_PDU_HEADER flags, [MS-RDPBCGR] 2.2.6.1.1.
namespace flag {
inline constexpr std::uint32_t first = 0x00000001;
inline constexpr std::uint32_t last = 0x00000002;
inline constexpr std::uint32_t show_protocol = 0x00000010;
inline constexpr std::uint32_t suspend = 0x00000020;            ///< Server to client only; ignored from the client.
inline constexpr std::uint32_t resume = 0x00000040;             ///< Server to client only; ignored from the client.
inline constexpr std::uint32_t shadow_persistent = 0x00000080;  ///< Unused, ignored.
inline constexpr std::uint32_t compression_type_mask = 0x000F0000;
inline constexpr std::uint32_t packet_compressed = 0x00200000;
inline constexpr std::uint32_t packet_at_front = 0x00400000;
inline constexpr std::uint32_t packet_flushed = 0x00800000;
}  // namespace flag

/// CHANNEL_PDU_HEADER, [MS-RDPBCGR] 2.2.6.1.1. `length` is the total length
/// of the uncompressed message, repeated in every chunk.
struct ChunkHeader {
    std::uint32_t length = 0;
    std::uint32_t flags = 0;

    friend bool operator==(const ChunkHeader&, const ChunkHeader&) = default;
};

void encode(Writer& w, const ChunkHeader& header);
[[nodiscard]] Result<ChunkHeader> decode_header(Reader& r);

/// The chunk size to use for sending, from the VCChunkSize fields of both
/// Virtual Channel Capability Sets ([MS-RDPBCGR] 3.1.5.2.1): the server's
/// value applies only when the client sent the field too, otherwise
/// CHANNEL_CHUNK_LENGTH. The client's value itself is ignored (2.2.7.1.10).
/// The server's value is clamped to [chunk_length, max_chunk_length].
[[nodiscard]] std::size_t negotiated_chunk_length(std::optional<std::uint32_t> server_chunk_size,
                                                  std::optional<std::uint32_t> client_chunk_size) noexcept;

/// Splits one channel message into Virtual Channel PDU payloads (header plus
/// at most `max_chunk` bytes of data each), [MS-RDPBCGR] 3.1.5.2.1. A message
/// that fits one chunk carries FIRST|LAST; a chunked one carries FIRST on the
/// first chunk, LAST on the last, and SHOW_PROTOCOL on every chunk, as the
/// specification requires for chunked data. An empty message becomes one
/// empty FIRST|LAST chunk. The message must fit the 32-bit length field.
[[nodiscard]] std::vector<std::vector<std::byte>> encode_chunks(std::span<const std::byte> message,
                                                                std::size_t max_chunk = chunk_length);

/// Reassembles the chunks of one static virtual channel (one instance per MCS
/// channel), [MS-RDPBCGR] 3.1.5.2.2.1. Stricter than the specification
/// requires of receivers:
/// - compressed chunks are rejected (farland never negotiates channel MPPC,
///   VCCAPS_COMPR_CS_8K);
/// - a chunk without FIRST outside a sequence, a FIRST inside one, a length
///   that changes between chunks, or data that overruns or falls short of
///   the announced length is an error;
/// - lengths above `max_message_size` and chunks above `max_chunk` are errors.
/// SUSPEND, RESUME, SHADOW_PERSISTENT and SHOW_PROTOCOL are ignored. After an
/// error the partial message is dropped.
class Reassembler {
public:
    explicit Reassembler(std::size_t max_message_size, std::size_t max_chunk = max_chunk_length)
        : max_message_size_(max_message_size), max_chunk_(max_chunk)
    {
    }

    /// `pdu` is the CHANNEL_PDU_HEADER followed by the chunk data. Returns the
    /// complete message once its last chunk arrives.
    [[nodiscard]] Result<std::optional<std::vector<std::byte>>> add(std::span<const std::byte> pdu);

    /// True while a chunked message is incomplete.
    [[nodiscard]] bool in_progress() const noexcept { return total_.has_value(); }

private:
    [[nodiscard]] Result<std::optional<std::vector<std::byte>>> process(std::span<const std::byte> pdu);

    std::size_t max_message_size_;
    std::size_t max_chunk_;
    std::optional<std::uint32_t> total_;
    std::vector<std::byte> buffer_;
};

}  // namespace farland::channels::svc
