// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// nvenc_abi.hpp against the real headers where they are installed: EGL's
// eglext.h always (libegl-dev), NVIDIA's nvEncodeAPI.h from FFmpeg's
// nv-codec-headers (libffmpeg-nvenc-dev, pkg-config ffnvcodec) if present.
// Structure layouts are only compared against the 12.0 headers the backend
// was written for; the constants are the same in every version.

#include <farland/video/nvenc_abi.hpp>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#if defined(FARLAND_TEST_HAVE_FFNVCODEC)
#include <ffnvcodec/nvEncodeAPI.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>

namespace abi = farland::video::nvenc::abi;

TEST_CASE("NVENC ABI: EGL constants match eglext.h")
{
    CHECK(abi::egl_success == EGL_SUCCESS);
    CHECK(abi::egl_none == EGL_NONE);
    CHECK(abi::egl_extensions == EGL_EXTENSIONS);
    CHECK(abi::egl_width == EGL_WIDTH);
    CHECK(abi::egl_height == EGL_HEIGHT);
    CHECK(abi::egl_draw == EGL_DRAW);
    CHECK(abi::egl_read == EGL_READ);
    CHECK(abi::egl_opengl_api == EGL_OPENGL_API);
    CHECK(abi::egl_image_preserved_khr == EGL_IMAGE_PRESERVED_KHR);
    CHECK(abi::egl_platform_device_ext == EGL_PLATFORM_DEVICE_EXT);
    CHECK(abi::egl_cuda_device_nv == EGL_CUDA_DEVICE_NV);
    CHECK(abi::egl_linux_dma_buf_ext == EGL_LINUX_DMA_BUF_EXT);
    CHECK(abi::egl_linux_drm_fourcc_ext == EGL_LINUX_DRM_FOURCC_EXT);
    const auto& p = abi::egl_dma_buf_plane;
    CHECK(p[0].fd == EGL_DMA_BUF_PLANE0_FD_EXT);
    CHECK(p[0].offset == EGL_DMA_BUF_PLANE0_OFFSET_EXT);
    CHECK(p[0].pitch == EGL_DMA_BUF_PLANE0_PITCH_EXT);
    CHECK(p[0].modifier_lo == EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT);
    CHECK(p[0].modifier_hi == EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT);
    CHECK(p[1].fd == EGL_DMA_BUF_PLANE1_FD_EXT);
    CHECK(p[1].offset == EGL_DMA_BUF_PLANE1_OFFSET_EXT);
    CHECK(p[1].pitch == EGL_DMA_BUF_PLANE1_PITCH_EXT);
    CHECK(p[1].modifier_lo == EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT);
    CHECK(p[1].modifier_hi == EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT);
    CHECK(p[2].fd == EGL_DMA_BUF_PLANE2_FD_EXT);
    CHECK(p[2].offset == EGL_DMA_BUF_PLANE2_OFFSET_EXT);
    CHECK(p[2].pitch == EGL_DMA_BUF_PLANE2_PITCH_EXT);
    CHECK(p[2].modifier_lo == EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT);
    CHECK(p[2].modifier_hi == EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT);
    CHECK(p[3].fd == EGL_DMA_BUF_PLANE3_FD_EXT);
    CHECK(p[3].offset == EGL_DMA_BUF_PLANE3_OFFSET_EXT);
    CHECK(p[3].pitch == EGL_DMA_BUF_PLANE3_PITCH_EXT);
    CHECK(p[3].modifier_lo == EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT);
    CHECK(p[3].modifier_hi == EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT);
}

#if defined(FARLAND_TEST_HAVE_FFNVCODEC)

namespace {

bool same(const abi::GUID& ours, const GUID& theirs)
{
    return std::memcmp(&ours, &theirs, sizeof(GUID)) == 0;
}

}  // namespace

TEST_CASE("NVENC ABI: constants match nvEncodeAPI.h")
{
    static_assert(sizeof(abi::GUID) == sizeof(GUID));
    CHECK(same(abi::codec_h264_guid, NV_ENC_CODEC_H264_GUID));
    CHECK(same(abi::h264_profile_baseline_guid, NV_ENC_H264_PROFILE_BASELINE_GUID));
    CHECK(same(abi::h264_profile_main_guid, NV_ENC_H264_PROFILE_MAIN_GUID));
    CHECK(same(abi::h264_profile_high_guid, NV_ENC_H264_PROFILE_HIGH_GUID));
    CHECK(same(abi::preset_guids[0], NV_ENC_PRESET_P1_GUID));
    CHECK(same(abi::preset_guids[1], NV_ENC_PRESET_P2_GUID));
    CHECK(same(abi::preset_guids[2], NV_ENC_PRESET_P3_GUID));
    CHECK(same(abi::preset_guids[3], NV_ENC_PRESET_P4_GUID));
    CHECK(same(abi::preset_guids[4], NV_ENC_PRESET_P5_GUID));
    CHECK(same(abi::preset_guids[5], NV_ENC_PRESET_P6_GUID));
    CHECK(same(abi::preset_guids[6], NV_ENC_PRESET_P7_GUID));
    CHECK(abi::nv_enc_err_no_encode_device == NV_ENC_ERR_NO_ENCODE_DEVICE);
    CHECK(abi::nv_enc_err_unsupported_device == NV_ENC_ERR_UNSUPPORTED_DEVICE);
    CHECK(abi::nv_enc_err_out_of_memory == NV_ENC_ERR_OUT_OF_MEMORY);
    CHECK(abi::nv_enc_err_incompatible_client_key == NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY);
    CHECK(abi::nv_enc_err_resource_not_mapped == NV_ENC_ERR_RESOURCE_NOT_MAPPED);
    CHECK(abi::buffer_format_nv12 == NV_ENC_BUFFER_FORMAT_NV12);
    CHECK(abi::buffer_format_iyuv == NV_ENC_BUFFER_FORMAT_IYUV);
    CHECK(abi::buffer_format_argb == NV_ENC_BUFFER_FORMAT_ARGB);
    CHECK(abi::buffer_format_abgr == NV_ENC_BUFFER_FORMAT_ABGR);
    CHECK(abi::pic_flag_forceidr == NV_ENC_PIC_FLAG_FORCEIDR);
    CHECK(abi::pic_flag_output_spspps == NV_ENC_PIC_FLAG_OUTPUT_SPSPPS);
    CHECK(abi::pic_type_idr == NV_ENC_PIC_TYPE_IDR);
    CHECK(abi::rc_constqp == NV_ENC_PARAMS_RC_CONSTQP);
    CHECK(abi::rc_vbr == NV_ENC_PARAMS_RC_VBR);
    CHECK(abi::rc_cbr == NV_ENC_PARAMS_RC_CBR);
    CHECK(abi::tuning_info_low_latency == NV_ENC_TUNING_INFO_LOW_LATENCY);
    CHECK(abi::tuning_info_ultra_low_latency == NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY);
    CHECK(abi::caps_width_max == NV_ENC_CAPS_WIDTH_MAX);
    CHECK(abi::caps_height_max == NV_ENC_CAPS_HEIGHT_MAX);
    CHECK(abi::caps_width_min == NV_ENC_CAPS_WIDTH_MIN);
    CHECK(abi::caps_height_min == NV_ENC_CAPS_HEIGHT_MIN);
    CHECK(abi::entropy_coding_cavlc == NV_ENC_H264_ENTROPY_CODING_MODE_CAVLC);
    CHECK(abi::vui_transfer_srgb == NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB);
    CHECK(abi::vui_matrix_smpte170m == NV_ENC_VUI_MATRIX_COEFFS_SMPTE170M);
}

#if NVENCAPI_MAJOR_VERSION == 12 && NVENCAPI_MINOR_VERSION == 0

// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define FARLAND_SAME_LAYOUT(type) CHECK(sizeof(abi::type) == sizeof(::type))
#define FARLAND_SAME_OFFSET(type, field) CHECK(offsetof(abi::type, field) == offsetof(::type, field))
// NOLINTEND(cppcoreguidelines-macro-usage)

TEST_CASE("NVENC ABI: structures match nvEncodeAPI.h 12.0")
{
    CHECK(abi::api_version == NVENCAPI_VERSION);
    CHECK(abi::config_ver == NV_ENC_CONFIG_VER);
    CHECK(abi::initialize_params_ver == NV_ENC_INITIALIZE_PARAMS_VER);
    CHECK(abi::reconfigure_params_ver == NV_ENC_RECONFIGURE_PARAMS_VER);
    CHECK(abi::preset_config_ver == NV_ENC_PRESET_CONFIG_VER);
    CHECK(abi::pic_params_ver == NV_ENC_PIC_PARAMS_VER);
    CHECK(abi::lock_bitstream_ver == NV_ENC_LOCK_BITSTREAM_VER);
    CHECK(abi::register_resource_ver == NV_ENC_REGISTER_RESOURCE_VER);
    CHECK(abi::encode_api_function_list_ver == NV_ENCODE_API_FUNCTION_LIST_VER);
    FARLAND_SAME_LAYOUT(NV_ENC_CAPS_PARAM);
    FARLAND_SAME_LAYOUT(NV_ENC_CREATE_INPUT_BUFFER);
    FARLAND_SAME_LAYOUT(NV_ENC_CREATE_BITSTREAM_BUFFER);
    FARLAND_SAME_LAYOUT(NV_ENC_RC_PARAMS);
    FARLAND_SAME_OFFSET(NV_ENC_RC_PARAMS, minQP);
    FARLAND_SAME_OFFSET(NV_ENC_RC_PARAMS, targetQuality);
    FARLAND_SAME_OFFSET(NV_ENC_RC_PARAMS, multiPass);
    FARLAND_SAME_LAYOUT(NV_ENC_CONFIG_H264_VUI_PARAMETERS);
    FARLAND_SAME_LAYOUT(NV_ENC_CONFIG_H264);
    FARLAND_SAME_OFFSET(NV_ENC_CONFIG_H264, idrPeriod);
    FARLAND_SAME_OFFSET(NV_ENC_CONFIG_H264, maxNumRefFrames);
    FARLAND_SAME_OFFSET(NV_ENC_CONFIG_H264, h264VUIParameters);
    FARLAND_SAME_OFFSET(NV_ENC_CONFIG_H264, chromaFormatIDC);
    FARLAND_SAME_LAYOUT(NV_ENC_CONFIG);
    CHECK(offsetof(abi::NV_ENC_CONFIG, h264Config) == offsetof(::NV_ENC_CONFIG, encodeCodecConfig));
    FARLAND_SAME_LAYOUT(NV_ENC_INITIALIZE_PARAMS);
    FARLAND_SAME_OFFSET(NV_ENC_INITIALIZE_PARAMS, frameRateNum);
    FARLAND_SAME_OFFSET(NV_ENC_INITIALIZE_PARAMS, encodeConfig);
    FARLAND_SAME_OFFSET(NV_ENC_INITIALIZE_PARAMS, tuningInfo);
    FARLAND_SAME_LAYOUT(NV_ENC_RECONFIGURE_PARAMS);
    FARLAND_SAME_LAYOUT(NV_ENC_PRESET_CONFIG);
    FARLAND_SAME_LAYOUT(NV_ENC_PIC_PARAMS_H264);
    FARLAND_SAME_LAYOUT(NV_ENC_CODEC_PIC_PARAMS);
    FARLAND_SAME_LAYOUT(NV_ENC_PIC_PARAMS);
    FARLAND_SAME_OFFSET(NV_ENC_PIC_PARAMS, inputTimeStamp);
    FARLAND_SAME_OFFSET(NV_ENC_PIC_PARAMS, pictureType);
    FARLAND_SAME_OFFSET(NV_ENC_PIC_PARAMS, codecPicParams);
    FARLAND_SAME_LAYOUT(NV_ENC_LOCK_BITSTREAM);
    FARLAND_SAME_OFFSET(NV_ENC_LOCK_BITSTREAM, pictureType);
    FARLAND_SAME_OFFSET(NV_ENC_LOCK_BITSTREAM, frameAvgQP);
    FARLAND_SAME_LAYOUT(NV_ENC_LOCK_INPUT_BUFFER);
    FARLAND_SAME_OFFSET(NV_ENC_LOCK_INPUT_BUFFER, bufferDataPtr);
    FARLAND_SAME_LAYOUT(NV_ENC_MAP_INPUT_RESOURCE);
    FARLAND_SAME_OFFSET(NV_ENC_MAP_INPUT_RESOURCE, mappedResource);
    FARLAND_SAME_LAYOUT(NV_ENC_REGISTER_RESOURCE);
    FARLAND_SAME_OFFSET(NV_ENC_REGISTER_RESOURCE, registeredResource);
    FARLAND_SAME_LAYOUT(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS);
    FARLAND_SAME_LAYOUT(NV_ENCODE_API_FUNCTION_LIST);
    FARLAND_SAME_OFFSET(NV_ENCODE_API_FUNCTION_LIST, nvEncGetEncodeCaps);
    FARLAND_SAME_OFFSET(NV_ENCODE_API_FUNCTION_LIST, nvEncEncodePicture);
    FARLAND_SAME_OFFSET(NV_ENCODE_API_FUNCTION_LIST, nvEncMapInputResource);
    FARLAND_SAME_OFFSET(NV_ENCODE_API_FUNCTION_LIST, nvEncRegisterResource);
    FARLAND_SAME_OFFSET(NV_ENCODE_API_FUNCTION_LIST, nvEncGetEncodePresetConfigEx);
}

#undef FARLAND_SAME_LAYOUT
#undef FARLAND_SAME_OFFSET

#endif
#endif
