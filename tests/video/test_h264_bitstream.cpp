// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The H.264 header code behind the hardware encoders: bit I/O, parameter
// sets, VUI replacement, slice QPs and access unit assembly.

#include <farland/base/error.hpp>
#include <farland/codec/h264_nal.hpp>
#include <farland/video/h264_bitstream.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace video = farland::video;
namespace bits = farland::video::bitstream;
namespace h264 = farland::codec::h264;
using farland::Errc;

namespace {

std::vector<std::byte> annex_b(std::initializer_list<std::span<const std::byte>> nals)
{
    std::vector<std::byte> out;
    for (const auto nal : nals) {
        out.insert(out.end(), {std::byte{0}, std::byte{0}, std::byte{1}});
        out.insert(out.end(), nal.begin(), nal.end());
    }
    return out;
}

/// A slice NAL unit as a driver would write it for our SPS and PPS.
std::vector<std::byte> slice(bool idr, std::uint32_t frame_num, std::int32_t qp_delta, bool cabac,
                             std::uint32_t log2_max_frame_num = 16, std::uint32_t log2_max_poc_lsb = 16)
{
    bits::BitWriter w;
    w.ue(0);            // first_mb_in_slice
    w.ue(idr ? 7 : 5);  // slice_type I or P (all slices of the picture)
    w.ue(0);            // pic_parameter_set_id
    w.bits(frame_num, log2_max_frame_num);
    if (idr) {
        w.ue(3);  // idr_pic_id
    }
    w.bits(2 * frame_num, log2_max_poc_lsb);
    if (!idr) {
        w.flag(false);  // num_ref_idx_active_override_flag
        w.flag(false);  // ref_pic_list_modification_flag_l0
    }
    if (idr) {
        w.flag(false);  // no_output_of_prior_pics_flag
        w.flag(false);  // long_term_reference_flag
    } else {
        w.flag(false);  // adaptive_ref_pic_marking_mode_flag
    }
    if (cabac && !idr) {
        w.ue(0);  // cabac_init_idc
    }
    w.se(qp_delta);
    w.bits(0xA5, 8);  // stands in for the slice data
    w.trailing_bits();
    std::vector<std::byte> nal{std::byte{static_cast<std::uint8_t>(0x60U | (idr ? 5U : 1U))}};
    const auto payload = bits::escape(w.bytes());
    nal.insert(nal.end(), payload.begin(), payload.end());
    return nal;
}

bits::SpsParams sps_params(video::Profile profile)
{
    return {.profile = profile,
            .level_idc = 40,
            .width_mbs = 120,
            .height_mbs = 68,
            .log2_max_frame_num = 16,
            .log2_max_poc_lsb = 16,
            .max_num_ref_frames = 1,
            .vui = {.color = {}, .fps = 30, .max_dec_frame_buffering = 1}};
}

}  // namespace

TEST_CASE("Exp-Golomb codes round-trip")
{
    bits::BitWriter w;
    const std::array<std::uint32_t, 9> unsigned_values{0, 1, 2, 3, 7, 8, 255, 65535, 0xFFFF'FFFEU};
    const std::array<std::int32_t, 7> signed_values{0, 1, -1, 2, -2, 1000, -32768};
    for (const auto v : unsigned_values) {
        w.ue(v);
    }
    for (const auto v : signed_values) {
        w.se(v);
    }
    w.bits(0x5, 3);
    w.trailing_bits();
    CHECK(w.bit_count() % 8 == 0);

    bits::BitReader r(w.bytes());
    for (const auto v : unsigned_values) {
        CHECK(r.ue() == v);
    }
    for (const auto v : signed_values) {
        CHECK(r.se() == v);
    }
    CHECK(r.bits(3) == 5U);
    CHECK(r.flag() == true);  // the stop bit

    // ue(0) is the single bit 1; ue(3) is 00100.
    bits::BitWriter small;
    small.ue(0);
    small.ue(3);
    small.bits(0, 2);
    REQUIRE(small.bytes().size() == 1);
    CHECK(small.bytes()[0] == std::byte{0x90});  // 1 00100 00

    bits::BitReader empty(std::span<const std::byte>{});
    CHECK(empty.ue().error().code == Errc::truncated);
    const std::array<std::byte, 4> zeros{};
    bits::BitReader too_long(zeros);
    CHECK(too_long.ue().error().code == Errc::invalid_value);
}

TEST_CASE("Emulation prevention round-trips")
{
    const std::vector<std::byte> rbsp{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
                                      std::byte{0}, std::byte{0}, std::byte{3}, std::byte{0}, std::byte{0},
                                      std::byte{2}, std::byte{0}, std::byte{0}, std::byte{4}};
    const auto payload = bits::escape(rbsp);
    CHECK(payload == std::vector<std::byte>{std::byte{0}, std::byte{0}, std::byte{3}, std::byte{0}, std::byte{0},
                                            std::byte{3}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{3},
                                            std::byte{3}, std::byte{0}, std::byte{0}, std::byte{3}, std::byte{2},
                                            std::byte{0}, std::byte{0}, std::byte{4}});
    CHECK(bits::unescape(payload) == rbsp);
}

TEST_CASE("SPS and PPS are written and read back")
{
    for (const auto profile : {video::Profile::constrained_baseline, video::Profile::main, video::Profile::high}) {
        CAPTURE(static_cast<int>(profile));
        const auto sps = bits::write_sps(sps_params(profile));
        CHECK(sps.front() == std::byte{0x67});
        const auto info = bits::parse_sps(sps);
        REQUIRE(info.has_value());
        CHECK(info->profile_idc == (profile == video::Profile::constrained_baseline ? 66
                                    : profile == video::Profile::main               ? 77
                                                                                    : 100));
        CHECK(info->level_idc == 40);
        CHECK(info->width_mbs == 120);
        CHECK(info->height_mbs == 68);
        CHECK(info->log2_max_frame_num == 16);
        CHECK(info->pic_order_cnt_type == 0);
        CHECK(info->log2_max_poc_lsb == 16);
        CHECK(info->max_num_ref_frames == 1);
        CHECK(info->frame_mbs_only);
        CHECK(info->vui_present);

        const auto pps = bits::write_pps({.profile = profile, .pic_init_qp = 30});
        CHECK(pps.front() == std::byte{0x68});
        const auto pps_info = bits::parse_pps(pps);
        REQUIRE(pps_info.has_value());
        CHECK(pps_info->pic_init_qp == 30);
        CHECK(pps_info->entropy_coding_mode == (profile != video::Profile::constrained_baseline));
        CHECK_FALSE(pps_info->weighted_pred);
    }
    CHECK(bits::parse_sps(bits::write_pps({})).error().code == Errc::invalid_value);
    auto truncated = bits::write_sps(sps_params(video::Profile::high));
    truncated.resize(6);
    CHECK_FALSE(bits::parse_sps(truncated).has_value());
}

TEST_CASE("The VUI of an SPS is replaced bit-exactly")
{
    auto params = sps_params(video::Profile::main);
    params.vui.color = {.matrix = video::ColorSpace::Matrix::bt601, .full_range = false};
    params.vui.fps = 0;
    const auto limited = bits::write_sps(params);

    const bits::Vui ours{.color = {}, .fps = 30, .max_dec_frame_buffering = 1};
    const auto replaced = bits::replace_vui(limited, ours);
    REQUIRE(replaced.has_value());
    // Everything before the VUI is the same, so it equals an SPS written with our VUI.
    CHECK(*replaced == bits::write_sps(sps_params(video::Profile::main)));
    CHECK(bits::replace_vui(*replaced, ours) == *replaced);

    // max_dec_frame_buffering follows max_num_ref_frames.
    params.max_num_ref_frames = 3;
    const auto three_refs = bits::replace_vui(bits::write_sps(params), ours);
    REQUIRE(three_refs.has_value());
    auto expected = sps_params(video::Profile::main);
    expected.max_num_ref_frames = 3;
    CHECK(*three_refs == bits::write_sps(expected));
}

TEST_CASE("Slice QPs are read from slice headers")
{
    for (const bool cabac : {false, true}) {
        const auto profile = cabac ? video::Profile::high : video::Profile::constrained_baseline;
        const auto sps = bits::parse_sps(bits::write_sps(sps_params(profile)));
        const auto pps = bits::parse_pps(bits::write_pps({.profile = profile, .pic_init_qp = 26}));
        REQUIRE(sps.has_value());
        REQUIRE(pps.has_value());
        CHECK(bits::slice_qp(slice(true, 0, -3, cabac), *sps, *pps) == 23);
        CHECK(bits::slice_qp(slice(false, 1, 4, cabac), *sps, *pps) == 30);
        CHECK(bits::slice_qp(slice(false, 700, 25, cabac), *sps, *pps) == 51);
        CHECK(bits::slice_qp(slice(false, 2, 26, cabac), *sps, *pps).error().code == Errc::invalid_value);
    }
}

TEST_CASE("Packed slice headers match our SPS and PPS")
{
    for (const auto profile : {video::Profile::constrained_baseline, video::Profile::main, video::Profile::high}) {
        const auto sps = bits::parse_sps(bits::write_sps(sps_params(profile)));
        const auto pps = bits::parse_pps(bits::write_pps({.profile = profile, .pic_init_qp = 30}));
        REQUIRE(sps.has_value());
        REQUIRE(pps.has_value());
        for (const bool idr : {true, false}) {
            CAPTURE(static_cast<int>(profile), idr);
            const auto header = bits::write_slice_header({.profile = profile,
                                                          .idr = idr,
                                                          .frame_num = idr ? 0U : 70'000U,
                                                          .idr_pic_id = 2,
                                                          .poc_lsb = idr ? 0U : 140'000U,
                                                          .qp_delta = -4});
            CHECK(header.bytes.front() == (idr ? std::byte{0x65} : std::byte{0x61}));
            CHECK(header.bits <= header.bytes.size() * 8);
            CHECK(header.bits > (header.bytes.size() - 1) * 8);
            CHECK(bits::slice_qp(header.bytes, *sps, *pps) == 26);
        }
    }
}

TEST_CASE("H.264 levels follow Table A-1")
{
    CHECK(bits::level_for(20, 15, 30, 0, video::Profile::constrained_baseline) == 13);
    CHECK(bits::level_for(120, 68, 30, 0, video::Profile::constrained_baseline) == 40);
    CHECK(bits::level_for(120, 68, 60, 0, video::Profile::main) == 42);
    CHECK(bits::level_for(120, 68, 30, 30'000, video::Profile::main) == 41);
    CHECK(bits::level_for(120, 68, 30, 22'000, video::Profile::high) == 40);  // High may use 1.25 x MaxBR
    CHECK(bits::level_for(240, 135, 30, 0, video::Profile::high) == 51);
    CHECK(bits::level_for(240, 135, 60, 0, video::Profile::high) == 52);
    CHECK(bits::level_for(512, 270, 30, 0, video::Profile::high) == 60);
    CHECK(bits::level_for(512, 512, 240, 0, video::Profile::high) == 62);
}

TEST_CASE("Driver access units become AVC420 access units")
{
    // As a driver might write it: its own AUD and SEI, limited-range BT.601 VUI.
    auto driver_params = sps_params(video::Profile::constrained_baseline);
    driver_params.vui.color = {.matrix = video::ColorSpace::Matrix::bt601, .full_range = false};
    const auto driver_sps = bits::write_sps(driver_params);
    const auto pps = bits::write_pps({.pic_init_qp = 26});
    const std::array aud{std::byte{0x09}, std::byte{0xF0}};
    const std::array sei{std::byte{0x06}, std::byte{0x05}, std::byte{0x01}, std::byte{0x00}, std::byte{0x80}};
    const auto idr_slice = slice(true, 0, -2, false);
    const auto p_slice = slice(false, 1, 1, false);

    bits::AccessUnitWriter writer;
    const bits::Vui vui{.color = {}, .fps = 30, .max_dec_frame_buffering = 1};
    writer.reset({.access_unit_delimiters = true, .vui = vui});

    const auto first = writer.finish(annex_b({aud, driver_sps, pps, sei, idr_slice}), true, 99);
    REQUIRE(first.has_value());
    CHECK(first->idr);
    CHECK(first->qp == 24);
    auto units = h264::split_annex_b(first->bitstream);
    REQUIRE(units.has_value());
    REQUIRE(units->size() == 4);
    CHECK((*units)[0].type == h264::nal_type::aud);
    CHECK((*units)[0].data[1] == std::byte{0x10});
    CHECK((*units)[1].type == h264::nal_type::sps);
    CHECK(std::ranges::equal((*units)[1].data, *bits::replace_vui(driver_sps, vui)));
    CHECK((*units)[2].type == h264::nal_type::pps);
    CHECK((*units)[3].type == h264::nal_type::idr);

    const auto second = writer.finish(annex_b({aud, p_slice}), false, 99);
    REQUIRE(second.has_value());
    CHECK_FALSE(second->idr);
    CHECK(second->qp == 27);
    units = h264::split_annex_b(second->bitstream);
    REQUIRE(units.has_value());
    REQUIRE(units->size() == 2);
    CHECK((*units)[0].data[1] == std::byte{0x30});
    CHECK((*units)[1].type == h264::nal_type::slice);

    // An IDR without parameter sets gets the remembered ones.
    const auto third = writer.finish(annex_b({idr_slice}), true, 99);
    REQUIRE(third.has_value());
    units = h264::split_annex_b(third->bitstream);
    REQUIRE(units.has_value());
    REQUIRE(units->size() == 4);
    CHECK((*units)[1].type == h264::nal_type::sps);
    CHECK((*units)[2].type == h264::nal_type::pps);

    // The picture type must be the one asked for.
    CHECK(writer.finish(annex_b({p_slice}), true, 99).error().code == Errc::io);
    CHECK(writer.finish(annex_b({aud}), false, 99).error().code == Errc::io);

    // Without delimiters, and without parameter sets there is no first IDR.
    writer.reset({.access_unit_delimiters = false, .vui = vui});
    CHECK(writer.finish(annex_b({idr_slice}), true, 99).error().code == Errc::io);
    const auto plain = writer.finish(annex_b({driver_sps, pps, idr_slice}), true, 99);
    REQUIRE(plain.has_value());
    units = h264::split_annex_b(plain->bitstream);
    REQUIRE(units.has_value());
    CHECK(units->front().type == h264::nal_type::sps);
}
