// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/proto/fastpath.hpp>

#include <algorithm>

namespace farland::proto::fastpath {

namespace {

constexpr std::uint8_t action_fastpath = 0;
constexpr std::uint8_t flag_secure_checksum = 0x1;
constexpr std::uint8_t flag_encrypted = 0x2;
constexpr std::uint8_t compression_used = 0x2;
constexpr std::uint8_t packet_compressed = 0x20;
constexpr std::size_t pdu_overhead = 3 + 3;  // header + 2-byte length, updateHeader + size

void write_pdu(Writer& w, std::uint8_t code, Fragmentation fragmentation, std::span<const std::byte> chunk)
{
    const std::size_t length = pdu_overhead + chunk.size();
    FARLAND_ASSERT(length <= 0x7FFF);
    w.u8(action_fastpath);
    // Always the two-octet length form, like FreeRDP and Windows.
    w.u16be(static_cast<std::uint16_t>(0x8000U | length));
    w.u8(static_cast<std::uint8_t>(code | (static_cast<unsigned>(fragmentation) << 4U)));
    w.u16le(static_cast<std::uint16_t>(chunk.size()));
    w.bytes(chunk);
}

}  // namespace

void encode_update(Writer& w, std::uint8_t code, std::span<const std::byte> data, std::size_t max_fragment)
{
    FARLAND_ASSERT(code <= 0x0F && max_fragment > 0 && max_fragment <= 0x7FFF - pdu_overhead);
    if (data.size() <= max_fragment) {
        write_pdu(w, code, Fragmentation::single, data);
        return;
    }
    for (std::size_t offset = 0; offset < data.size(); offset += max_fragment) {
        const std::size_t size = std::min(max_fragment, data.size() - offset);
        const bool first = offset == 0;
        const bool last = offset + size == data.size();
        auto fragmentation = Fragmentation::next;
        if (first) {
            fragmentation = Fragmentation::first;
        } else if (last) {
            fragmentation = Fragmentation::last;
        }
        write_pdu(w, code, fragmentation, data.subspan(offset, size));
    }
}

Result<std::vector<UpdateFragment>> decode_output_pdu(Reader& pdu)
{
    const std::size_t start = pdu.offset();
    FARLAND_TRY(const std::uint8_t header, pdu.u8());
    if ((header & 0x03U) != action_fastpath) {
        return fail(Errc::invalid_value, "not a fast-path PDU", start);
    }
    if (((header >> 6U) & (flag_encrypted | flag_secure_checksum)) != 0) {
        return fail(Errc::invalid_value, "RDP-encrypted fast-path output under Enhanced RDP Security", start);
    }
    FARLAND_TRY(const std::uint8_t length1, pdu.u8());
    std::size_t length = length1;
    if ((length1 & 0x80U) != 0) {
        FARLAND_TRY(const std::uint8_t length2, pdu.u8());
        length = (static_cast<std::size_t>(length1 & 0x7FU) << 8U) | length2;
    }
    if (length != pdu.size()) {
        return fail(Errc::invalid_length, "fast-path length does not match the PDU", start + 1);
    }
    std::vector<UpdateFragment> updates;
    while (!pdu.empty()) {
        const std::size_t update_start = pdu.offset();
        FARLAND_TRY(const std::uint8_t update_header, pdu.u8());
        UpdateFragment fragment;
        fragment.code = update_header & 0x0FU;
        fragment.fragmentation = static_cast<Fragmentation>((update_header >> 4U) & 0x03U);
        if (((update_header >> 6U) & compression_used) != 0) {
            FARLAND_TRY(const std::uint8_t compression_flags, pdu.u8());
            if ((compression_flags & packet_compressed) != 0) {
                return fail(Errc::unsupported, "bulk-compressed fast-path update", update_start);
            }
        }
        FARLAND_TRY(const std::uint16_t size, pdu.u16le());
        FARLAND_TRY(fragment.data, pdu.bytes(size));
        updates.push_back(fragment);
    }
    return updates;
}

Result<std::optional<Reassembler::Update>> Reassembler::add(const UpdateFragment& fragment)
{
    const auto too_large = [this](std::size_t extra) { return buffer_.size() + extra > max_size_; };
    switch (fragment.fragmentation) {
    case Fragmentation::single:
        if (code_ || fragment.data.size() > max_size_) {
            return fail(Errc::invalid_value, "unexpected single fast-path update", 0);
        }
        return Update{fragment.code, {fragment.data.begin(), fragment.data.end()}};
    case Fragmentation::first:
        if (code_ || fragment.data.size() > max_size_) {
            return fail(Errc::invalid_value, "unexpected first fast-path fragment", 0);
        }
        code_ = fragment.code;
        buffer_.assign(fragment.data.begin(), fragment.data.end());
        return std::nullopt;
    case Fragmentation::next:
    case Fragmentation::last:
        if (!code_ || *code_ != fragment.code) {
            return fail(Errc::invalid_value, "fast-path fragment without a first fragment", 0);
        }
        if (too_large(fragment.data.size())) {
            return fail(Errc::limit_exceeded, "reassembled fast-path update too large", 0);
        }
        buffer_.insert(buffer_.end(), fragment.data.begin(), fragment.data.end());
        if (fragment.fragmentation == Fragmentation::next) {
            return std::nullopt;
        }
        Update update{*code_, std::move(buffer_)};
        code_.reset();
        buffer_.clear();
        return update;
    }
    return fail(Errc::invalid_value, "bad fast-path fragmentation", 0);
}

}  // namespace farland::proto::fastpath
