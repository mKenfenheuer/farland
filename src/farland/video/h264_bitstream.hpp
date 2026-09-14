// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/video/h264_encoder.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

/// H.264 parameter sets and slice headers ([ITU-H.264-201201] 7.3), as far as
/// the hardware encoders need them:
/// - write SPS and PPS for drivers that take packed headers;
/// - replace the VUI of the SPS a driver wrote itself, which often lacks the
///   full-range BT.709 signal and the bitstream restriction that tells
///   decoders not to wait for reordered pictures;
/// - read the slice QP back for EncodedFrame::qp;
/// - turn what a driver produced into the access units of h264_encoder.hpp.
///
/// Only encoder output is parsed here, never data from the network.
namespace farland::video::bitstream {

/// Writes RBSP bits, most significant first ([ITU-H.264-201201] 7.2).
class BitWriter {
public:
    /// The low `count` bits of `value`, count <= 32.
    void bits(std::uint32_t value, unsigned count);
    void flag(bool value) { bits(value ? 1U : 0U, 1); }
    /// ue(v), Exp-Golomb ([ITU-H.264-201201] 9.1).
    void ue(std::uint32_t value);
    /// se(v) ([ITU-H.264-201201] 9.1.1).
    void se(std::int32_t value);
    /// rbsp_trailing_bits ([ITU-H.264-201201] 7.3.2.11): a one, then zeros to
    /// the byte boundary.
    void trailing_bits();

    [[nodiscard]] std::size_t bit_count() const noexcept { return (bytes_.size() * 8U) + filled_; }
    /// The complete bytes written so far.
    [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }

private:
    std::vector<std::byte> bytes_;
    std::uint32_t current_ = 0;
    unsigned filled_ = 0;
};

/// Reads RBSP bits, most significant first.
class BitReader {
public:
    explicit BitReader(std::span<const std::byte> rbsp) noexcept : data_(rbsp) {}

    /// `count` bits, count <= 32.
    [[nodiscard]] Result<std::uint32_t> bits(unsigned count);
    [[nodiscard]] Result<bool> flag();
    [[nodiscard]] Result<std::uint32_t> ue();
    [[nodiscard]] Result<std::int32_t> se();
    /// Bits read so far.
    [[nodiscard]] std::size_t position() const noexcept { return position_; }

private:
    std::span<const std::byte> data_;
    std::size_t position_ = 0;
};

/// Removes the emulation_prevention_three_byte of a NAL unit payload
/// ([ITU-H.264-201201] 7.4.1).
[[nodiscard]] std::vector<std::byte> unescape(std::span<const std::byte> payload);
/// Inserts emulation_prevention_three_byte where the RBSP would contain a
/// start code prefix.
[[nodiscard]] std::vector<std::byte> escape(std::span<const std::byte> rbsp);

/// The VUI farland writes into every SPS ([ITU-H.264-201201] E.1.1): the
/// colour space (sRGB primaries and transfer, the matrix and range of
/// `color`), the frame rate, and a bitstream restriction without reordering
/// so that decoders output every picture at once.
struct Vui {
    ColorSpace color;
    /// Frame rate for timing_info; 0 leaves timing_info out.
    std::uint32_t fps = 0;
    /// max_dec_frame_buffering: the reference frames the stream keeps.
    std::uint32_t max_dec_frame_buffering = 1;
};

struct SpsParams {
    Profile profile = Profile::constrained_baseline;
    std::uint8_t level_idc = 40;
    std::uint32_t width_mbs = 0;
    std::uint32_t height_mbs = 0;
    /// log2_max_frame_num and log2_max_pic_order_cnt_lsb, 4..16.
    std::uint32_t log2_max_frame_num = 16;
    std::uint32_t log2_max_poc_lsb = 16;
    std::uint32_t max_num_ref_frames = 1;
    Vui vui;
};

struct PpsParams {
    Profile profile = Profile::constrained_baseline;  ///< CABAC above baseline, 8x8 transform in high.
    std::uint8_t pic_init_qp = 26;
};

/// A complete SPS NAL unit (header byte included, no start code) with
/// seq_parameter_set_id 0, frames only, POC type 0, and the VUI of `params`.
[[nodiscard]] std::vector<std::byte> write_sps(const SpsParams& params);
/// A complete PPS NAL unit (ids 0, one reference, deblocking control present).
[[nodiscard]] std::vector<std::byte> write_pps(const PpsParams& params);

/// A slice header for drivers that take packed slice headers
/// (VA_ENC_PACKED_HEADER_SLICE): Mesa's radeonsi, for one, takes the NAL
/// header of the slice only from there.
struct SliceHeaderParams {
    Profile profile = Profile::constrained_baseline;
    bool idr = false;
    std::uint32_t frame_num = 0;
    std::uint16_t idr_pic_id = 0;
    std::uint32_t poc_lsb = 0;
    std::int32_t qp_delta = 0;
    std::uint32_t log2_max_frame_num = 16;
    std::uint32_t log2_max_poc_lsb = 16;
};

/// The NAL header byte and slice_header() ([ITU-H.264-201201] 7.3.3) for our
/// SPS and PPS, escaped, without start code. The header ends inside a byte
/// (the slice data follows directly): `bits` is its exact length.
struct PackedSliceHeader {
    std::vector<std::byte> bytes;
    std::size_t bits = 0;
};
[[nodiscard]] PackedSliceHeader write_slice_header(const SliceHeaderParams& params);

/// What a slice header depends on from the SPS ([ITU-H.264-201201] 7.3.2.1.1).
struct SpsInfo {
    std::uint8_t profile_idc = 0;
    std::uint8_t level_idc = 0;
    std::uint32_t log2_max_frame_num = 4;
    std::uint32_t pic_order_cnt_type = 0;
    std::uint32_t log2_max_poc_lsb = 4;
    bool delta_pic_order_always_zero = false;
    std::uint32_t max_num_ref_frames = 0;
    std::uint32_t width_mbs = 0;
    std::uint32_t height_mbs = 0;
    bool frame_mbs_only = true;
    bool vui_present = false;
    /// Position of vui_parameters_present_flag in the RBSP, in bits.
    std::size_t vui_flag_position = 0;
};

/// What a slice header depends on from the PPS ([ITU-H.264-201201] 7.3.2.2).
struct PpsInfo {
    bool entropy_coding_mode = false;
    bool bottom_field_pic_order_in_frame_present = false;
    bool weighted_pred = false;
    std::uint32_t weighted_bipred_idc = 0;
    std::int32_t pic_init_qp = 26;
    bool redundant_pic_cnt_present = false;
};

/// Parse a complete SPS or PPS NAL unit. Scaling matrices, slice groups and
/// other features no encoder here uses are Errc::unsupported.
[[nodiscard]] Result<SpsInfo> parse_sps(std::span<const std::byte> nal);
[[nodiscard]] Result<PpsInfo> parse_pps(std::span<const std::byte> nal);

/// The SPS NAL unit `nal` with its VUI replaced by `vui`; everything before
/// the VUI is kept bit for bit. max_dec_frame_buffering is raised to the
/// SPS's max_num_ref_frames if it is lower.
[[nodiscard]] Result<std::vector<std::byte>> replace_vui(std::span<const std::byte> nal, Vui vui);

/// SliceQPY of an I or P slice NAL unit ([ITU-H.264-201201] 7.4.3):
/// pic_init_qp + slice_qp_delta. B slices and explicit weighted prediction
/// are Errc::unsupported.
[[nodiscard]] Result<std::uint8_t> slice_qp(std::span<const std::byte> nal, const SpsInfo& sps, const PpsInfo& pps);

/// The lowest level_idc ([ITU-H.264-201201] Table A-1) for pictures of
/// `width_mbs` x `height_mbs` macroblocks at `fps` and `max_kbps` (0: no
/// bitrate limit), or 62 if none suffices.
[[nodiscard]] std::uint8_t level_for(std::uint32_t width_mbs, std::uint32_t height_mbs, std::uint32_t fps,
                                     std::uint32_t max_kbps, Profile profile) noexcept;

/// Turns the access units a hardware encoder wrote into the form of
/// h264_encoder.hpp: our own access unit delimiter first (if configured),
/// SPS (with Vui) and PPS before every IDR, then the slices. Driver
/// delimiters, SEI (which may refer to HRD parameters the new VUI drops) and
/// filler data are dropped. Parameter sets are remembered, so an IDR whose
/// access unit lacks them gets the last ones.
class AccessUnitWriter {
public:
    struct Options {
        bool access_unit_delimiters = true;
        Vui vui;
    };

    /// Forgets the parameter sets, for a new stream.
    void reset(const Options& options);

    /// `coded` is the encoder's Annex B output for one picture, `idr` whether
    /// it was asked for an IDR (checked against the slices). `fallback_qp`
    /// is reported when the slice QP cannot be read.
    [[nodiscard]] Result<EncodedFrame> finish(std::span<const std::byte> coded, bool idr, std::uint8_t fallback_qp);

    /// The parameter sets seen last (as written to the stream), for tests.
    [[nodiscard]] std::span<const std::byte> sps() const noexcept { return sps_; }
    [[nodiscard]] std::span<const std::byte> pps() const noexcept { return pps_; }

private:
    Options options_;
    std::vector<std::byte> sps_;
    std::vector<std::byte> pps_;
    std::optional<SpsInfo> sps_info_;
    std::optional<PpsInfo> pps_info_;
};

}  // namespace farland::video::bitstream
