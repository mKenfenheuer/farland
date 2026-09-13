// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/channels/svc.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace farland::channels::svc {

void encode(Writer& w, const ChunkHeader& header)
{
    w.u32le(header.length);
    w.u32le(header.flags);
}

Result<ChunkHeader> decode_header(Reader& r)
{
    ChunkHeader header;
    FARLAND_TRY(header.length, r.u32le());
    FARLAND_TRY(header.flags, r.u32le());
    return header;
}

std::size_t negotiated_chunk_length(std::optional<std::uint32_t> server_chunk_size,
                                    std::optional<std::uint32_t> client_chunk_size) noexcept
{
    if (!server_chunk_size || !client_chunk_size) {
        return chunk_length;
    }
    return std::clamp<std::size_t>(*server_chunk_size, chunk_length, max_chunk_length);
}

std::vector<std::vector<std::byte>> encode_chunks(std::span<const std::byte> message, std::size_t max_chunk)
{
    FARLAND_ASSERT(max_chunk > 0);
    FARLAND_ASSERT(message.size() <= std::numeric_limits<std::uint32_t>::max());
    const auto total = static_cast<std::uint32_t>(message.size());
    // [MS-RDPBCGR] 3.1.5.2.1: chunked data carries CHANNEL_FLAG_SHOW_PROTOCOL.
    const std::uint32_t common_flags = message.size() > max_chunk ? flag::show_protocol : 0;

    std::vector<std::vector<std::byte>> chunks;
    chunks.reserve(std::max<std::size_t>(1, (message.size() + max_chunk - 1) / max_chunk));
    std::size_t offset = 0;
    do {
        const std::size_t size = std::min(max_chunk, message.size() - offset);
        std::uint32_t flags = common_flags;
        if (offset == 0) {
            flags |= flag::first;
        }
        if (offset + size == message.size()) {
            flags |= flag::last;
        }
        Writer w(header_size + size);
        encode(w, ChunkHeader{total, flags});
        w.bytes(message.subspan(offset, size));
        chunks.push_back(std::move(w).take());
        offset += size;
    } while (offset < message.size());
    return chunks;
}

Result<std::optional<std::vector<std::byte>>> Reassembler::add(std::span<const std::byte> pdu)
{
    auto result = process(pdu);
    if (!result.has_value()) {
        total_.reset();
        buffer_.clear();
    }
    return result;
}

// [MS-RDPBCGR] 3.1.5.2.2.1, with the stricter checks described in svc.hpp.
Result<std::optional<std::vector<std::byte>>> Reassembler::process(std::span<const std::byte> pdu)
{
    Reader r(pdu);
    FARLAND_TRY(const auto header, decode_header(r));
    const auto data = r.rest();

    if ((header.flags & flag::packet_compressed) != 0) {
        return fail(Errc::unsupported, "compressed static virtual channel data", 4);
    }
    if (data.size() > max_chunk_) {
        return fail(Errc::limit_exceeded, "static virtual channel chunk above the chunk size", header_size);
    }
    if (header.length > max_message_size_) {
        return fail(Errc::limit_exceeded, "static virtual channel message too large", 0);
    }

    const bool first = (header.flags & flag::first) != 0;
    const bool last = (header.flags & flag::last) != 0;

    if (first) {
        if (total_) {
            return fail(Errc::invalid_value, "first static virtual channel chunk inside a message", 4);
        }
        if (last) {
            if (data.size() != header.length) {
                return fail(Errc::invalid_length, "static virtual channel chunk disagrees with its length", 0);
            }
            return std::vector<std::byte>(data.begin(), data.end());
        }
        if (data.size() >= header.length) {
            return fail(Errc::invalid_length, "first static virtual channel chunk holds the whole message", 0);
        }
        total_ = header.length;
        buffer_.assign(data.begin(), data.end());
        return std::nullopt;
    }

    if (!total_) {
        return fail(Errc::invalid_value, "static virtual channel chunk without a first chunk", 4);
    }
    if (header.length != *total_) {
        return fail(Errc::invalid_length, "static virtual channel length changed between chunks", 0);
    }
    if (data.size() > *total_ - buffer_.size()) {
        return fail(Errc::invalid_length, "static virtual channel chunks overrun the message", header_size);
    }
    buffer_.insert(buffer_.end(), data.begin(), data.end());
    if (!last) {
        return std::nullopt;
    }
    if (buffer_.size() != *total_) {
        return fail(Errc::invalid_length, "last static virtual channel chunk ends early", header_size);
    }
    total_.reset();
    return std::exchange(buffer_, {});
}

}  // namespace farland::channels::svc
