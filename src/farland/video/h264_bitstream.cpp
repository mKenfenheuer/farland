// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/codec/h264_nal.hpp>
#include <farland/video/h264_bitstream.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace farland::video::bitstream {

namespace {

namespace nal_type = codec::h264::nal_type;

constexpr std::uint8_t nal_ref_idc_high = 3 << 5;
constexpr std::uint8_t profile_idc_baseline = 66;
constexpr std::uint8_t profile_idc_main = 77;
constexpr std::uint8_t profile_idc_high = 100;
/// A slice header fits in far fewer bytes; only these are unescaped.
constexpr std::size_t slice_header_bytes = 64;
constexpr std::array<std::byte, 4> start_code{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};

[[nodiscard]] Error truncated()
{
    return Error{Errc::truncated, "H.264 header ends early"};
}

/// Profiles whose SPS carries chroma_format_idc and the bit depths
/// ([ITU-H.264-201201] 7.3.2.1.1).
[[nodiscard]] constexpr bool has_chroma_info(std::uint32_t profile_idc) noexcept
{
    constexpr std::array<std::uint32_t, 13> profiles{100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135};
    return std::ranges::find(profiles, profile_idc) != profiles.end();
}

[[nodiscard]] std::uint8_t nal_header(std::span<const std::byte> nal)
{
    return nal.empty() ? std::uint8_t{0} : std::to_integer<std::uint8_t>(nal.front());
}

/// vui_parameters() ([ITU-H.264-201201] E.1.1).
void write_vui(BitWriter& w, const Vui& vui)
{
    constexpr std::uint32_t video_format_unspecified = 5;
    constexpr std::uint32_t primaries_bt709 = 1;
    constexpr std::uint32_t transfer_srgb = 13;  // IEC 61966-2-1, like the other backends.
    constexpr std::uint32_t matrix_bt709 = 1;
    constexpr std::uint32_t matrix_smpte170m = 6;
    constexpr std::uint32_t log2_max_mv_length = 15;

    w.flag(false);  // aspect_ratio_info_present_flag
    w.flag(false);  // overscan_info_present_flag
    w.flag(true);   // video_signal_type_present_flag
    w.bits(video_format_unspecified, 3);
    w.flag(vui.color.full_range);
    w.flag(true);  // colour_description_present_flag
    w.bits(primaries_bt709, 8);
    w.bits(transfer_srgb, 8);
    w.bits(vui.color.matrix == ColorSpace::Matrix::bt709 ? matrix_bt709 : matrix_smpte170m, 8);
    w.flag(false);         // chroma_loc_info_present_flag
    w.flag(vui.fps != 0);  // timing_info_present_flag
    if (vui.fps != 0) {
        w.bits(1, 32);             // num_units_in_tick
        w.bits(2U * vui.fps, 32);  // time_scale: two ticks per frame (E.2.1)
        w.flag(false);             // fixed_frame_rate_flag
    }
    w.flag(false);  // nal_hrd_parameters_present_flag
    w.flag(false);  // vcl_hrd_parameters_present_flag
    w.flag(false);  // pic_struct_present_flag
    w.flag(true);   // bitstream_restriction_flag
    w.flag(true);   // motion_vectors_over_pic_boundaries_flag
    w.ue(2);        // max_bytes_per_pic_denom (the default)
    w.ue(1);        // max_bits_per_mb_denom (the default)
    w.ue(log2_max_mv_length);
    w.ue(log2_max_mv_length);
    w.ue(0);  // max_num_reorder_frames: no picture waits for a later one
    w.ue(vui.max_dec_frame_buffering);
}

[[nodiscard]] std::vector<std::byte> finish_nal(std::uint8_t header, BitWriter& w)
{
    w.trailing_bits();
    std::vector<std::byte> nal{std::byte{header}};
    const auto payload = escape(w.bytes());
    nal.insert(nal.end(), payload.begin(), payload.end());
    return nal;
}

/// ref_pic_list_modification() for list 0 ([ITU-H.264-201201] 7.3.3.1).
[[nodiscard]] Result<void> skip_ref_pic_list_modification(BitReader& r)
{
    FARLAND_TRY(const bool present, r.flag());
    if (!present) {
        return {};
    }
    for (int i = 0; i <= 32; ++i) {
        FARLAND_TRY(const std::uint32_t idc, r.ue());
        if (idc == 3) {
            return {};
        }
        if (idc > 5) {
            return fail(Errc::invalid_value, "H.264 modification_of_pic_nums_idc is out of range");
        }
        FARLAND_TRY_VOID(r.ue());
    }
    return fail(Errc::invalid_value, "H.264 reference list modification does not end");
}

/// dec_ref_pic_marking() ([ITU-H.264-201201] 7.3.3.3).
[[nodiscard]] Result<void> skip_dec_ref_pic_marking(BitReader& r, bool idr)
{
    if (idr) {
        FARLAND_TRY_VOID(r.flag());  // no_output_of_prior_pics_flag
        FARLAND_TRY_VOID(r.flag());  // long_term_reference_flag
        return {};
    }
    FARLAND_TRY(const bool adaptive, r.flag());
    if (!adaptive) {
        return {};
    }
    for (int i = 0; i <= 66; ++i) {
        FARLAND_TRY(const std::uint32_t operation, r.ue());
        if (operation == 0) {
            return {};
        }
        if (operation > 6) {
            return fail(Errc::invalid_value, "H.264 memory_management_control_operation is out of range");
        }
        if (operation == 1 || operation == 3) {
            FARLAND_TRY_VOID(r.ue());  // difference_of_pic_nums_minus1
        }
        if (operation == 2) {
            FARLAND_TRY_VOID(r.ue());  // long_term_pic_num
        }
        if (operation == 3 || operation == 6) {
            FARLAND_TRY_VOID(r.ue());  // long_term_frame_idx
        }
        if (operation == 4) {
            FARLAND_TRY_VOID(r.ue());  // max_long_term_frame_idx_plus1
        }
    }
    return fail(Errc::invalid_value, "H.264 reference marking does not end");
}

void append_nal(std::vector<std::byte>& out, std::span<const std::byte> nal)
{
    out.insert(out.end(), start_code.begin(), start_code.end());
    out.insert(out.end(), nal.begin(), nal.end());
}

}  // namespace

void BitWriter::bits(std::uint32_t value, unsigned count)
{
    FARLAND_ASSERT(count <= 32);
    for (unsigned i = count; i-- > 0;) {
        current_ = (current_ << 1U) | ((value >> i) & 1U);
        if (++filled_ == 8) {
            bytes_.push_back(static_cast<std::byte>(current_));
            current_ = 0;
            filled_ = 0;
        }
    }
}

void BitWriter::ue(std::uint32_t value)
{
    const std::uint64_t code = std::uint64_t{value} + 1U;
    const auto length = static_cast<unsigned>(std::bit_width(code));
    // length - 1 leading zeros, then `code` in `length` bits (up to 33).
    for (unsigned i = 1; i < length; ++i) {
        bits(0, 1);
    }
    if (length > 32) {
        bits(1, 1);
        bits(static_cast<std::uint32_t>(code), 32);
    } else {
        bits(static_cast<std::uint32_t>(code), length);
    }
}

void BitWriter::se(std::int32_t value)
{
    const std::int64_t v = value;
    ue(static_cast<std::uint32_t>(v > 0 ? (2 * v) - 1 : -2 * v));
}

void BitWriter::trailing_bits()
{
    bits(1, 1);
    while (filled_ != 0) {
        bits(0, 1);
    }
}

Result<std::uint32_t> BitReader::bits(unsigned count)
{
    FARLAND_ASSERT(count <= 32);
    if (position_ + count > data_.size() * 8U) {
        return std::unexpected(truncated());
    }
    std::uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto byte = std::to_integer<std::uint32_t>(data_[position_ / 8U]);
        value = (value << 1U) | ((byte >> (7U - (position_ % 8U))) & 1U);
        ++position_;
    }
    return value;
}

Result<bool> BitReader::flag()
{
    FARLAND_TRY(const std::uint32_t bit, bits(1));
    return bit != 0;
}

Result<std::uint32_t> BitReader::ue()
{
    unsigned zeros = 0;
    for (;;) {
        FARLAND_TRY(const bool one, flag());
        if (one) {
            break;
        }
        if (++zeros > 31) {
            return fail(Errc::invalid_value, "H.264 Exp-Golomb code is too long");
        }
    }
    FARLAND_TRY(const std::uint32_t suffix, bits(zeros));
    return static_cast<std::uint32_t>((std::uint64_t{1} << zeros) - 1U + suffix);
}

Result<std::int32_t> BitReader::se()
{
    FARLAND_TRY(const std::uint32_t code, ue());
    const std::int64_t magnitude = (std::int64_t{code} + 1) / 2;
    return static_cast<std::int32_t>(code % 2 == 1 ? magnitude : -magnitude);
}

std::vector<std::byte> unescape(std::span<const std::byte> payload)
{
    std::vector<std::byte> rbsp;
    rbsp.reserve(payload.size());
    unsigned zeros = 0;
    for (const std::byte b : payload) {
        if (zeros >= 2 && b == std::byte{3}) {
            zeros = 0;
            continue;
        }
        rbsp.push_back(b);
        zeros = b == std::byte{0} ? zeros + 1 : 0;
    }
    return rbsp;
}

std::vector<std::byte> escape(std::span<const std::byte> rbsp)
{
    std::vector<std::byte> payload;
    payload.reserve(rbsp.size() + (rbsp.size() / 64));
    unsigned zeros = 0;
    for (const std::byte b : rbsp) {
        if (zeros >= 2 && std::to_integer<unsigned>(b) <= 3) {
            payload.push_back(std::byte{3});
            zeros = 0;
        }
        payload.push_back(b);
        zeros = b == std::byte{0} ? zeros + 1 : 0;
    }
    return payload;
}

std::vector<std::byte> write_sps(const SpsParams& params)
{
    FARLAND_ASSERT(params.width_mbs > 0 && params.height_mbs > 0);
    FARLAND_ASSERT(params.log2_max_frame_num >= 4 && params.log2_max_frame_num <= 16);
    FARLAND_ASSERT(params.log2_max_poc_lsb >= 4 && params.log2_max_poc_lsb <= 16);
    BitWriter w;
    std::uint8_t profile_idc = profile_idc_baseline;
    bool constraint_set0 = false;
    bool constraint_set1 = false;
    switch (params.profile) {
    case Profile::constrained_baseline:
        // Constrained Baseline is Baseline that also obeys Main (A.2.1.1).
        constraint_set0 = constraint_set1 = true;
        break;
    case Profile::main:
        profile_idc = profile_idc_main;
        constraint_set1 = true;
        break;
    case Profile::high:
        profile_idc = profile_idc_high;
        break;
    }
    // seq_parameter_set_data() ([ITU-H.264-201201] 7.3.2.1.1)
    w.bits(profile_idc, 8);
    w.flag(constraint_set0);
    w.flag(constraint_set1);
    w.bits(0, 6);  // constraint_set2..5_flag, reserved_zero_2bits
    w.bits(params.level_idc, 8);
    w.ue(0);  // seq_parameter_set_id
    if (has_chroma_info(profile_idc)) {
        w.ue(1);        // chroma_format_idc: 4:2:0
        w.ue(0);        // bit_depth_luma_minus8
        w.ue(0);        // bit_depth_chroma_minus8
        w.flag(false);  // qpprime_y_zero_transform_bypass_flag
        w.flag(false);  // seq_scaling_matrix_present_flag
    }
    w.ue(params.log2_max_frame_num - 4U);
    w.ue(0);  // pic_order_cnt_type
    w.ue(params.log2_max_poc_lsb - 4U);
    w.ue(params.max_num_ref_frames);
    w.flag(false);  // gaps_in_frame_num_value_allowed_flag
    w.ue(params.width_mbs - 1U);
    w.ue(params.height_mbs - 1U);  // pic_height_in_map_units_minus1 (frames only)
    w.flag(true);                  // frame_mbs_only_flag
    w.flag(true);                  // direct_8x8_inference_flag
    w.flag(false);                 // frame_cropping_flag
    w.flag(true);                  // vui_parameters_present_flag
    Vui vui = params.vui;
    vui.max_dec_frame_buffering = std::max(vui.max_dec_frame_buffering, params.max_num_ref_frames);
    write_vui(w, vui);
    return finish_nal(nal_ref_idc_high | nal_type::sps, w);
}

std::vector<std::byte> write_pps(const PpsParams& params)
{
    BitWriter w;
    // pic_parameter_set_rbsp() ([ITU-H.264-201201] 7.3.2.2)
    w.ue(0);                                                   // pic_parameter_set_id
    w.ue(0);                                                   // seq_parameter_set_id
    w.flag(params.profile != Profile::constrained_baseline);   // entropy_coding_mode_flag
    w.flag(false);                                             // bottom_field_pic_order_in_frame_present_flag
    w.ue(0);                                                   // num_slice_groups_minus1
    w.ue(0);                                                   // num_ref_idx_l0_default_active_minus1
    w.ue(0);                                                   // num_ref_idx_l1_default_active_minus1
    w.flag(false);                                             // weighted_pred_flag
    w.bits(0, 2);                                              // weighted_bipred_idc
    w.se(static_cast<std::int32_t>(params.pic_init_qp) - 26);  // pic_init_qp_minus26
    w.se(0);                                                   // pic_init_qs_minus26
    w.se(0);                                                   // chroma_qp_index_offset
    w.flag(true);                                              // deblocking_filter_control_present_flag
    w.flag(false);                                             // constrained_intra_pred_flag
    w.flag(false);                                             // redundant_pic_cnt_present_flag
    if (params.profile == Profile::high) {
        w.flag(true);   // transform_8x8_mode_flag
        w.flag(false);  // pic_scaling_matrix_present_flag
        w.se(0);        // second_chroma_qp_index_offset
    }
    return finish_nal(nal_ref_idc_high | nal_type::pps, w);
}

PackedSliceHeader write_slice_header(const SliceHeaderParams& params)
{
    BitWriter w;
    // Every picture is a reference for the next one.
    w.bits(nal_ref_idc_high | (params.idr ? nal_type::idr : nal_type::slice), 8);
    w.ue(0);                   // first_mb_in_slice
    w.ue(params.idr ? 7 : 5);  // slice_type: I or P, the same for all slices of the picture
    w.ue(0);                   // pic_parameter_set_id
    w.bits(params.frame_num % (1U << params.log2_max_frame_num), params.log2_max_frame_num);
    if (params.idr) {
        w.ue(params.idr_pic_id);
    }
    w.bits(params.poc_lsb % (1U << params.log2_max_poc_lsb), params.log2_max_poc_lsb);
    if (!params.idr) {
        w.flag(false);  // num_ref_idx_active_override_flag: one reference, as in the PPS
        w.flag(false);  // ref_pic_list_modification_flag_l0
    }
    // dec_ref_pic_marking() (7.3.3.3)
    w.flag(false);  // IDR: no_output_of_prior_pics_flag; P: adaptive_ref_pic_marking_mode_flag
    if (params.idr) {
        w.flag(false);  // long_term_reference_flag
    }
    if (params.profile != Profile::constrained_baseline && !params.idr) {
        w.ue(0);  // cabac_init_idc
    }
    w.se(params.qp_delta);
    // deblocking_filter_control_present_flag is set in our PPS.
    w.ue(0);  // disable_deblocking_filter_idc
    w.se(0);  // slice_alpha_c0_offset_div2
    w.se(0);  // slice_beta_offset_div2

    // Escape the bits written so far; the last partial byte is padded with
    // zeros that do not count.
    const std::size_t bits = w.bit_count();
    const std::size_t padding = (8U - (bits % 8U)) % 8U;
    w.bits(0, static_cast<unsigned>(padding));
    const auto& raw = w.bytes();
    PackedSliceHeader header;
    header.bytes.push_back(raw.front());
    const auto escaped = escape(std::span(raw).subspan(1));
    header.bytes.insert(header.bytes.end(), escaped.begin(), escaped.end());
    header.bits = (header.bytes.size() * 8U) - padding;
    return header;
}

Result<SpsInfo> parse_sps(std::span<const std::byte> nal)
{
    if ((nal_header(nal) & 0x1FU) != nal_type::sps) {
        return fail(Errc::invalid_value, "not an H.264 SPS");
    }
    const auto rbsp = unescape(nal.subspan(1));
    BitReader r(rbsp);
    SpsInfo info;
    FARLAND_TRY(const std::uint32_t profile_idc, r.bits(8));
    FARLAND_TRY_VOID(r.bits(8));  // constraint flags
    FARLAND_TRY(const std::uint32_t level_idc, r.bits(8));
    info.profile_idc = static_cast<std::uint8_t>(profile_idc);
    info.level_idc = static_cast<std::uint8_t>(level_idc);
    FARLAND_TRY(const std::uint32_t sps_id, r.ue());
    if (sps_id > 31) {
        return fail(Errc::invalid_value, "H.264 seq_parameter_set_id is out of range");
    }
    if (has_chroma_info(profile_idc)) {
        FARLAND_TRY(const std::uint32_t chroma_format_idc, r.ue());
        if (chroma_format_idc == 3) {
            FARLAND_TRY_VOID(r.flag());  // separate_colour_plane_flag
        }
        FARLAND_TRY_VOID(r.ue());    // bit_depth_luma_minus8
        FARLAND_TRY_VOID(r.ue());    // bit_depth_chroma_minus8
        FARLAND_TRY_VOID(r.flag());  // qpprime_y_zero_transform_bypass_flag
        FARLAND_TRY(const bool scaling, r.flag());
        if (scaling) {
            return fail(Errc::unsupported, "H.264 SPS with scaling matrices");
        }
    }
    FARLAND_TRY(const std::uint32_t log2_max_frame_num_minus4, r.ue());
    if (log2_max_frame_num_minus4 > 12) {
        return fail(Errc::invalid_value, "H.264 log2_max_frame_num is out of range");
    }
    info.log2_max_frame_num = log2_max_frame_num_minus4 + 4U;
    FARLAND_TRY(info.pic_order_cnt_type, r.ue());
    if (info.pic_order_cnt_type == 0) {
        FARLAND_TRY(const std::uint32_t log2_max_poc_lsb_minus4, r.ue());
        if (log2_max_poc_lsb_minus4 > 12) {
            return fail(Errc::invalid_value, "H.264 log2_max_pic_order_cnt_lsb is out of range");
        }
        info.log2_max_poc_lsb = log2_max_poc_lsb_minus4 + 4U;
    } else if (info.pic_order_cnt_type == 1) {
        FARLAND_TRY(info.delta_pic_order_always_zero, r.flag());
        FARLAND_TRY_VOID(r.se());  // offset_for_non_ref_pic
        FARLAND_TRY_VOID(r.se());  // offset_for_top_to_bottom_field
        FARLAND_TRY(const std::uint32_t cycle, r.ue());
        if (cycle > 255) {
            return fail(Errc::invalid_value, "H.264 num_ref_frames_in_pic_order_cnt_cycle is out of range");
        }
        for (std::uint32_t i = 0; i < cycle; ++i) {
            FARLAND_TRY_VOID(r.se());
        }
    } else if (info.pic_order_cnt_type != 2) {
        return fail(Errc::invalid_value, "H.264 pic_order_cnt_type is out of range");
    }
    FARLAND_TRY(info.max_num_ref_frames, r.ue());
    FARLAND_TRY_VOID(r.flag());  // gaps_in_frame_num_value_allowed_flag
    FARLAND_TRY(const std::uint32_t width_minus1, r.ue());
    FARLAND_TRY(const std::uint32_t height_minus1, r.ue());
    FARLAND_TRY(info.frame_mbs_only, r.flag());
    if (width_minus1 >= 1024 || height_minus1 >= 1024) {
        return fail(Errc::invalid_value, "H.264 picture size is out of range");
    }
    info.width_mbs = width_minus1 + 1U;
    info.height_mbs = (height_minus1 + 1U) * (info.frame_mbs_only ? 1U : 2U);
    if (!info.frame_mbs_only) {
        FARLAND_TRY_VOID(r.flag());  // mb_adaptive_frame_field_flag
    }
    FARLAND_TRY_VOID(r.flag());  // direct_8x8_inference_flag
    FARLAND_TRY(const bool cropping, r.flag());
    if (cropping) {
        for (int i = 0; i < 4; ++i) {
            FARLAND_TRY_VOID(r.ue());
        }
    }
    info.vui_flag_position = r.position();
    FARLAND_TRY(info.vui_present, r.flag());
    return info;
}

Result<PpsInfo> parse_pps(std::span<const std::byte> nal)
{
    if ((nal_header(nal) & 0x1FU) != nal_type::pps) {
        return fail(Errc::invalid_value, "not an H.264 PPS");
    }
    const auto rbsp = unescape(nal.subspan(1));
    BitReader r(rbsp);
    PpsInfo info;
    FARLAND_TRY_VOID(r.ue());  // pic_parameter_set_id
    FARLAND_TRY_VOID(r.ue());  // seq_parameter_set_id
    FARLAND_TRY(info.entropy_coding_mode, r.flag());
    FARLAND_TRY(info.bottom_field_pic_order_in_frame_present, r.flag());
    FARLAND_TRY(const std::uint32_t slice_groups_minus1, r.ue());
    if (slice_groups_minus1 != 0) {
        return fail(Errc::unsupported, "H.264 PPS with slice groups");
    }
    FARLAND_TRY_VOID(r.ue());  // num_ref_idx_l0_default_active_minus1
    FARLAND_TRY_VOID(r.ue());  // num_ref_idx_l1_default_active_minus1
    FARLAND_TRY(info.weighted_pred, r.flag());
    FARLAND_TRY(info.weighted_bipred_idc, r.bits(2));
    FARLAND_TRY(const std::int32_t qp_minus26, r.se());
    if (qp_minus26 < -26 || qp_minus26 > 25) {
        return fail(Errc::invalid_value, "H.264 pic_init_qp is out of range");
    }
    info.pic_init_qp = qp_minus26 + 26;
    FARLAND_TRY_VOID(r.se());    // pic_init_qs_minus26
    FARLAND_TRY_VOID(r.se());    // chroma_qp_index_offset
    FARLAND_TRY_VOID(r.flag());  // deblocking_filter_control_present_flag
    FARLAND_TRY_VOID(r.flag());  // constrained_intra_pred_flag
    FARLAND_TRY(info.redundant_pic_cnt_present, r.flag());
    return info;
}

Result<std::vector<std::byte>> replace_vui(std::span<const std::byte> nal, Vui vui)
{
    FARLAND_TRY(const SpsInfo info, parse_sps(nal));
    const auto rbsp = unescape(nal.subspan(1));
    BitReader copy(rbsp);
    BitWriter w;
    for (std::size_t left = info.vui_flag_position; left > 0;) {
        const auto count = static_cast<unsigned>(std::min<std::size_t>(left, 32));
        FARLAND_TRY(const std::uint32_t chunk, copy.bits(count));
        w.bits(chunk, count);
        left -= count;
    }
    w.flag(true);  // vui_parameters_present_flag
    vui.max_dec_frame_buffering = std::max(vui.max_dec_frame_buffering, info.max_num_ref_frames);
    write_vui(w, vui);
    return finish_nal(nal_header(nal), w);
}

Result<std::uint8_t> slice_qp(std::span<const std::byte> nal, const SpsInfo& sps, const PpsInfo& pps)
{
    constexpr std::uint32_t slice_p = 0;
    constexpr std::uint32_t slice_b = 1;
    constexpr std::uint32_t slice_i = 2;
    constexpr std::uint32_t slice_sp = 3;
    constexpr std::uint32_t slice_si = 4;

    const std::uint8_t header = nal_header(nal);
    const std::uint8_t type = header & 0x1FU;
    if (type != nal_type::slice && type != nal_type::idr) {
        return fail(Errc::invalid_value, "not an H.264 slice");
    }
    const bool idr = type == nal_type::idr;
    const auto rbsp = unescape(nal.subspan(1, std::min(nal.size() - 1, slice_header_bytes)));
    BitReader r(rbsp);
    // slice_header() ([ITU-H.264-201201] 7.3.3)
    FARLAND_TRY_VOID(r.ue());  // first_mb_in_slice
    FARLAND_TRY(const std::uint32_t slice_type_code, r.ue());
    const std::uint32_t slice_type = slice_type_code % 5;
    if (slice_type_code > 9) {
        return fail(Errc::invalid_value, "H.264 slice_type is out of range");
    }
    if (slice_type == slice_b) {
        return fail(Errc::unsupported, "H.264 B slices");
    }
    FARLAND_TRY_VOID(r.ue());  // pic_parameter_set_id
    FARLAND_TRY_VOID(r.bits(sps.log2_max_frame_num));
    bool field_pic = false;
    if (!sps.frame_mbs_only) {
        FARLAND_TRY(field_pic, r.flag());
        if (field_pic) {
            FARLAND_TRY_VOID(r.flag());  // bottom_field_flag
        }
    }
    if (idr) {
        FARLAND_TRY_VOID(r.ue());  // idr_pic_id
    }
    if (sps.pic_order_cnt_type == 0) {
        FARLAND_TRY_VOID(r.bits(sps.log2_max_poc_lsb));
        if (pps.bottom_field_pic_order_in_frame_present && !field_pic) {
            FARLAND_TRY_VOID(r.se());  // delta_pic_order_cnt_bottom
        }
    }
    if (sps.pic_order_cnt_type == 1 && !sps.delta_pic_order_always_zero) {
        FARLAND_TRY_VOID(r.se());
        if (pps.bottom_field_pic_order_in_frame_present && !field_pic) {
            FARLAND_TRY_VOID(r.se());
        }
    }
    if (pps.redundant_pic_cnt_present) {
        FARLAND_TRY_VOID(r.ue());
    }
    const bool predicted = slice_type == slice_p || slice_type == slice_sp;
    if (predicted) {
        FARLAND_TRY(const bool override_refs, r.flag());
        if (override_refs) {
            FARLAND_TRY_VOID(r.ue());  // num_ref_idx_l0_active_minus1
        }
        FARLAND_TRY_VOID(skip_ref_pic_list_modification(r));
        if (pps.weighted_pred) {
            return fail(Errc::unsupported, "H.264 explicit weighted prediction");
        }
    } else if (slice_type != slice_i && slice_type != slice_si) {
        return fail(Errc::invalid_value, "H.264 slice_type is out of range");
    }
    if ((header & 0x60U) != 0) {
        FARLAND_TRY_VOID(skip_dec_ref_pic_marking(r, idr));
    }
    if (pps.entropy_coding_mode && predicted) {
        FARLAND_TRY_VOID(r.ue());  // cabac_init_idc
    }
    FARLAND_TRY(const std::int32_t delta, r.se());
    const std::int32_t qp = pps.pic_init_qp + delta;
    if (qp < 0 || qp > 51) {
        return fail(Errc::invalid_value, "H.264 slice QP is out of range");
    }
    return static_cast<std::uint8_t>(qp);
}

std::uint8_t level_for(std::uint32_t width_mbs, std::uint32_t height_mbs, std::uint32_t fps, std::uint32_t max_kbps,
                       Profile profile) noexcept
{
    struct Level {
        std::uint8_t idc;
        std::uint32_t max_mbps;  // macroblocks per second
        std::uint32_t max_fs;    // macroblocks per frame
        std::uint32_t max_kbps;  // MaxBR for Baseline and Main (1000 bit/s)
    };
    // [ITU-H.264-201201] Table A-1 (level 1b left out).
    constexpr std::array<Level, 19> levels{{
        {10, 1485, 99, 64},
        {11, 3000, 396, 192},
        {12, 6000, 396, 384},
        {13, 11880, 396, 768},
        {20, 11880, 396, 2000},
        {21, 19800, 792, 4000},
        {22, 20250, 1620, 4000},
        {30, 40500, 1620, 10000},
        {31, 108000, 3600, 14000},
        {32, 216000, 5120, 20000},
        {40, 245760, 8192, 20000},
        {41, 245760, 8192, 50000},
        {42, 522240, 8704, 50000},
        {50, 589824, 22080, 135000},
        {51, 983040, 36864, 240000},
        {52, 2073600, 36864, 240000},
        {60, 4177920, 139264, 240000},
        {61, 8355840, 139264, 480000},
        {62, 16711680, 139264, 800000},
    }};
    const std::uint64_t frame_size = std::uint64_t{width_mbs} * height_mbs;
    // High allows 1.25 times the bitrate (Table A-2, cpbBrVclFactor).
    const std::uint64_t kbps = profile == Profile::high ? (std::uint64_t{max_kbps} * 4U + 4U) / 5U : max_kbps;
    for (const Level& level : levels) {
        // A side may not exceed sqrt(8 * MaxFS) macroblocks (A.3.1 f, g).
        const std::uint64_t side_limit = std::uint64_t{level.max_fs} * 8U;
        if (frame_size <= level.max_fs && frame_size * fps <= level.max_mbps &&
            std::uint64_t{width_mbs} * width_mbs <= side_limit &&
            std::uint64_t{height_mbs} * height_mbs <= side_limit && kbps <= level.max_kbps) {
            return level.idc;
        }
    }
    return levels.back().idc;
}

void AccessUnitWriter::reset(const Options& options)
{
    options_ = options;
    sps_.clear();
    pps_.clear();
    sps_info_.reset();
    pps_info_.reset();
}

Result<EncodedFrame> AccessUnitWriter::finish(std::span<const std::byte> coded, bool idr, std::uint8_t fallback_qp)
{
    FARLAND_TRY(const auto units, codec::h264::split_annex_b(coded));
    bool has_sps = false;
    bool has_pps = false;
    std::vector<std::span<const std::byte>> slices;
    for (const auto& unit : units) {
        switch (unit.type) {
        case nal_type::sps: {
            FARLAND_TRY(sps_, replace_vui(unit.data, options_.vui));
            FARLAND_TRY(sps_info_, parse_sps(sps_));
            has_sps = true;
            break;
        }
        case nal_type::pps: {
            FARLAND_TRY(pps_info_, parse_pps(unit.data));
            pps_.assign(unit.data.begin(), unit.data.end());
            has_pps = true;
            break;
        }
        case nal_type::slice:
        case nal_type::idr:
            slices.push_back(unit.data);
            break;
        default:
            break;  // AUD, SEI, filler, end of sequence: see the class comment.
        }
    }
    if (slices.empty()) {
        return fail(Errc::io, "the H.264 encoder produced no slices");
    }
    if (codec::h264::contains_idr(units) != idr) {
        return fail(Errc::io, "the H.264 encoder did not code the picture type it was asked for");
    }
    if (idr && (sps_.empty() || pps_.empty())) {
        return fail(Errc::io, "the H.264 encoder wrote no SPS and PPS before the first IDR picture");
    }

    EncodedFrame frame;
    frame.idr = idr;
    frame.bitstream.reserve(coded.size() + 64U);
    if (options_.access_unit_delimiters) {
        // access_unit_delimiter_rbsp ([ITU-H.264-201201] 7.3.2.4): primary_pic_type
        // 0 (I slices) or 1 (I and P slices), then the RBSP stop bit.
        append_nal(frame.bitstream, std::array{std::byte{nal_type::aud}, idr ? std::byte{0x10} : std::byte{0x30}});
    }
    if (idr || has_sps) {
        append_nal(frame.bitstream, sps_);
    }
    if (idr || has_pps) {
        append_nal(frame.bitstream, pps_);
    }
    for (const auto slice : slices) {
        append_nal(frame.bitstream, slice);
    }
    frame.qp = fallback_qp;
    if (sps_info_ && pps_info_) {
        if (const auto qp = slice_qp(slices.front(), *sps_info_, *pps_info_)) {
            frame.qp = *qp;
        }
    }
    return frame;
}

}  // namespace farland::video::bitstream
