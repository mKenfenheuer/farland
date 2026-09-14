// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/channels/rdpsnd.hpp>

#include <algorithm>
#include <limits>
#include <type_traits>

namespace farland::channels::rdpsnd {

namespace {

/// Writes the RDPSND PDU Header with a BodySize placeholder; returns its position.
std::size_t begin_pdu(Writer& w, std::uint8_t type)
{
    const std::size_t start = w.size();
    w.u8(type);
    w.u8(0);  // bPad
    w.u16le(0);
    return start;
}

/// Patches BodySize: everything after the header ([MS-RDPEA] 2.2.1).
void end_pdu(Writer& w, std::size_t start)
{
    const std::size_t body = w.size() - start - header_size;
    FARLAND_ASSERT(body <= std::numeric_limits<std::uint16_t>::max());
    w.patch_u16le(start + 2, static_cast<std::uint16_t>(body));
}

struct Header {
    std::uint8_t type = 0;
    std::uint16_t body_size = 0;
};

Result<Header> decode_header(Reader& r)
{
    Header h;
    FARLAND_TRY(h.type, r.u8());
    FARLAND_TRY_VOID(r.skip(1));  // bPad
    FARLAND_TRY(h.body_size, r.u16le());
    return h;
}

/// The body of a PDU whose BodySize must match the rest of the message.
Result<Reader> exact_body(Reader& r, const Header& h)
{
    if (h.body_size != r.remaining()) {
        return fail(Errc::invalid_length, "RDPSND BodySize does not match the message", r.offset());
    }
    return r.sub(h.body_size);
}

void encode_formats(Writer& w, const std::vector<AudioFormat>& formats)
{
    for (const AudioFormat& format : formats) {
        encode(w, format);
    }
}

Result<std::vector<AudioFormat>> decode_formats(Reader& r, std::size_t count)
{
    if (count > max_formats) {
        return fail(Errc::limit_exceeded, "too many audio formats", r.offset());
    }
    std::vector<AudioFormat> formats;
    formats.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        FARLAND_TRY(auto format, decode_audio_format(r));
        formats.push_back(std::move(format));
    }
    return formats;
}

}  // namespace

AudioFormat pcm_format(std::uint32_t rate, std::uint16_t channels, std::uint16_t bits)
{
    const auto block_align = static_cast<std::uint16_t>(channels * ((bits + 7U) / 8U));
    return AudioFormat{
        .tag = format_tag::pcm,
        .channels = channels,
        .samples_per_sec = rate,
        .avg_bytes_per_sec = rate * block_align,
        .block_align = block_align,
        .bits_per_sample = bits,
        .extra = {},
    };
}

// [MS-RDPEA] 2.2.2.1.1 Audio Format (AUDIO_FORMAT)
void encode(Writer& w, const AudioFormat& format)
{
    FARLAND_ASSERT(format.extra.size() <= std::numeric_limits<std::uint16_t>::max());
    w.u16le(format.tag);
    w.u16le(format.channels);
    w.u32le(format.samples_per_sec);
    w.u32le(format.avg_bytes_per_sec);
    w.u16le(format.block_align);
    w.u16le(format.bits_per_sample);
    w.u16le(static_cast<std::uint16_t>(format.extra.size()));
    w.bytes(format.extra);
}

Result<AudioFormat> decode_audio_format(Reader& r)
{
    AudioFormat format;
    const std::size_t start = r.offset();
    FARLAND_TRY(format.tag, r.u16le());
    FARLAND_TRY(format.channels, r.u16le());
    FARLAND_TRY(format.samples_per_sec, r.u32le());
    FARLAND_TRY(format.avg_bytes_per_sec, r.u32le());
    FARLAND_TRY(format.block_align, r.u16le());
    FARLAND_TRY(format.bits_per_sample, r.u16le());
    FARLAND_TRY(const auto extra_size, r.u16le());
    if (extra_size > max_format_extra) {
        return fail(Errc::limit_exceeded, "AUDIO_FORMAT cbSize too large", r.offset());
    }
    FARLAND_TRY(const auto extra, r.bytes(extra_size));
    format.extra.assign(extra.begin(), extra.end());
    if (format.channels == 0 || format.block_align == 0) {
        return fail(Errc::invalid_value, "AUDIO_FORMAT without channels or block alignment", start);
    }
    return format;
}

// --- Server to client ----------------------------------------------------------

std::vector<std::byte> encode_server_pdu(const ServerPdu& pdu)
{
    Writer w;
    std::visit(
        [&w](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, ServerFormats>) {
                // [MS-RDPEA] 2.2.2.1 Server Audio Formats and Version PDU
                FARLAND_ASSERT(p.formats.size() <= max_formats);
                const auto start = begin_pdu(w, msg::formats);
                w.u32le(0);  // dwFlags
                w.u32le(0);  // dwVolume
                w.u32le(0);  // dwPitch
                w.u16le(0);  // wDGramPort
                w.u16le(static_cast<std::uint16_t>(p.formats.size()));
                w.u8(p.last_block_confirmed);
                w.u16le(p.version);
                w.u8(0);  // bPad
                encode_formats(w, p.formats);
                end_pdu(w, start);
            } else if constexpr (std::is_same_v<T, Training>) {
                // [MS-RDPEA] 2.2.3.1 Training PDU
                const auto start = begin_pdu(w, msg::training);
                w.u16le(p.timestamp);
                const std::size_t pack_size = p.data_size == 0 ? 0 : header_size + 4 + p.data_size;
                FARLAND_ASSERT(pack_size <= std::numeric_limits<std::uint16_t>::max());
                w.u16le(static_cast<std::uint16_t>(pack_size));
                w.zeros(p.data_size);
                end_pdu(w, start);
            } else if constexpr (std::is_same_v<T, WaveInfo>) {
                constexpr bool wave_info_goes_through_encode_wave = false;
                FARLAND_ASSERT(wave_info_goes_through_encode_wave);
            } else if constexpr (std::is_same_v<T, Wave2>) {
                // [MS-RDPEA] 2.2.3.10 Wave2 PDU
                const auto start = begin_pdu(w, msg::wave2);
                w.u16le(p.timestamp);
                w.u16le(p.format_no);
                w.u8(p.block_no);
                w.zeros(3);  // bPad
                w.u32le(p.audio_timestamp);
                w.bytes(p.data);
                end_pdu(w, start);
            } else if constexpr (std::is_same_v<T, Close>) {
                // [MS-RDPEA] 2.2.3.9 Close PDU
                end_pdu(w, begin_pdu(w, msg::close));
            } else if constexpr (std::is_same_v<T, Volume>) {
                // [MS-RDPEA] 2.2.4.1 Volume PDU
                const auto start = begin_pdu(w, msg::set_volume);
                w.u32le(p.volume);
                end_pdu(w, start);
            } else if constexpr (std::is_same_v<T, Pitch>) {
                // [MS-RDPEA] 2.2.4.2 Pitch PDU
                const auto start = begin_pdu(w, msg::set_pitch);
                w.u32le(p.pitch);
                end_pdu(w, start);
            }
        },
        pdu);
    return std::move(w).take();
}

// [MS-RDPEA] 2.2.3.3 WaveInfo PDU, 2.2.3.4 Wave PDU. BodySize is the sample
// size plus 8 (3.3.5.2.1.1): the WaveInfo body without its four data bytes.
std::array<std::vector<std::byte>, 2> encode_wave(const Wave& wave)
{
    FARLAND_ASSERT(wave.data.size() > 4);
    FARLAND_ASSERT(wave.data.size() + 8 <= std::numeric_limits<std::uint16_t>::max());
    Writer info(header_size + 12);
    info.u8(msg::wave);
    info.u8(0);  // bPad
    info.u16le(static_cast<std::uint16_t>(wave.data.size() + 8));
    info.u16le(wave.timestamp);
    info.u16le(wave.format_no);
    info.u8(wave.block_no);
    info.zeros(3);  // bPad
    info.bytes(wave.data.first(4));
    Writer rest(wave.data.size());
    rest.u32le(0);  // bPad, where the four bytes of WaveInfo belong
    rest.bytes(wave.data.subspan(4));
    return {std::move(info).take(), std::move(rest).take()};
}

Result<ServerPdu> decode_server_pdu(std::span<const std::byte> message)
{
    Reader outer(message);
    FARLAND_TRY(const Header h, decode_header(outer));
    if (h.type == msg::wave) {
        // BodySize describes the WaveInfo and Wave PDUs together.
        WaveInfo info;
        FARLAND_TRY(info.timestamp, outer.u16le());
        FARLAND_TRY(info.format_no, outer.u16le());
        FARLAND_TRY(info.block_no, outer.u8());
        FARLAND_TRY_VOID(outer.skip(3));
        FARLAND_TRY(const auto first, outer.bytes(4));
        std::ranges::copy(first, info.first_bytes.begin());
        FARLAND_TRY_VOID(outer.expect_end("WaveInfo PDU"));
        if (h.body_size < 8 + 4) {
            return fail(Errc::invalid_length, "WaveInfo BodySize too small", 2);
        }
        info.sample_size = static_cast<std::uint16_t>(h.body_size - 8);
        return info;
    }
    FARLAND_TRY(auto r, exact_body(outer, h));
    switch (h.type) {
    case msg::formats: {
        ServerFormats p;
        FARLAND_TRY_VOID(r.skip(4 + 4 + 4 + 2));  // dwFlags, dwVolume, dwPitch, wDGramPort
        FARLAND_TRY(const auto count, r.u16le());
        FARLAND_TRY(p.last_block_confirmed, r.u8());
        FARLAND_TRY(p.version, r.u16le());
        FARLAND_TRY_VOID(r.skip(1));
        FARLAND_TRY(p.formats, decode_formats(r, count));
        FARLAND_TRY_VOID(r.expect_end("Server Audio Formats and Version PDU"));
        return p;
    }
    case msg::training: {
        Training p;
        FARLAND_TRY(p.timestamp, r.u16le());
        FARLAND_TRY(const auto pack_size, r.u16le());
        p.data_size = static_cast<std::uint16_t>(r.remaining());
        const std::size_t expected = p.data_size == 0 ? 0 : header_size + 4 + p.data_size;
        if (pack_size != expected) {
            return fail(Errc::invalid_length, "Training wPackSize does not match the PDU", 6);
        }
        return p;
    }
    case msg::wave2: {
        Wave2 p;
        FARLAND_TRY(p.timestamp, r.u16le());
        FARLAND_TRY(p.format_no, r.u16le());
        FARLAND_TRY(p.block_no, r.u8());
        FARLAND_TRY_VOID(r.skip(3));
        FARLAND_TRY(p.audio_timestamp, r.u32le());
        const auto data = r.rest();
        p.data.assign(data.begin(), data.end());
        return p;
    }
    case msg::close:
        FARLAND_TRY_VOID(r.expect_end("Close PDU"));
        return Close{};
    case msg::set_volume: {
        Volume p;
        FARLAND_TRY(p.volume, r.u32le());
        FARLAND_TRY_VOID(r.expect_end("Volume PDU"));
        return p;
    }
    case msg::set_pitch: {
        Pitch p;
        FARLAND_TRY(p.pitch, r.u32le());
        FARLAND_TRY_VOID(r.expect_end("Pitch PDU"));
        return p;
    }
    default:
        return fail(Errc::unsupported, "unknown or unsupported RDPSND server PDU", 0);
    }
}

Result<std::vector<std::byte>> decode_wave_body(std::span<const std::byte> message, const WaveInfo& info)
{
    // [MS-RDPEA] 2.2.3.4: the data after bPad is the sample minus WaveInfo's four bytes.
    if (message.size() != info.sample_size) {
        return fail(Errc::invalid_length, "Wave PDU size does not match WaveInfo", 0);
    }
    Reader r(message);
    FARLAND_TRY(const auto pad, r.u32le());
    if (pad != 0) {
        return fail(Errc::invalid_value, "Wave PDU bPad is not zero", 0);
    }
    std::vector<std::byte> sample(info.first_bytes.begin(), info.first_bytes.end());
    const auto rest = r.rest();
    sample.insert(sample.end(), rest.begin(), rest.end());
    return sample;
}

// --- Client to server ----------------------------------------------------------

std::vector<std::byte> encode_client_pdu(const ClientPdu& pdu)
{
    Writer w;
    std::visit(
        [&w](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, ClientFormats>) {
                // [MS-RDPEA] 2.2.2.2 Client Audio Formats and Version PDU
                FARLAND_ASSERT(p.formats.size() <= max_formats);
                const auto start = begin_pdu(w, msg::formats);
                w.u32le(p.flags);
                w.u32le(p.volume);
                w.u32le(p.pitch);
                w.u16be(p.dgram_port);
                w.u16le(static_cast<std::uint16_t>(p.formats.size()));
                w.u8(0);  // cLastBlockConfirmed
                w.u16le(p.version);
                w.u8(0);  // bPad
                encode_formats(w, p.formats);
                end_pdu(w, start);
            } else if constexpr (std::is_same_v<T, QualityMode>) {
                // [MS-RDPEA] 2.2.2.3 Quality Mode PDU
                const auto start = begin_pdu(w, msg::quality_mode);
                w.u16le(p.mode);
                w.u16le(0);  // Reserved
                end_pdu(w, start);
            } else if constexpr (std::is_same_v<T, TrainingConfirm>) {
                // [MS-RDPEA] 2.2.3.2 Training Confirm PDU
                const auto start = begin_pdu(w, msg::training);
                w.u16le(p.timestamp);
                w.u16le(p.pack_size);
                end_pdu(w, start);
            } else if constexpr (std::is_same_v<T, WaveConfirm>) {
                // [MS-RDPEA] 2.2.3.8 Wave Confirm PDU
                const auto start = begin_pdu(w, msg::wave_confirm);
                w.u16le(p.timestamp);
                w.u8(p.block_no);
                w.u8(0);  // bPad
                end_pdu(w, start);
            }
        },
        pdu);
    return std::move(w).take();
}

Result<ClientPdu> decode_client_pdu(std::span<const std::byte> message)
{
    Reader outer(message);
    FARLAND_TRY(const Header h, decode_header(outer));
    FARLAND_TRY(auto r, exact_body(outer, h));
    switch (h.type) {
    case msg::formats: {
        ClientFormats p;
        FARLAND_TRY(p.flags, r.u32le());
        FARLAND_TRY(p.volume, r.u32le());
        FARLAND_TRY(p.pitch, r.u32le());
        FARLAND_TRY(p.dgram_port, r.u16be());
        FARLAND_TRY(const auto count, r.u16le());
        FARLAND_TRY_VOID(r.skip(1));  // cLastBlockConfirmed
        FARLAND_TRY(p.version, r.u16le());
        FARLAND_TRY_VOID(r.skip(1));  // bPad
        FARLAND_TRY(p.formats, decode_formats(r, count));
        FARLAND_TRY_VOID(r.expect_end("Client Audio Formats and Version PDU"));
        return p;
    }
    case msg::quality_mode: {
        QualityMode p;
        FARLAND_TRY(p.mode, r.u16le());
        FARLAND_TRY_VOID(r.skip(2));  // Reserved
        FARLAND_TRY_VOID(r.expect_end("Quality Mode PDU"));
        return p;
    }
    case msg::training: {
        TrainingConfirm p;
        FARLAND_TRY(p.timestamp, r.u16le());
        FARLAND_TRY(p.pack_size, r.u16le());
        FARLAND_TRY_VOID(r.expect_end("Training Confirm PDU"));
        return p;
    }
    case msg::wave_confirm: {
        WaveConfirm p;
        FARLAND_TRY(p.timestamp, r.u16le());
        FARLAND_TRY(p.block_no, r.u8());
        FARLAND_TRY_VOID(r.skip(1));  // bPad
        FARLAND_TRY_VOID(r.expect_end("Wave Confirm PDU"));
        return p;
    }
    default:
        return fail(Errc::unsupported, "unknown or unsupported RDPSND client PDU", 0);
    }
}

}  // namespace farland::channels::rdpsnd
