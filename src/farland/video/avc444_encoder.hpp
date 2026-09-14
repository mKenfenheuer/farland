// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/codec/avc420.hpp>
#include <farland/codec/image.hpp>
#include <farland/codec/yuv.hpp>
#include <farland/codec/yuv444.hpp>
#include <farland/video/h264_encoder.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

/// True YUV 4:4:4 over RDPGFX: RFX_AVC444_BITMAP_STREAM (v1) and
/// RFX_AVC444V2_BITMAP_STREAM (v2), [MS-RDPEGFX] 2.2.4.5, 2.2.4.6 and 3.3.8.3.
///
/// Each frame is converted to YUV444 and split into a main view (a normal
/// 4:2:0 picture) and an auxiliary view (the remaining chroma), see
/// codec/yuv444.hpp. Both views go through ONE H.264 encoder, as pictures of
/// one stream: "These bitstreams MUST be encoded using the same MPEG-4
/// AVC/H.264 encoder and decoded by a single MPEG-4 AVC/H.264 decoder as one
/// stream" (2.2.4.5). FreeRDP's client indeed feeds both to one decoder
/// (codec/h264.c avc444_decompress), so two independent encoders would break
/// its reference pictures and parameter sets. Every access unit the encoder
/// produces must therefore reach the client, in order. Configure the H.264
/// encoder with EncoderConfig::reference_frames = 2, so each view predicts
/// from its own previous picture rather than from the other view (OpenH264
/// otherwise codes every picture as an IDR, about 7x the size).
///
/// The encoder never sends a view that did not change. A main view updates
/// the regions it lists to 4:2:0 on the client ("color conversion MUST be
/// performed as in YUV420p mode using only the data in the main view"), so a
/// region needs a chroma view after every main view unless its chroma is
/// flat at 2x2 (grey text, solid colours): then 4:2:0 already shows it.
///
/// LC policy (avc420EncodedBitstreamInfo, 2.2.4.5):
/// - A frame with damage sends the main view of the damaged 64x64 tiles.
/// - By default the chroma view of every tile that needs one goes in the same
///   message (LC 0); if no tile needs one, the message is luma only (LC 1).
/// - When bandwidth is short (Avc444Policy::defer_chroma, set by congestion
///   control, or a main view of at least luma_budget_bytes) the chroma waits
///   (LC 1), and the next frame without damage sends it alone (LC 2): text
///   and motion stay fast, colour detail follows. After max_chroma_delay
///   frames it goes out with the next main view regardless (LC 0).
/// - A frame without damage and without pending chroma sends nothing.
///
/// Region rectangles are 64x64 tiles clipped to the surface. Tiles start on
/// multiples of 16, as FreeRDP's v1 decoder requires. The metablocks carry
/// each view's QP (informational, 2.2.4.4).
namespace farland::video {

struct Avc444Policy {
    /// Hold chroma back while the main view keeps changing.
    bool defer_chroma = false;
    /// Also hold it back for a frame whose main view took at least this many
    /// bytes (0: never).
    std::size_t luma_budget_bytes = 0;
    /// Longest wait for chroma, in frames, before it joins a main view anyway
    /// (0: no limit, only frames without damage send it).
    std::uint32_t max_chroma_delay = 8;
    /// A tile's chroma counts as flat when no U or V sample differs from its
    /// 2x2 average by more than this (on the 0..255 scale).
    std::uint8_t chroma_tolerance = 2;

    friend bool operator==(const Avc444Policy&, const Avc444Policy&) = default;
};

/// One encoded frame: the bitmapData of a WireToSurface1 with codecId
/// RDPGFX_CODECID_AVC444 or RDPGFX_CODECID_AVC444V2 (by version()).
struct Avc444Frame {
    codec::avc::Avc444Layout layout = codec::avc::Avc444Layout::luma;
    /// The RFX_AVC444(V2)_BITMAP_STREAM.
    std::vector<std::byte> bitmap_stream;
    /// destRect: the bounding rectangle of all regions.
    codec::avc::Rect16 dest_rect;
    /// The regions of the main and the auxiliary view (empty when not sent).
    std::vector<codec::avc::Rect16> luma_regions;
    std::vector<codec::avc::Rect16> chroma_regions;
    /// The main view is an IDR picture (its regions cover the surface).
    bool idr = false;
};

class Avc444Encoder {
public:
    static constexpr std::uint32_t tile_size = 64;

    /// Takes over `encoder`, which must be configured for the coded size of a
    /// `width` x `height` surface (codec::avc::coded_size, asserted). The
    /// first frame starts the stream with an IDR and covers the surface.
    Avc444Encoder(std::unique_ptr<H264Encoder> encoder, std::uint16_t width, std::uint16_t height,
                  codec::Avc444Version version, Avc444Policy policy = {});

    [[nodiscard]] codec::Avc444Version version() const noexcept { return version_; }
    [[nodiscard]] const Avc444Policy& policy() const noexcept { return policy_; }
    void set_policy(const Avc444Policy& policy) noexcept { policy_ = policy; }
    /// The H.264 encoder, for rate control.
    [[nodiscard]] H264Encoder& h264() noexcept { return *encoder_; }

    /// The next frame starts over: an IDR main view of the whole surface,
    /// then chroma for every tile that needs it.
    void request_idr() noexcept { restart_ = true; }
    /// Some tile waits for its chroma view; a frame without damage sends it.
    [[nodiscard]] bool chroma_pending() const noexcept;

    /// Encodes the current picture of the surface. `damage` lists the
    /// rectangles (surface coordinates, exclusive right and bottom) that
    /// changed since the last call or must be sent again. Returns nullopt
    /// when there is nothing to send, or when the encoder dropped the picture
    /// (the damage then has to be given again). Asserts that `frame` has the
    /// surface's size.
    [[nodiscard]] Result<std::optional<Avc444Frame>>
    encode(const codec::ImageView& frame, std::span<const codec::avc::Rect16> damage, const FrameOptions& options = {});

private:
    /// Waits for a chroma view since that frame number.
    using Pending = std::optional<std::uint64_t>;

    [[nodiscard]] codec::avc::Rect16 tile_rect(std::size_t tile) const noexcept;
    /// Whether the tile's chroma is not flat at 2x2 (see Avc444Policy).
    [[nodiscard]] bool needs_chroma(std::size_t tile) const;
    [[nodiscard]] std::vector<codec::avc::Region> regions(std::span<const std::size_t> tiles, std::uint8_t qp) const;

    std::unique_ptr<H264Encoder> encoder_;
    std::uint16_t width_;
    std::uint16_t height_;
    codec::Avc444Version version_;
    Avc444Policy policy_;
    std::uint32_t tiles_x_;
    std::uint32_t tiles_y_;
    codec::Yuv444Frame yuv_;
    codec::Yuv420Frame main_;
    codec::Yuv420Frame aux_;
    std::vector<Pending> pending_;  ///< per tile
    std::uint64_t frame_ = 0;
    bool restart_ = true;
};

}  // namespace farland::video
