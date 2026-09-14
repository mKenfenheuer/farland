// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/channels/audin.hpp>

#include <algorithm>
#include <array>
#include <type_traits>

namespace farland::channels {

namespace audin {

namespace {

/// KSDATAFORMAT_SUBTYPE_PCM {00000001-0000-0010-8000-00aa00389b71} as a
/// little-endian GUID ([MS-RDPEAI] 2.2.2.3.1).
constexpr std::array<std::uint8_t, 16> subtype_pcm{0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                                   0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
constexpr std::uint32_t speaker_front_left = 0x1;
constexpr std::uint32_t speaker_front_right = 0x2;
constexpr std::uint32_t speaker_front_center = 0x4;
/// Size of WAVEFORMAT_EXTENSIBLE's extra part, the cbSize it requires.
constexpr std::size_t extensible_size = 22;

}  // namespace

rdpsnd::AudioFormat extensible_pcm_format(std::uint32_t rate, std::uint16_t channels)
{
    FARLAND_ASSERT(channels == 1 || channels == 2);
    auto format = rdpsnd::pcm_format(rate, channels);
    format.tag = rdpsnd::format_tag::extensible;
    Writer w(extensible_size);
    w.u16le(16);  // wValidBitsPerSample
    w.u32le(channels == 1 ? speaker_front_center : speaker_front_left | speaker_front_right);
    for (const std::uint8_t b : subtype_pcm) {
        w.u8(b);
    }
    format.extra = std::move(w).take();
    return format;
}

std::vector<std::byte> encode_server_pdu(const ServerPdu& pdu)
{
    Writer w;
    std::visit(
        [&w](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, Version>) {
                // [MS-RDPEAI] 2.2.2.1 Version PDU
                w.u8(msg::version);
                w.u32le(p.version);
            } else if constexpr (std::is_same_v<T, Formats>) {
                // [MS-RDPEAI] 2.2.2.2 Sound Formats PDU; cbSizeFormatsPacket
                // is reserved from the server, farland writes the size anyway.
                w.u8(msg::formats);
                w.u32le(static_cast<std::uint32_t>(p.formats.size()));
                w.u32le(0);
                for (const auto& format : p.formats) {
                    rdpsnd::encode(w, format);
                }
                w.patch_u32le(5, static_cast<std::uint32_t>(w.size()));
            } else if constexpr (std::is_same_v<T, Open>) {
                // [MS-RDPEAI] 2.2.2.3 Open PDU
                w.u8(msg::open);
                w.u32le(p.frames_per_packet);
                w.u32le(p.initial_format);
                rdpsnd::encode(w, p.capture_format);
            } else if constexpr (std::is_same_v<T, FormatChange>) {
                // [MS-RDPEAI] 2.2.4.1 Format Change PDU
                w.u8(msg::format_change);
                w.u32le(p.new_format);
            }
        },
        pdu);
    return std::move(w).take();
}

std::vector<std::byte> encode_client_pdu(const ClientPdu& pdu)
{
    Writer w;
    std::visit(
        [&w](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, Version>) {
                w.u8(msg::version);
                w.u32le(p.version);
            } else if constexpr (std::is_same_v<T, Formats>) {
                // cbSizeFormatsPacket: the PDU without ExtraData (2.2.2.2).
                w.u8(msg::formats);
                w.u32le(static_cast<std::uint32_t>(p.formats.size()));
                w.u32le(0);
                for (const auto& format : p.formats) {
                    rdpsnd::encode(w, format);
                }
                w.patch_u32le(5, static_cast<std::uint32_t>(w.size()));
            } else if constexpr (std::is_same_v<T, OpenReply>) {
                w.u8(msg::open_reply);
                w.u32le(p.result);
            } else if constexpr (std::is_same_v<T, IncomingData>) {
                w.u8(msg::data_incoming);
            } else if constexpr (std::is_same_v<T, Data>) {
                w.u8(msg::data);
                w.bytes(p.data);
            } else if constexpr (std::is_same_v<T, FormatChange>) {
                w.u8(msg::format_change);
                w.u32le(p.new_format);
            }
        },
        pdu);
    return std::move(w).take();
}

namespace {

Result<std::vector<rdpsnd::AudioFormat>> decode_format_list(Reader& r, std::uint32_t count)
{
    if (count > rdpsnd::max_formats) {
        return fail(Errc::limit_exceeded, "too many audio formats", r.offset());
    }
    std::vector<rdpsnd::AudioFormat> formats;
    formats.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        FARLAND_TRY(auto format, rdpsnd::decode_audio_format(r));
        formats.push_back(std::move(format));
    }
    return formats;
}

}  // namespace

Result<ClientPdu> decode_client_pdu(std::span<const std::byte> message)
{
    Reader r(message);
    FARLAND_TRY(const auto id, r.u8());
    switch (id) {
    case msg::version: {
        Version p;
        FARLAND_TRY(p.version, r.u32le());
        FARLAND_TRY_VOID(r.expect_end("Version PDU"));
        return p;
    }
    case msg::formats: {
        // [MS-RDPEAI] 2.2.2.2: cbSizeFormatsPacket is where ExtraData starts.
        FARLAND_TRY(const auto count, r.u32le());
        FARLAND_TRY(const auto size, r.u32le());
        Formats p;
        FARLAND_TRY(p.formats, decode_format_list(r, count));
        if (size != r.position()) {
            return fail(Errc::invalid_length, "Sound Formats cbSizeFormatsPacket does not match", 5);
        }
        return p;
    }
    case msg::open_reply: {
        OpenReply p;
        FARLAND_TRY(p.result, r.u32le());
        FARLAND_TRY_VOID(r.expect_end("Open Reply PDU"));
        return p;
    }
    case msg::data_incoming:
        FARLAND_TRY_VOID(r.expect_end("Incoming Data PDU"));
        return IncomingData{};
    case msg::data:
        return Data{r.rest()};
    case msg::format_change: {
        FormatChange p;
        FARLAND_TRY(p.new_format, r.u32le());
        FARLAND_TRY_VOID(r.expect_end("Format Change PDU"));
        return p;
    }
    default:
        return fail(Errc::unsupported, "unknown or unexpected SNDIN client PDU", 0);
    }
}

Result<ServerPdu> decode_server_pdu(std::span<const std::byte> message)
{
    Reader r(message);
    FARLAND_TRY(const auto id, r.u8());
    switch (id) {
    case msg::version: {
        Version p;
        FARLAND_TRY(p.version, r.u32le());
        FARLAND_TRY_VOID(r.expect_end("Version PDU"));
        return p;
    }
    case msg::formats: {
        FARLAND_TRY(const auto count, r.u32le());
        FARLAND_TRY_VOID(r.skip(4));  // cbSizeFormatsPacket, reserved from the server
        Formats p;
        FARLAND_TRY(p.formats, decode_format_list(r, count));
        return p;  // ExtraData, if any, is ignored
    }
    case msg::open: {
        Open p;
        FARLAND_TRY(p.frames_per_packet, r.u32le());
        FARLAND_TRY(p.initial_format, r.u32le());
        FARLAND_TRY(p.capture_format, rdpsnd::decode_audio_format(r));
        if (p.capture_format.tag == rdpsnd::format_tag::extensible && p.capture_format.extra.size() != 22) {
            return fail(Errc::invalid_length, "WAVE_FORMAT_EXTENSIBLE needs cbSize 22", r.offset());
        }
        FARLAND_TRY_VOID(r.expect_end("Open PDU"));
        return p;
    }
    case msg::format_change: {
        FormatChange p;
        FARLAND_TRY(p.new_format, r.u32le());
        FARLAND_TRY_VOID(r.expect_end("Format Change PDU"));
        return p;
    }
    default:
        return fail(Errc::unsupported, "unknown or unexpected SNDIN server PDU", 0);
    }
}

}  // namespace audin

AudinServer::AudinServer(AudinServerConfig config) : config_(std::move(config))
{
    FARLAND_ASSERT(!config_.formats.empty() && config_.formats.size() <= rdpsnd::max_formats);
    FARLAND_ASSERT(std::ranges::all_of(config_.formats, [](const rdpsnd::AudioFormat& f) {
        return f.tag == rdpsnd::format_tag::pcm && f.bits_per_sample == 16;
    }));
}

void AudinServer::start()
{
    FARLAND_ASSERT(state_ == State::idle);
    // [MS-RDPEAI] 3.1.5.1: the first PDU is always the Version PDU.
    output_.push_back(audin::encode_server_pdu(audin::Version{audin::version2}));
    state_ = State::waiting_version;
}

Result<void> AudinServer::receive(std::span<const std::byte> message)
{
    if (error_) {
        return std::unexpected(*error_);
    }
    auto pdu = audin::decode_client_pdu(message);
    Result<void> result;
    if (!pdu) {
        result = std::unexpected(pdu.error());
    } else {
        result = std::visit([this](const auto& p) { return handle(p); }, *pdu);
    }
    if (!result) {
        error_ = result.error();
        state_ = State::failed;
    }
    return result;
}

// [MS-RDPEAI] 3.3.5.1.2 Processing a Version PDU: the server's Sound Formats follow.
Result<void> AudinServer::handle(const audin::Version& pdu)
{
    if (state_ != State::waiting_version) {
        return {};
    }
    client_version_ = pdu.version;
    output_.push_back(audin::encode_server_pdu(audin::Formats{config_.formats}));
    state_ = State::waiting_formats;
    return {};
}

// [MS-RDPEAI] 3.3.5.1.4 Processing a Sound Formats PDU
Result<void> AudinServer::handle(const audin::Formats& pdu)
{
    if (state_ != State::waiting_formats) {
        return {};
    }
    for (const auto& format : pdu.formats) {
        // 3.2.5.1.5: the client's list MUST be a subset of the server's.
        if (std::ranges::find(config_.formats, format) == config_.formats.end()) {
            return fail(Errc::invalid_value, "client audio input format the server did not offer");
        }
    }
    client_formats_ = pdu.formats;
    // The first format of the server's list (its preference) that the client has.
    for (const auto& wanted : config_.formats) {
        const auto found = std::ranges::find(client_formats_, wanted);
        if (found == client_formats_.end()) {
            continue;
        }
        current_format_ = static_cast<std::uint32_t>(found - client_formats_.begin());
        // FramesPerPacket: frames of `packet_ms` (3.3.5.1.5).
        frames_per_packet_ = std::max<std::uint32_t>(1, wanted.samples_per_sec * config_.packet_ms / 1000);
        output_.push_back(audin::encode_server_pdu(audin::Open{
            .frames_per_packet = frames_per_packet_,
            .initial_format = current_format_,
            .capture_format = audin::extensible_pcm_format(wanted.samples_per_sec, wanted.channels),
        }));
        state_ = State::waiting_reply;
        return {};
    }
    state_ = State::failed;
    events_.emplace_back(audin_event::Failed{.reason = "no audio input format in common with the client", .result = 0});
    return {};
}

// [MS-RDPEAI] 3.3.5.1.6 Processing an Open Reply PDU
Result<void> AudinServer::handle(const audin::OpenReply& pdu)
{
    if (state_ != State::waiting_reply) {
        return {};
    }
    if ((pdu.result & 0x80000000U) != 0) {
        state_ = State::failed;
        events_.emplace_back(
            audin_event::Failed{.reason = "the client could not open its microphone", .result = pdu.result});
        return {};
    }
    state_ = State::opened;
    events_.emplace_back(audin_event::Opened{client_formats_.at(current_format_), frames_per_packet_});
    return {};
}

// One of the overloads std::visit dispatches to, so a member like the others.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
Result<void> AudinServer::handle(const audin::IncomingData& /*pdu*/)
{
    return {};  // diagnostic only (2.2.3.1)
}

// [MS-RDPEAI] 3.3.5.2.2 Processing a Data PDU
Result<void> AudinServer::handle(const audin::Data& pdu)
{
    if (state_ != State::opened) {
        return {};
    }
    const auto& format = client_formats_.at(current_format_);
    if (pdu.data.size() % format.block_align != 0) {
        return fail(Errc::invalid_length, "audio input data is not a whole number of frames");
    }
    events_.emplace_back(audin_event::Data{pdu.data});
    return {};
}

// [MS-RDPEAI] 3.3.5.1.7 / 3.3.5.3.1: the client confirms the format of the
// Open PDU (or of a Format Change the server sent; farland sends none).
Result<void> AudinServer::handle(const audin::FormatChange& pdu)
{
    if (state_ != State::waiting_reply && state_ != State::opened) {
        return {};
    }
    if (pdu.new_format != current_format_) {
        return fail(Errc::invalid_value, "Format Change PDU for a format the server did not ask for");
    }
    return {};
}

std::optional<AudinEvent> AudinServer::poll_event()
{
    if (events_.empty()) {
        return std::nullopt;
    }
    AudinEvent event = events_.front();
    events_.pop_front();
    return event;
}

}  // namespace farland::channels
