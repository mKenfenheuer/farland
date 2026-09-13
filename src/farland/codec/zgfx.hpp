// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/writer.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

/// ZGFX, the RDP 8.0 bulk compression of [MS-RDPEGFX] 3.1.9.1: LZ77 over a
/// history that persists for the lifetime of a channel, with a static
/// Huffman-like token code. Every server-to-client RDPGFX message travels in
/// an RDP_SEGMENTED_DATA structure ([MS-RDPEGFX] 2.2.5.1) produced by it.
///
/// The "RDP 8.0 Lite" variant ([MS-RDPEDYC] 2.2.3.3 and 2.2.3.4) compresses
/// the Data field of DYNVC_DATA_FIRST_COMPRESSED and DYNVC_DATA_COMPRESSED.
/// It is the same format with an 8,192-byte history, at most 8,192 bytes per
/// segment, only a single segment per RDP_SEGMENTED_DATA and compression
/// type 0x06 instead of 0x04.
///
/// Both classes keep the history across calls: use one instance per channel
/// and direction ([MS-RDPEDYC] 3.1.5.1.3: "Each channel MUST use a dedicated
/// compression context"). They are not thread-safe.
namespace farland::codec {

namespace zgfx {

// RDP_SEGMENTED_DATA descriptor, [MS-RDPEGFX] 2.2.5.1.
inline constexpr std::uint8_t descriptor_single = 0xE0;
inline constexpr std::uint8_t descriptor_multipart = 0xE1;

// RDP8_BULK_ENCODED_DATA header, [MS-RDPEGFX] 2.2.5.3.
inline constexpr std::uint8_t compression_type_mask = 0x0F;
inline constexpr std::uint8_t packet_compressed = 0x20;
inline constexpr std::uint8_t packet_compr_type_rdp8 = 0x04;
/// PACKET_COMPR_TYPE_RDP8_LITE, [MS-RDPEDYC] 2.2.3.3.
inline constexpr std::uint8_t packet_compr_type_rdp8_lite = 0x06;

// RDP 8.0 compressor limits, [MS-RDPEGFX] 3.1.9.1.2.
inline constexpr std::size_t max_segment_size = 65535;
inline constexpr std::size_t history_size = 2'500'000;
inline constexpr std::size_t max_segment_count = 65535;

// RDP 8.0 Lite limits, [MS-RDPEDYC] 2.2.3.3.
inline constexpr std::size_t lite_max_segment_size = 8192;
inline constexpr std::size_t lite_history_size = 8192;

/// Default ZgfxDecompressor limit on the output of one RDP_SEGMENTED_DATA.
inline constexpr std::size_t default_max_output_size = std::size_t{64} << 20U;

}  // namespace zgfx

enum class ZgfxVariant : std::uint8_t {
    rdp8,       ///< [MS-RDPEGFX] 3.1.9.1, the graphics channel.
    rdp8_lite,  ///< [MS-RDPEDYC] 2.2.3.3, compressed dynamic channel data.
};

enum class ZgfxMode : std::uint8_t {
    /// LZ77 compression; each segment is stored raw instead when that is not
    /// larger than the compressed form.
    compress,
    /// Every segment is stored raw (debugging and interop checks). The
    /// decompressor's history still records the bytes, as it must.
    store,
};

/// Largest input one ZgfxCompressor::compress call accepts.
[[nodiscard]] constexpr std::size_t zgfx_max_input_size(ZgfxVariant variant) noexcept
{
    return variant == ZgfxVariant::rdp8 ? zgfx::max_segment_size * zgfx::max_segment_count
                                        : zgfx::lite_max_segment_size;
}

/// Produces RDP_SEGMENTED_DATA ([MS-RDPEGFX] 2.2.5.1). Input that fits one
/// segment gives SINGLE, anything larger MULTIPART with segments of at most
/// 65,535 uncompressed bytes. Each segment is independently compressed or
/// raw, whichever is smaller, so a SINGLE never exceeds the input by more than
/// 2 bytes (descriptor and header) and a MULTIPART by 7 + 5 per segment. The
/// one exception is empty input, which becomes the 3 bytes E0, header with
/// PACKET_COMPRESSED, 00 (an empty bit stream; FreeRDP rejects an empty raw
/// segment).
///
/// Matches reach back into everything compressed since construction or
/// reset(), up to the history size of the variant. Memory grows with the data
/// compressed, up to about 21.5 MiB for rdp8 (a 5 MiB window, 16 MiB of hash
/// chains, 256 KiB of hash heads) and about 60 KiB for rdp8_lite; store mode
/// needs none. A ZgfxDecompressor holds up to the history size (2.5 MB).
class ZgfxCompressor {
public:
    explicit ZgfxCompressor(ZgfxVariant variant = ZgfxVariant::rdp8, ZgfxMode mode = ZgfxMode::compress);
    ~ZgfxCompressor();
    ZgfxCompressor(ZgfxCompressor&&) noexcept;
    ZgfxCompressor& operator=(ZgfxCompressor&&) noexcept;
    ZgfxCompressor(const ZgfxCompressor&) = delete;
    ZgfxCompressor& operator=(const ZgfxCompressor&) = delete;

    /// Compresses `data` into one RDP_SEGMENTED_DATA. Asserts that `data` is
    /// at most zgfx_max_input_size(variant) bytes: rdp8_lite is a single
    /// segment of at most 8,192 bytes by definition ([MS-RDPEDYC] 2.2.3.3).
    [[nodiscard]] std::vector<std::byte> compress(std::span<const std::byte> data);

    /// As above, appending to `out` (for example after a PDU header).
    void compress(std::span<const std::byte> data, Writer& out);

    /// Forgets the history. The peer's decompressor must be reset at the same
    /// point in the stream (a new channel).
    void reset();

    [[nodiscard]] ZgfxVariant variant() const noexcept;
    [[nodiscard]] ZgfxMode mode() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

/// Decodes RDP_SEGMENTED_DATA ([MS-RDPEGFX] 2.2.5.1), SINGLE or MULTIPART,
/// with raw and compressed segments. Rejects: unknown descriptors, a
/// compression type other than the variant's, reserved tokens, match
/// distances beyond the bytes decoded so far or the history size, segments
/// that decode to more than the variant's segment limit, a MULTIPART whose
/// segments do not add up to uncompressedSize exactly, and trailing bytes.
///
/// After an error the history is undefined; the caller must drop the channel
/// (or reset() both ends).
class ZgfxDecompressor {
public:
    /// `max_output_size` bounds the output of each decompress() call; larger
    /// messages fail with Errc::limit_exceeded before anything is allocated
    /// for them (MULTIPART) or as soon as the output passes the limit.
    explicit ZgfxDecompressor(ZgfxVariant variant = ZgfxVariant::rdp8,
                              std::size_t max_output_size = zgfx::default_max_output_size);
    ~ZgfxDecompressor();
    ZgfxDecompressor(ZgfxDecompressor&&) noexcept;
    ZgfxDecompressor& operator=(ZgfxDecompressor&&) noexcept;
    ZgfxDecompressor(const ZgfxDecompressor&) = delete;
    ZgfxDecompressor& operator=(const ZgfxDecompressor&) = delete;

    /// Decompresses one RDP_SEGMENTED_DATA.
    [[nodiscard]] Result<std::vector<std::byte>> decompress(std::span<const std::byte> data);

    /// Forgets the history.
    void reset();

    [[nodiscard]] ZgfxVariant variant() const noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace farland::codec
