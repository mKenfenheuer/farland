// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

/// Audio output PDUs, [MS-RDPEA] 2.2. One PDU is one reassembled message of
/// the "rdpsnd" static virtual channel or of the AUDIO_PLAYBACK_DVC dynamic
/// channel (2.1); the UDP transport and its PDUs (Crypt Key, Wave Encrypt,
/// UDP Wave) are not implemented.
namespace farland::channels::rdpsnd {

/// msgType of the RDPSND PDU Header, [MS-RDPEA] 2.2.1.
namespace msg {
inline constexpr std::uint8_t close = 0x01;
inline constexpr std::uint8_t wave = 0x02;
inline constexpr std::uint8_t set_volume = 0x03;
inline constexpr std::uint8_t set_pitch = 0x04;
inline constexpr std::uint8_t wave_confirm = 0x05;
inline constexpr std::uint8_t training = 0x06;
inline constexpr std::uint8_t formats = 0x07;
inline constexpr std::uint8_t crypt_key = 0x08;
inline constexpr std::uint8_t wave_encrypt = 0x09;
inline constexpr std::uint8_t udp_wave = 0x0A;
inline constexpr std::uint8_t udp_wave_last = 0x0B;
inline constexpr std::uint8_t quality_mode = 0x0C;
inline constexpr std::uint8_t wave2 = 0x0D;
}  // namespace msg

/// wVersion values ([MS-RDPEA] 3.1.1.1 and its product behavior notes).
namespace version {
inline constexpr std::uint16_t windows_xp = 0x02;
inline constexpr std::uint16_t windows_vista = 0x05;
inline constexpr std::uint16_t windows_7 = 0x06;  ///< Quality Mode PDU
inline constexpr std::uint16_t windows_8 = 0x08;  ///< Wave2 PDU
}  // namespace version

/// dwFlags of the Client Audio Formats and Version PDU, [MS-RDPEA] 2.2.2.2.
namespace caps {
inline constexpr std::uint32_t alive = 0x00000001;
inline constexpr std::uint32_t volume = 0x00000002;
inline constexpr std::uint32_t pitch = 0x00000004;
}  // namespace caps

/// wQualityMode, [MS-RDPEA] 2.2.2.3.
namespace quality {
inline constexpr std::uint16_t dynamic = 0x0000;
inline constexpr std::uint16_t medium = 0x0001;
inline constexpr std::uint16_t high = 0x0002;
}  // namespace quality

/// wFormatTag values farland knows by name ([RFC2361]; Opus and AAC are
/// Microsoft registrations used by Windows and FreeRDP).
namespace format_tag {
inline constexpr std::uint16_t pcm = 0x0001;
inline constexpr std::uint16_t adpcm = 0x0002;
inline constexpr std::uint16_t alaw = 0x0006;
inline constexpr std::uint16_t mulaw = 0x0007;
inline constexpr std::uint16_t dvi_adpcm = 0x0011;
inline constexpr std::uint16_t gsm610 = 0x0031;
inline constexpr std::uint16_t mpeg_layer3 = 0x0055;
inline constexpr std::uint16_t opus = 0x704F;
inline constexpr std::uint16_t aac_ms = 0xA106;
inline constexpr std::uint16_t extensible = 0xFFFE;
}  // namespace format_tag

/// Size of the RDPSND PDU Header (SNDPROLOG).
inline constexpr std::size_t header_size = 4;
/// farland limits on what a peer may announce.
inline constexpr std::size_t max_formats = 256;
inline constexpr std::size_t max_format_extra = 1024;

/// AUDIO_FORMAT, [MS-RDPEA] 2.2.2.1.1 (a WAVEFORMATEX with its extra bytes).
struct AudioFormat {
    std::uint16_t tag = 0;
    std::uint16_t channels = 0;
    std::uint32_t samples_per_sec = 0;
    std::uint32_t avg_bytes_per_sec = 0;
    std::uint16_t block_align = 0;
    std::uint16_t bits_per_sample = 0;
    std::vector<std::byte> extra;  ///< cbSize bytes

    friend bool operator==(const AudioFormat&, const AudioFormat&) = default;
};

/// Linear PCM, little-endian, interleaved.
[[nodiscard]] AudioFormat pcm_format(std::uint32_t rate, std::uint16_t channels, std::uint16_t bits = 16);

void encode(Writer& w, const AudioFormat& format);
/// Rejects cbSize above max_format_extra and zero channels or block alignment
/// (every decoder divides by them).
[[nodiscard]] Result<AudioFormat> decode_audio_format(Reader& r);

// --- Server to client ----------------------------------------------------------

/// Server Audio Formats and Version PDU, [MS-RDPEA] 2.2.2.1. dwFlags,
/// dwVolume, dwPitch and wDGramPort are unused and written as 0.
struct ServerFormats {
    std::uint16_t version = version::windows_8;
    std::uint8_t last_block_confirmed = 0;
    std::vector<AudioFormat> formats;

    friend bool operator==(const ServerFormats&, const ServerFormats&) = default;
};

/// Training PDU, [MS-RDPEA] 2.2.3.1. wPackSize is the size of the whole PDU,
/// or 0 without data.
struct Training {
    std::uint16_t timestamp = 0;
    std::uint16_t data_size = 0;  ///< arbitrary (zero) bytes after the fixed fields

    friend bool operator==(const Training&, const Training&) = default;
};

/// WaveInfo PDU, [MS-RDPEA] 2.2.3.3: the first four bytes of an audio sample,
/// whose rest follows in a Wave PDU (2.2.3.4).
struct WaveInfo {
    std::uint16_t timestamp = 0;
    std::uint16_t format_no = 0;
    std::uint8_t block_no = 0;
    std::array<std::byte, 4> first_bytes{};
    /// Size of the whole audio sample: BodySize minus 8 (3.3.5.2.1.1).
    std::uint16_t sample_size = 0;

    friend bool operator==(const WaveInfo&, const WaveInfo&) = default;
};

/// Wave2 PDU, [MS-RDPEA] 2.2.3.10.
struct Wave2 {
    std::uint16_t timestamp = 0;
    std::uint16_t format_no = 0;
    std::uint8_t block_no = 0;
    std::uint32_t audio_timestamp = 0;  ///< milliseconds since system start, when the audio was captured
    std::vector<std::byte> data;

    friend bool operator==(const Wave2&, const Wave2&) = default;
};

/// Close PDU, [MS-RDPEA] 2.2.3.9.
struct Close {
    friend bool operator==(const Close&, const Close&) = default;
};

/// Volume PDU, [MS-RDPEA] 2.2.4.1: left channel in the low word, right in the high one.
struct Volume {
    std::uint32_t volume = 0;
    friend bool operator==(const Volume&, const Volume&) = default;
};

/// Pitch PDU, [MS-RDPEA] 2.2.4.2 (clients ignore it).
struct Pitch {
    std::uint32_t pitch = 0;
    friend bool operator==(const Pitch&, const Pitch&) = default;
};

using ServerPdu = std::variant<ServerFormats, Training, WaveInfo, Wave2, Close, Volume, Pitch>;

/// One audio sample as sent over a virtual channel.
struct Wave {
    std::uint16_t timestamp = 0;
    std::uint16_t format_no = 0;
    std::uint8_t block_no = 0;
    std::span<const std::byte> data;
};

/// Encodes a server PDU. A WaveInfo cannot be encoded alone; use encode_wave.
[[nodiscard]] std::vector<std::byte> encode_server_pdu(const ServerPdu& pdu);

/// A WaveInfo PDU and the Wave PDU that follows it ([MS-RDPEA] 3.3.5.2.1.1,
/// 3.3.5.2.1.2), as two messages. The sample must be longer than four bytes
/// and its BodySize must fit 16 bits (at most 65527 bytes).
[[nodiscard]] std::array<std::vector<std::byte>, 2> encode_wave(const Wave& wave);

/// Decodes a server PDU (for tests, fuzzing and a future client). A Wave PDU
/// has no header of its own: after a WaveInfo, pass the next message to
/// decode_wave_body instead.
[[nodiscard]] Result<ServerPdu> decode_server_pdu(std::span<const std::byte> message);
/// The whole audio sample from the Wave PDU following `info`: its first four
/// bytes (the bPad field) are replaced with WaveInfo's.
[[nodiscard]] Result<std::vector<std::byte>> decode_wave_body(std::span<const std::byte> message, const WaveInfo& info);

// --- Client to server ----------------------------------------------------------

/// Client Audio Formats and Version PDU, [MS-RDPEA] 2.2.2.2.
/// cLastBlockConfirmed and bPad are unused and written as 0.
struct ClientFormats {
    std::uint32_t flags = 0;  ///< caps::*
    std::uint32_t volume = 0;
    std::uint32_t pitch = 0;
    std::uint16_t dgram_port = 0;  ///< big-endian on the wire; 0: no UDP
    std::uint16_t version = 0;
    std::vector<AudioFormat> formats;

    friend bool operator==(const ClientFormats&, const ClientFormats&) = default;
};

/// Quality Mode PDU, [MS-RDPEA] 2.2.2.3.
struct QualityMode {
    std::uint16_t mode = quality::dynamic;
    friend bool operator==(const QualityMode&, const QualityMode&) = default;
};

/// Training Confirm PDU, [MS-RDPEA] 2.2.3.2.
struct TrainingConfirm {
    std::uint16_t timestamp = 0;
    std::uint16_t pack_size = 0;
    friend bool operator==(const TrainingConfirm&, const TrainingConfirm&) = default;
};

/// Wave Confirm PDU, [MS-RDPEA] 2.2.3.8.
struct WaveConfirm {
    std::uint16_t timestamp = 0;
    std::uint8_t block_no = 0;
    friend bool operator==(const WaveConfirm&, const WaveConfirm&) = default;
};

using ClientPdu = std::variant<ClientFormats, QualityMode, TrainingConfirm, WaveConfirm>;

[[nodiscard]] std::vector<std::byte> encode_client_pdu(const ClientPdu& pdu);

/// Decodes one client PDU. Strict: BodySize must cover exactly the rest of
/// the message, and PDUs a client does not send (or that belong to the UDP
/// transport, which farland never offers) are errors.
[[nodiscard]] Result<ClientPdu> decode_client_pdu(std::span<const std::byte> message);

}  // namespace farland::channels::rdpsnd
