// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

/// The parts of NVIDIA's encoder, CUDA driver, EGL and OpenGL interfaces that
/// the NVENC backend uses (docs/PLAN.md §3.4).
///
/// farland loads libnvidia-encode.so.1, libcuda.so.1 and libEGL.so.1 at
/// runtime, like OpenH264 (openh264_abi.hpp), so neither the Video Codec SDK
/// nor the CUDA toolkit is needed to build. The declarations follow
///
/// - nvEncodeAPI.h of NVENC API 12.0 (NVIDIA, MIT licence), as shipped in
///   FFmpeg's nv-codec-headers n12.0.16.0;
/// - cuda.h / dynlink_cuda.h (the same package) for the CUDA driver API;
/// - the Khronos EGL and GL registries (eglext.h, gl.h),
///
/// keep their names, and assert their sizes and offsets on LP64 below. Where
/// the headers are installed, tests/video/test_nvenc_abi.cpp compares them.
///
/// Versions. Each NVENC structure carries a version word that encodes the
/// API version the caller was built for (NVENCAPI_STRUCT_VERSION). Drivers
/// accept every older API version, so the backend targets 12.0: it needs the
/// P1-P7 presets with tuning info (10.0) and nothing newer, and runs on every
/// driver since 520 (NvEncodeAPIGetMaxSupportedVersion >= 12.0). Newer
/// drivers (595 reports API 13.0) keep accepting 12.0 structures; structure
/// layouts only change together with the version words, so a newer header is
/// never needed to talk to a newer driver.
namespace farland::video::nvenc::abi {

// ---------------------------------------------------------------------------
// NVENC API 12.0 (nvEncodeAPI.h)
// ---------------------------------------------------------------------------

inline constexpr std::uint32_t api_major = 12;
inline constexpr std::uint32_t api_minor = 0;
/// NVENCAPI_VERSION.
inline constexpr std::uint32_t api_version = api_major | (api_minor << 24U);

/// NVENCAPI_STRUCT_VERSION(ver).
[[nodiscard]] constexpr std::uint32_t struct_version(std::uint32_t ver) noexcept
{
    return api_version | (ver << 16U) | (0x7U << 28U);
}

inline constexpr std::uint32_t caps_param_ver = struct_version(1);
inline constexpr std::uint32_t create_input_buffer_ver = struct_version(1);
inline constexpr std::uint32_t create_bitstream_buffer_ver = struct_version(1);
inline constexpr std::uint32_t rc_params_ver = struct_version(1);
inline constexpr std::uint32_t config_ver = struct_version(8) | (1U << 31U);
inline constexpr std::uint32_t initialize_params_ver = struct_version(5) | (1U << 31U);
inline constexpr std::uint32_t reconfigure_params_ver = struct_version(1) | (1U << 31U);
inline constexpr std::uint32_t preset_config_ver = struct_version(4) | (1U << 31U);
inline constexpr std::uint32_t pic_params_ver = struct_version(6) | (1U << 31U);
inline constexpr std::uint32_t lock_bitstream_ver = struct_version(2);
inline constexpr std::uint32_t lock_input_buffer_ver = struct_version(1);
inline constexpr std::uint32_t map_input_resource_ver = struct_version(4);
inline constexpr std::uint32_t register_resource_ver = struct_version(4);
inline constexpr std::uint32_t open_encode_session_ex_params_ver = struct_version(1);
inline constexpr std::uint32_t encode_api_function_list_ver = struct_version(2);

inline constexpr std::uint32_t infinite_goplength = 0xffffffffU;

// NVENCSTATUS
inline constexpr int nv_enc_success = 0;
inline constexpr int nv_enc_err_no_encode_device = 1;
inline constexpr int nv_enc_err_unsupported_device = 2;
inline constexpr int nv_enc_err_out_of_memory = 10;
inline constexpr int nv_enc_err_incompatible_client_key = 21;
inline constexpr int nv_enc_err_resource_not_mapped = 25;
// NV_ENC_BUFFER_FORMAT
inline constexpr std::uint32_t buffer_format_nv12 = 0x00000001;
inline constexpr std::uint32_t buffer_format_iyuv = 0x00000100;
inline constexpr std::uint32_t buffer_format_argb = 0x01000000;  ///< B, G, R, A in memory
inline constexpr std::uint32_t buffer_format_abgr = 0x10000000;  ///< R, G, B, A in memory
// NV_ENC_PIC_FLAGS
inline constexpr std::uint32_t pic_flag_forceidr = 0x2;
inline constexpr std::uint32_t pic_flag_output_spspps = 0x4;
// NV_ENC_PIC_STRUCT, NV_ENC_PIC_TYPE
inline constexpr std::uint32_t pic_struct_frame = 0x01;
inline constexpr std::uint32_t pic_type_p = 0x00;
inline constexpr std::uint32_t pic_type_i = 0x02;
inline constexpr std::uint32_t pic_type_idr = 0x03;
// NV_ENC_PARAMS_RC_MODE
inline constexpr std::uint32_t rc_constqp = 0x0;
inline constexpr std::uint32_t rc_vbr = 0x1;
inline constexpr std::uint32_t rc_cbr = 0x2;
// NV_ENC_TUNING_INFO
inline constexpr std::uint32_t tuning_info_low_latency = 2;
inline constexpr std::uint32_t tuning_info_ultra_low_latency = 3;
// NV_ENC_DEVICE_TYPE, NV_ENC_INPUT_RESOURCE_TYPE, NV_ENC_BUFFER_USAGE
inline constexpr std::uint32_t device_type_cuda = 0x1;
inline constexpr std::uint32_t input_resource_type_cudadeviceptr = 0x1;
inline constexpr std::uint32_t buffer_usage_input_image = 0x0;
// NV_ENC_H264_ENTROPY_CODING_MODE, NV_ENC_H264_ADAPTIVE_TRANSFORM_MODE
inline constexpr std::uint32_t entropy_coding_cabac = 0x1;
inline constexpr std::uint32_t entropy_coding_cavlc = 0x2;
inline constexpr std::uint32_t adaptive_transform_autoselect = 0x0;
inline constexpr std::uint32_t adaptive_transform_disable = 0x1;
// NV_ENC_CAPS
inline constexpr std::uint32_t caps_width_max = 16;
inline constexpr std::uint32_t caps_height_max = 17;
inline constexpr std::uint32_t caps_width_min = 45;
inline constexpr std::uint32_t caps_height_min = 46;
// VUI values (H.264 Annex E, as NV_ENC_VUI_*).
inline constexpr std::uint32_t vui_video_format_unspecified = 5;
inline constexpr std::uint32_t vui_color_primaries_bt709 = 1;
inline constexpr std::uint32_t vui_transfer_srgb = 13;  // IEC 61966-2-1
inline constexpr std::uint32_t vui_matrix_bt709 = 1;
inline constexpr std::uint32_t vui_matrix_smpte170m = 6;

struct GUID {
    std::uint32_t Data1;
    std::uint16_t Data2;
    std::uint16_t Data3;
    std::array<std::uint8_t, 8> Data4;
};

// {6BC82762-4E63-4ca4-AA85-1E50F321F6BF}
inline constexpr GUID codec_h264_guid{0x6bc82762, 0x4e63, 0x4ca4, {0xaa, 0x85, 0x1e, 0x50, 0xf3, 0x21, 0xf6, 0xbf}};
// {0727BCAA-78C4-4c83-8C2F-EF3DFF267C6A}
inline constexpr GUID h264_profile_baseline_guid{
    0x727bcaa, 0x78c4, 0x4c83, {0x8c, 0x2f, 0xef, 0x3d, 0xff, 0x26, 0x7c, 0x6a}};
// {60B5C1D4-67FE-4790-94D5-C4726D7B6E6D}
inline constexpr GUID h264_profile_main_guid{
    0x60b5c1d4, 0x67fe, 0x4790, {0x94, 0xd5, 0xc4, 0x72, 0x6d, 0x7b, 0x6e, 0x6d}};
// {E7CBC309-4F7A-4b89-AF2A-D537C92BE310}
inline constexpr GUID h264_profile_high_guid{
    0xe7cbc309, 0x4f7a, 0x4b89, {0xaf, 0x2a, 0xd5, 0x37, 0xc9, 0x2b, 0xe3, 0x10}};
/// NV_ENC_PRESET_P1_GUID .. NV_ENC_PRESET_P7_GUID: faster to better.
inline constexpr std::array<GUID, 7> preset_guids{{
    {0xfc0a8d3e, 0x45f8, 0x4cf8, {0x80, 0xc7, 0x29, 0x88, 0x71, 0x59, 0x0e, 0xbf}},
    {0xf581cfb8, 0x88d6, 0x4381, {0x93, 0xf0, 0xdf, 0x13, 0xf9, 0xc2, 0x7d, 0xab}},
    {0x36850110, 0x3a07, 0x441f, {0x94, 0xd5, 0x36, 0x70, 0x63, 0x1f, 0x91, 0xf6}},
    {0x90a7b826, 0xdf06, 0x4862, {0xb9, 0xd2, 0xcd, 0x6d, 0x73, 0xa0, 0x86, 0x81}},
    {0x21c6e6b4, 0x297a, 0x4cba, {0x99, 0x8f, 0xb6, 0xcb, 0xde, 0x72, 0xad, 0xe3}},
    {0x8e75c279, 0x6299, 0x4ab6, {0x83, 0x02, 0x0b, 0x21, 0x5a, 0x33, 0x5c, 0xf5}},
    {0x84848c12, 0x6f71, 0x4c13, {0x93, 0x1b, 0x53, 0xe2, 0x83, 0xf5, 0x79, 0x74}},
}};

struct NV_ENC_CAPS_PARAM {
    std::uint32_t version;
    std::uint32_t capsToQuery;
    std::array<std::uint32_t, 62> reserved;
};

struct NV_ENC_CREATE_INPUT_BUFFER {
    std::uint32_t version;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t memoryHeap;
    std::uint32_t bufferFmt;
    std::uint32_t reserved;
    void* inputBuffer;
    void* pSysMemBuffer;
    std::array<std::uint32_t, 57> reserved1;
    std::array<void*, 63> reserved2;
};

struct NV_ENC_CREATE_BITSTREAM_BUFFER {
    std::uint32_t version;
    std::uint32_t size;
    std::uint32_t memoryHeap;
    std::uint32_t reserved;
    void* bitstreamBuffer;
    void* bitstreamBufferPtr;
    std::array<std::uint32_t, 58> reserved1;
    std::array<void*, 64> reserved2;
};

struct NV_ENC_QP {
    std::uint32_t qpInterP;
    std::uint32_t qpInterB;
    std::uint32_t qpIntra;
};

struct NV_ENC_RC_PARAMS {
    std::uint32_t version;
    std::uint32_t rateControlMode;
    NV_ENC_QP constQP;
    std::uint32_t averageBitRate;
    std::uint32_t maxBitRate;
    std::uint32_t vbvBufferSize;
    std::uint32_t vbvInitialDelay;
    std::uint32_t enableMinQP : 1;
    std::uint32_t enableMaxQP : 1;
    std::uint32_t enableInitialRCQP : 1;
    std::uint32_t enableAQ : 1;
    std::uint32_t reservedBitField1 : 1;
    std::uint32_t enableLookahead : 1;
    std::uint32_t disableIadapt : 1;
    std::uint32_t disableBadapt : 1;
    std::uint32_t enableTemporalAQ : 1;
    std::uint32_t zeroReorderDelay : 1;
    std::uint32_t enableNonRefP : 1;
    std::uint32_t strictGOPTarget : 1;
    std::uint32_t aqStrength : 4;
    std::uint32_t reservedBitFields : 16;
    NV_ENC_QP minQP;
    NV_ENC_QP maxQP;
    NV_ENC_QP initialRCQP;
    std::uint32_t temporallayerIdxMask;
    std::array<std::uint8_t, 8> temporalLayerQP;
    std::uint8_t targetQuality;
    std::uint8_t targetQualityLSB;
    std::uint16_t lookaheadDepth;
    std::uint8_t lowDelayKeyFrameScale;
    std::int8_t yDcQPIndexOffset;
    std::int8_t uDcQPIndexOffset;
    std::int8_t vDcQPIndexOffset;
    std::uint32_t qpMapMode;
    std::uint32_t multiPass;
    std::uint32_t alphaLayerBitrateRatio;
    std::int8_t cbQPIndexOffset;
    std::int8_t crQPIndexOffset;
    std::uint16_t reserved2;
    std::array<std::uint32_t, 4> reserved;
};

struct NV_ENC_CONFIG_H264_VUI_PARAMETERS {
    std::uint32_t overscanInfoPresentFlag;
    std::uint32_t overscanInfo;
    std::uint32_t videoSignalTypePresentFlag;
    std::uint32_t videoFormat;
    std::uint32_t videoFullRangeFlag;
    std::uint32_t colourDescriptionPresentFlag;
    std::uint32_t colourPrimaries;
    std::uint32_t transferCharacteristics;
    std::uint32_t colourMatrix;
    std::uint32_t chromaSampleLocationFlag;
    std::uint32_t chromaSampleLocationTop;
    std::uint32_t chromaSampleLocationBot;
    std::uint32_t bitstreamRestrictionFlag;
    std::uint32_t timingInfoPresentFlag;
    std::uint32_t numUnitInTicks;
    std::uint32_t timeScale;
    std::array<std::uint32_t, 12> reserved;
};

/// The largest member of NV_ENC_CODEC_CONFIG, so it stands for the union.
struct NV_ENC_CONFIG_H264 {
    std::uint32_t enableTemporalSVC : 1;
    std::uint32_t enableStereoMVC : 1;
    std::uint32_t hierarchicalPFrames : 1;
    std::uint32_t hierarchicalBFrames : 1;
    std::uint32_t outputBufferingPeriodSEI : 1;
    std::uint32_t outputPictureTimingSEI : 1;
    std::uint32_t outputAUD : 1;
    std::uint32_t disableSPSPPS : 1;
    std::uint32_t outputFramePackingSEI : 1;
    std::uint32_t outputRecoveryPointSEI : 1;
    std::uint32_t enableIntraRefresh : 1;
    std::uint32_t enableConstrainedEncoding : 1;
    std::uint32_t repeatSPSPPS : 1;
    std::uint32_t enableVFR : 1;
    std::uint32_t enableLTR : 1;
    std::uint32_t qpPrimeYZeroTransformBypassFlag : 1;
    std::uint32_t useConstrainedIntraPred : 1;
    std::uint32_t enableFillerDataInsertion : 1;
    std::uint32_t disableSVCPrefixNalu : 1;
    std::uint32_t enableScalabilityInfoSEI : 1;
    std::uint32_t singleSliceIntraRefresh : 1;
    std::uint32_t enableTimeCode : 1;
    std::uint32_t reservedBitFields : 10;
    std::uint32_t level;
    std::uint32_t idrPeriod;
    std::uint32_t separateColourPlaneFlag;
    std::uint32_t disableDeblockingFilterIDC;
    std::uint32_t numTemporalLayers;
    std::uint32_t spsId;
    std::uint32_t ppsId;
    std::uint32_t adaptiveTransformMode;
    std::uint32_t fmoMode;
    std::uint32_t bdirectMode;
    std::uint32_t entropyCodingMode;
    std::uint32_t stereoMode;
    std::uint32_t intraRefreshPeriod;
    std::uint32_t intraRefreshCnt;
    std::uint32_t maxNumRefFrames;
    std::uint32_t sliceMode;
    std::uint32_t sliceModeData;
    NV_ENC_CONFIG_H264_VUI_PARAMETERS h264VUIParameters;
    std::uint32_t ltrNumFrames;
    std::uint32_t ltrTrustMode;
    std::uint32_t chromaFormatIDC;
    std::uint32_t maxTemporalLayers;
    std::uint32_t useBFramesAsRef;
    std::uint32_t numRefL0;
    std::uint32_t numRefL1;
    std::array<std::uint32_t, 267> reserved1;
    std::array<void*, 64> reserved2;
};

struct NV_ENC_CONFIG {
    std::uint32_t version;
    GUID profileGUID;
    std::uint32_t gopLength;
    std::int32_t frameIntervalP;
    std::uint32_t monoChromeEncoding;
    std::uint32_t frameFieldMode;
    std::uint32_t mvPrecision;
    NV_ENC_RC_PARAMS rcParams;
    /// NV_ENC_CODEC_CONFIG encodeCodecConfig; h264Config is its largest member.
    NV_ENC_CONFIG_H264 h264Config;
    std::array<std::uint32_t, 278> reserved;
    std::array<void*, 64> reserved2;
};

struct NVENC_EXTERNAL_ME_HINT_COUNTS_PER_BLOCKTYPE {
    std::uint32_t counts;  ///< bit fields, unused here
    std::array<std::uint32_t, 3> reserved1;
};

struct NV_ENC_INITIALIZE_PARAMS {
    std::uint32_t version;
    GUID encodeGUID;
    GUID presetGUID;
    std::uint32_t encodeWidth;
    std::uint32_t encodeHeight;
    std::uint32_t darWidth;
    std::uint32_t darHeight;
    std::uint32_t frameRateNum;
    std::uint32_t frameRateDen;
    std::uint32_t enableEncodeAsync;
    std::uint32_t enablePTD;
    std::uint32_t reportSliceOffsets : 1;
    std::uint32_t enableSubFrameWrite : 1;
    std::uint32_t enableExternalMEHints : 1;
    std::uint32_t enableMEOnlyMode : 1;
    std::uint32_t enableWeightedPrediction : 1;
    std::uint32_t enableOutputInVidmem : 1;
    std::uint32_t reservedBitFields : 26;
    std::uint32_t privDataSize;
    void* privData;
    NV_ENC_CONFIG* encodeConfig;
    std::uint32_t maxEncodeWidth;
    std::uint32_t maxEncodeHeight;
    std::array<NVENC_EXTERNAL_ME_HINT_COUNTS_PER_BLOCKTYPE, 2> maxMEHintCountsPerBlock;
    std::uint32_t tuningInfo;
    std::uint32_t bufferFormat;
    std::array<std::uint32_t, 287> reserved;
    std::array<void*, 64> reserved2;
};

struct NV_ENC_RECONFIGURE_PARAMS {
    std::uint32_t version;
    NV_ENC_INITIALIZE_PARAMS reInitEncodeParams;
    std::uint32_t resetEncoder : 1;
    std::uint32_t forceIDR : 1;
    std::uint32_t reserved : 30;
};

struct NV_ENC_PRESET_CONFIG {
    std::uint32_t version;
    NV_ENC_CONFIG presetCfg;
    std::array<std::uint32_t, 255> reserved1;
    std::array<void*, 64> reserved2;
};

struct NV_ENC_TIME_CODE {
    std::uint32_t displayPicStruct;
    std::array<std::uint32_t, 6> clockTimestamp;  ///< NV_ENC_CLOCK_TIMESTAMP_SET[3]
};

struct NV_ENC_PIC_PARAMS_H264 {
    std::uint32_t displayPOCSyntax;
    std::uint32_t reserved3;
    std::uint32_t refPicFlag;
    std::uint32_t colourPlaneId;
    std::uint32_t forceIntraRefreshWithFrameCnt;
    std::uint32_t bitFields;  ///< constrainedFrame, sliceModeDataUpdate, ltrMarkFrame, ltrUseFrames
    std::uint8_t* sliceTypeData;
    std::uint32_t sliceTypeArrayCnt;
    std::uint32_t seiPayloadArrayCnt;
    void* seiPayloadArray;
    std::uint32_t sliceMode;
    std::uint32_t sliceModeData;
    std::uint32_t ltrMarkFrameIdx;
    std::uint32_t ltrUseFrameBitmap;
    std::uint32_t ltrUsageMode;
    std::uint32_t forceIntraSliceCount;
    std::uint32_t* forceIntraSliceIdx;
    std::array<std::uint32_t, 32> h264ExtPicParams;  ///< NV_ENC_PIC_PARAMS_H264_EXT
    NV_ENC_TIME_CODE timeCode;
    std::array<std::uint32_t, 203> reserved;
    std::array<void*, 61> reserved2;
};

/// NV_ENC_CODEC_PIC_PARAMS: the AV1 member makes the union larger than the
/// H.264 one.
union NV_ENC_CODEC_PIC_PARAMS {
    NV_ENC_PIC_PARAMS_H264 h264PicParams;
    std::array<std::uint32_t, 388> reserved;
};

struct NV_ENC_PIC_PARAMS {
    std::uint32_t version;
    std::uint32_t inputWidth;
    std::uint32_t inputHeight;
    std::uint32_t inputPitch;
    std::uint32_t encodePicFlags;
    std::uint32_t frameIdx;
    std::uint64_t inputTimeStamp;
    std::uint64_t inputDuration;
    void* inputBuffer;
    void* outputBitstream;
    void* completionEvent;
    std::uint32_t bufferFmt;
    std::uint32_t pictureStruct;
    std::uint32_t pictureType;
    NV_ENC_CODEC_PIC_PARAMS codecPicParams;
    std::array<NVENC_EXTERNAL_ME_HINT_COUNTS_PER_BLOCKTYPE, 2> meHintCountsPerBlock;
    void* meExternalHints;
    std::array<std::uint32_t, 6> reserved1;
    std::array<void*, 2> reserved2;
    std::int8_t* qpDeltaMap;
    std::uint32_t qpDeltaMapSize;
    std::uint32_t reservedBitFields;
    std::array<std::uint16_t, 2> meHintRefPicDist;
    void* alphaBuffer;
    void* meExternalSbHints;
    std::uint32_t meSbHintsCount;
    std::array<std::uint32_t, 285> reserved3;
    std::array<void*, 58> reserved4;
};

struct NV_ENC_LOCK_BITSTREAM {
    std::uint32_t version;
    std::uint32_t doNotWait : 1;
    std::uint32_t ltrFrame : 1;
    std::uint32_t getRCStats : 1;
    std::uint32_t reservedBitFields : 29;
    void* outputBitstream;
    std::uint32_t* sliceOffsets;
    std::uint32_t frameIdx;
    std::uint32_t hwEncodeStatus;
    std::uint32_t numSlices;
    std::uint32_t bitstreamSizeInBytes;
    std::uint64_t outputTimeStamp;
    std::uint64_t outputDuration;
    void* bitstreamBufferPtr;
    std::uint32_t pictureType;
    std::uint32_t pictureStruct;
    std::uint32_t frameAvgQP;
    std::uint32_t frameSatd;
    std::uint32_t ltrFrameIdx;
    std::uint32_t ltrFrameBitmap;
    std::uint32_t temporalId;
    std::array<std::uint32_t, 12> reserved;
    std::uint32_t intraMBCount;
    std::uint32_t interMBCount;
    std::int32_t averageMVX;
    std::int32_t averageMVY;
    std::uint32_t alphaLayerSizeInBytes;
    std::array<std::uint32_t, 218> reserved1;
    std::array<void*, 64> reserved2;
};

struct NV_ENC_LOCK_INPUT_BUFFER {
    std::uint32_t version;
    std::uint32_t doNotWait : 1;
    std::uint32_t reservedBitFields : 31;
    void* inputBuffer;
    void* bufferDataPtr;
    std::uint32_t pitch;
    std::array<std::uint32_t, 251> reserved1;
    std::array<void*, 64> reserved2;
};

struct NV_ENC_MAP_INPUT_RESOURCE {
    std::uint32_t version;
    std::uint32_t subResourceIndex;
    void* inputResource;
    void* registeredResource;
    void* mappedResource;
    std::uint32_t mappedBufferFmt;
    std::array<std::uint32_t, 251> reserved1;
    std::array<void*, 63> reserved2;
};

struct NV_ENC_REGISTER_RESOURCE {
    std::uint32_t version;
    std::uint32_t resourceType;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pitch;
    std::uint32_t subResourceIndex;
    void* resourceToRegister;
    void* registeredResource;
    std::uint32_t bufferFormat;
    std::uint32_t bufferUsage;
    void* pInputFencePoint;
    std::array<std::uint32_t, 247> reserved1;
    std::array<void*, 61> reserved2;
};

struct NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS {
    std::uint32_t version;
    std::uint32_t deviceType;
    void* device;
    void* reserved;
    std::uint32_t apiVersion;
    std::array<std::uint32_t, 253> reserved1;
    std::array<void*, 64> reserved2;
};

using NVENCSTATUS = int;

/// NV_ENCODE_API_FUNCTION_LIST. Entries farland does not call are void*.
struct NV_ENCODE_API_FUNCTION_LIST {
    std::uint32_t version;
    std::uint32_t reserved;
    void* nvEncOpenEncodeSession;
    void* nvEncGetEncodeGUIDCount;
    void* nvEncGetEncodeProfileGUIDCount;
    void* nvEncGetEncodeProfileGUIDs;
    void* nvEncGetEncodeGUIDs;
    void* nvEncGetInputFormatCount;
    void* nvEncGetInputFormats;
    NVENCSTATUS (*nvEncGetEncodeCaps)(void* encoder, GUID encodeGUID, NV_ENC_CAPS_PARAM* capsParam, int* capsVal);
    void* nvEncGetEncodePresetCount;
    void* nvEncGetEncodePresetGUIDs;
    void* nvEncGetEncodePresetConfig;
    NVENCSTATUS (*nvEncInitializeEncoder)(void* encoder, NV_ENC_INITIALIZE_PARAMS* params);
    NVENCSTATUS (*nvEncCreateInputBuffer)(void* encoder, NV_ENC_CREATE_INPUT_BUFFER* params);
    NVENCSTATUS (*nvEncDestroyInputBuffer)(void* encoder, void* inputBuffer);
    NVENCSTATUS (*nvEncCreateBitstreamBuffer)(void* encoder, NV_ENC_CREATE_BITSTREAM_BUFFER* params);
    NVENCSTATUS (*nvEncDestroyBitstreamBuffer)(void* encoder, void* bitstreamBuffer);
    NVENCSTATUS (*nvEncEncodePicture)(void* encoder, NV_ENC_PIC_PARAMS* params);
    NVENCSTATUS (*nvEncLockBitstream)(void* encoder, NV_ENC_LOCK_BITSTREAM* params);
    NVENCSTATUS (*nvEncUnlockBitstream)(void* encoder, void* bitstreamBuffer);
    NVENCSTATUS (*nvEncLockInputBuffer)(void* encoder, NV_ENC_LOCK_INPUT_BUFFER* params);
    NVENCSTATUS (*nvEncUnlockInputBuffer)(void* encoder, void* inputBuffer);
    void* nvEncGetEncodeStats;
    void* nvEncGetSequenceParams;
    void* nvEncRegisterAsyncEvent;
    void* nvEncUnregisterAsyncEvent;
    NVENCSTATUS (*nvEncMapInputResource)(void* encoder, NV_ENC_MAP_INPUT_RESOURCE* params);
    NVENCSTATUS (*nvEncUnmapInputResource)(void* encoder, void* mappedInputBuffer);
    NVENCSTATUS (*nvEncDestroyEncoder)(void* encoder);
    void* nvEncInvalidateRefFrames;
    NVENCSTATUS (*nvEncOpenEncodeSessionEx)(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS* params, void** encoder);
    NVENCSTATUS (*nvEncRegisterResource)(void* encoder, NV_ENC_REGISTER_RESOURCE* params);
    NVENCSTATUS (*nvEncUnregisterResource)(void* encoder, void* registeredResource);
    NVENCSTATUS (*nvEncReconfigureEncoder)(void* encoder, NV_ENC_RECONFIGURE_PARAMS* params);
    void* reserved1;
    void* nvEncCreateMVBuffer;
    void* nvEncDestroyMVBuffer;
    void* nvEncRunMotionEstimationOnly;
    const char* (*nvEncGetLastErrorString)(void* encoder);
    void* nvEncSetIOCudaStreams;
    NVENCSTATUS (*nvEncGetEncodePresetConfigEx)(void* encoder, GUID encodeGUID, GUID presetGUID,
                                                std::uint32_t tuningInfo, NV_ENC_PRESET_CONFIG* presetConfig);
    void* nvEncGetSequenceParamEx;
    std::array<void*, 277> reserved2;
};

using NvEncodeAPICreateInstanceFn = NVENCSTATUS (*)(NV_ENCODE_API_FUNCTION_LIST* functionList);
/// Reports (major << 4) | minor of the newest API the driver implements.
using NvEncodeAPIGetMaxSupportedVersionFn = NVENCSTATUS (*)(std::uint32_t* version);

// Sizes and offsets of the 12.0 structures on LP64 (x86_64, aarch64), taken
// from nv-codec-headers n12.0.16.0.
#if defined(__LP64__)
static_assert(sizeof(NV_ENC_CAPS_PARAM) == 256);
static_assert(sizeof(NV_ENC_CREATE_INPUT_BUFFER) == 776 && offsetof(NV_ENC_CREATE_INPUT_BUFFER, inputBuffer) == 24);
static_assert(sizeof(NV_ENC_CREATE_BITSTREAM_BUFFER) == 776 &&
              offsetof(NV_ENC_CREATE_BITSTREAM_BUFFER, bitstreamBuffer) == 16);
static_assert(sizeof(NV_ENC_RC_PARAMS) == 128 && offsetof(NV_ENC_RC_PARAMS, minQP) == 40 &&
              offsetof(NV_ENC_RC_PARAMS, targetQuality) == 88 && offsetof(NV_ENC_RC_PARAMS, qpMapMode) == 96 &&
              offsetof(NV_ENC_RC_PARAMS, cbQPIndexOffset) == 108);
static_assert(sizeof(NV_ENC_CONFIG_H264_VUI_PARAMETERS) == 112);
static_assert(sizeof(NV_ENC_CONFIG_H264) == 1792 && offsetof(NV_ENC_CONFIG_H264, entropyCodingMode) == 44 &&
              offsetof(NV_ENC_CONFIG_H264, h264VUIParameters) == 72 && offsetof(NV_ENC_CONFIG_H264, numRefL1) == 208);
static_assert(sizeof(NV_ENC_CONFIG) == 3584 && offsetof(NV_ENC_CONFIG, rcParams) == 40 &&
              offsetof(NV_ENC_CONFIG, h264Config) == 168 && offsetof(NV_ENC_CONFIG, reserved2) == 3072);
static_assert(sizeof(NV_ENC_INITIALIZE_PARAMS) == 1808 && offsetof(NV_ENC_INITIALIZE_PARAMS, enablePTD) == 64 &&
              offsetof(NV_ENC_INITIALIZE_PARAMS, encodeConfig) == 88 &&
              offsetof(NV_ENC_INITIALIZE_PARAMS, tuningInfo) == 136);
static_assert(sizeof(NV_ENC_RECONFIGURE_PARAMS) == 1824);
static_assert(sizeof(NV_ENC_PRESET_CONFIG) == 5128 && offsetof(NV_ENC_PRESET_CONFIG, presetCfg) == 8);
static_assert(sizeof(NV_ENC_PIC_PARAMS_H264) == 1536 && offsetof(NV_ENC_PIC_PARAMS_H264, timeCode) == 208);
static_assert(sizeof(NV_ENC_CODEC_PIC_PARAMS) == 1552);
static_assert(sizeof(NV_ENC_PIC_PARAMS) == 3360 && offsetof(NV_ENC_PIC_PARAMS, inputBuffer) == 40 &&
              offsetof(NV_ENC_PIC_PARAMS, codecPicParams) == 80 && offsetof(NV_ENC_PIC_PARAMS, qpDeltaMap) == 1712 &&
              offsetof(NV_ENC_PIC_PARAMS, meSbHintsCount) == 1752);
static_assert(sizeof(NV_ENC_LOCK_BITSTREAM) == 1544 && offsetof(NV_ENC_LOCK_BITSTREAM, bitstreamSizeInBytes) == 36 &&
              offsetof(NV_ENC_LOCK_BITSTREAM, bitstreamBufferPtr) == 56 &&
              offsetof(NV_ENC_LOCK_BITSTREAM, frameAvgQP) == 72 && offsetof(NV_ENC_LOCK_BITSTREAM, reserved1) == 160);
static_assert(sizeof(NV_ENC_LOCK_INPUT_BUFFER) == 1544 && offsetof(NV_ENC_LOCK_INPUT_BUFFER, pitch) == 24);
static_assert(sizeof(NV_ENC_MAP_INPUT_RESOURCE) == 1544 && offsetof(NV_ENC_MAP_INPUT_RESOURCE, mappedBufferFmt) == 32);
static_assert(sizeof(NV_ENC_REGISTER_RESOURCE) == 1536 && offsetof(NV_ENC_REGISTER_RESOURCE, bufferFormat) == 40 &&
              offsetof(NV_ENC_REGISTER_RESOURCE, reserved1) == 56);
static_assert(sizeof(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS) == 1552 &&
              offsetof(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS, apiVersion) == 24);
static_assert(sizeof(NV_ENCODE_API_FUNCTION_LIST) == 2552 &&
              offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncOpenEncodeSessionEx) == 240 &&
              offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncReconfigureEncoder) == 264 &&
              offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncGetLastErrorString) == 304 &&
              offsetof(NV_ENCODE_API_FUNCTION_LIST, reserved2) == 336);
#endif

// ---------------------------------------------------------------------------
// CUDA driver API (cuda.h). The _v2 entry points are the ones cuda.h maps
// the plain names to since CUDA 3.2; drivers keep exporting them.
// ---------------------------------------------------------------------------

using CUresult = int;
using CUdevice = int;
using CUdeviceptr = unsigned long long;
using CUcontext = struct CUctx_st*;
using CUmodule = struct CUmod_st*;
using CUfunction = struct CUfunc_st*;
using CUstream = struct CUstream_st*;
using CUarray = struct CUarray_st*;
using CUgraphicsResource = struct CUgraphicsResource_st*;

inline constexpr CUresult cuda_success = 0;
inline constexpr unsigned cu_ctx_sched_blocking_sync = 0x04;
inline constexpr int cu_memorytype_host = 1;
inline constexpr int cu_memorytype_device = 2;
inline constexpr int cu_memorytype_array = 3;
inline constexpr unsigned cu_graphics_register_flags_read_only = 0x01;
// CUjit_option
inline constexpr int cu_jit_error_log_buffer = 5;
inline constexpr int cu_jit_error_log_buffer_size_bytes = 6;

struct CUDA_MEMCPY2D {
    std::size_t srcXInBytes;
    std::size_t srcY;
    int srcMemoryType;
    const void* srcHost;
    CUdeviceptr srcDevice;
    CUarray srcArray;
    std::size_t srcPitch;
    std::size_t dstXInBytes;
    std::size_t dstY;
    int dstMemoryType;
    void* dstHost;
    CUdeviceptr dstDevice;
    CUarray dstArray;
    std::size_t dstPitch;
    std::size_t WidthInBytes;
    std::size_t Height;
};
#if defined(__LP64__)
static_assert(sizeof(CUDA_MEMCPY2D) == 128 && offsetof(CUDA_MEMCPY2D, dstXInBytes) == 56);
#endif

using cuInit_t = CUresult (*)(unsigned flags);
using cuDriverGetVersion_t = CUresult (*)(int* version);
using cuGetErrorName_t = CUresult (*)(CUresult error, const char** name);
using cuDeviceGetCount_t = CUresult (*)(int* count);
using cuDeviceGet_t = CUresult (*)(CUdevice* device, int ordinal);
using cuDeviceGetByPCIBusId_t = CUresult (*)(CUdevice* device, const char* bus_id);
using cuDeviceGetPCIBusId_t = CUresult (*)(char* bus_id, int length, CUdevice device);
using cuDeviceGetName_t = CUresult (*)(char* name, int length, CUdevice device);
using cuCtxCreate_v2_t = CUresult (*)(CUcontext* context, unsigned flags, CUdevice device);
using cuCtxDestroy_v2_t = CUresult (*)(CUcontext context);
using cuCtxPushCurrent_v2_t = CUresult (*)(CUcontext context);
using cuCtxPopCurrent_v2_t = CUresult (*)(CUcontext* context);
using cuCtxSynchronize_t = CUresult (*)();
using cuMemAllocPitch_v2_t = CUresult (*)(CUdeviceptr* pointer, std::size_t* pitch, std::size_t width_bytes,
                                          std::size_t height, unsigned element_size);
using cuMemFree_v2_t = CUresult (*)(CUdeviceptr pointer);
using cuMemsetD8_v2_t = CUresult (*)(CUdeviceptr pointer, unsigned char value, std::size_t count);
using cuMemcpy2D_v2_t = CUresult (*)(const CUDA_MEMCPY2D* copy);
using cuModuleLoadDataEx_t = CUresult (*)(CUmodule* module, const void* image, unsigned options, int* option_keys,
                                          void** option_values);
using cuModuleUnload_t = CUresult (*)(CUmodule module);
using cuModuleGetFunction_t = CUresult (*)(CUfunction* function, CUmodule module, const char* name);
using cuLaunchKernel_t = CUresult (*)(CUfunction function, unsigned grid_x, unsigned grid_y, unsigned grid_z,
                                      unsigned block_x, unsigned block_y, unsigned block_z, unsigned shared_bytes,
                                      CUstream stream, void** params, void** extra);
using cuGraphicsGLRegisterImage_t = CUresult (*)(CUgraphicsResource* resource, unsigned texture, unsigned target,
                                                 unsigned flags);
using cuGraphicsUnregisterResource_t = CUresult (*)(CUgraphicsResource resource);
using cuGraphicsMapResources_t = CUresult (*)(unsigned count, CUgraphicsResource* resources, CUstream stream);
using cuGraphicsUnmapResources_t = CUresult (*)(unsigned count, CUgraphicsResource* resources, CUstream stream);
using cuGraphicsSubResourceGetMappedArray_t = CUresult (*)(CUarray* array, CUgraphicsResource resource, unsigned index,
                                                           unsigned level);

// ---------------------------------------------------------------------------
// EGL 1.5 with EGL_EXT_device_enumeration, EGL_EXT_platform_device,
// EGL_NV_device_cuda, EGL_KHR_no_config_context, EGL_KHR_image_base and
// EGL_EXT_image_dma_buf_import(_modifiers).
// ---------------------------------------------------------------------------

using EGLint = std::int32_t;
using EGLAttrib = std::intptr_t;
using EGLBoolean = unsigned;
using EGLenum = unsigned;
using EGLDisplay = void*;
using EGLContext = void*;
using EGLSurface = void*;
using EGLConfig = void*;
using EGLDeviceEXT = void*;
using EGLImageKHR = void*;

inline constexpr EGLint egl_success = 0x3000;
inline constexpr EGLint egl_none = 0x3038;
inline constexpr EGLint egl_extensions = 0x3055;
inline constexpr EGLint egl_height = 0x3056;
inline constexpr EGLint egl_width = 0x3057;
inline constexpr EGLenum egl_opengl_api = 0x30A2;
inline constexpr EGLint egl_image_preserved_khr = 0x30D2;
inline constexpr EGLenum egl_platform_device_ext = 0x313F;
inline constexpr EGLint egl_cuda_device_nv = 0x323A;
inline constexpr EGLenum egl_linux_dma_buf_ext = 0x3270;
inline constexpr EGLint egl_linux_drm_fourcc_ext = 0x3271;
/// EGL_DMA_BUF_PLANEn_FD_EXT, _OFFSET_EXT, _PITCH_EXT, _MODIFIER_LO_EXT, _MODIFIER_HI_EXT.
struct DmaBufPlaneAttributes {
    EGLint fd;
    EGLint offset;
    EGLint pitch;
    EGLint modifier_lo;
    EGLint modifier_hi;
};
inline constexpr std::array<DmaBufPlaneAttributes, 4> egl_dma_buf_plane{{
    {0x3272, 0x3273, 0x3274, 0x3443, 0x3444},
    {0x3275, 0x3276, 0x3277, 0x3445, 0x3446},
    {0x3278, 0x3279, 0x327A, 0x3447, 0x3448},
    {0x3440, 0x3441, 0x3442, 0x3449, 0x344A},
}};

using eglGetProcAddress_t = void (*(*)(const char* name))();
using eglGetError_t = EGLint (*)();
using eglInitialize_t = EGLBoolean (*)(EGLDisplay display, EGLint* major, EGLint* minor);
using eglTerminate_t = EGLBoolean (*)(EGLDisplay display);
using eglQueryString_t = const char* (*)(EGLDisplay display, EGLint name);
using eglBindAPI_t = EGLBoolean (*)(EGLenum api);
using eglCreateContext_t = EGLContext (*)(EGLDisplay display, EGLConfig config, EGLContext share,
                                          const EGLint* attributes);
using eglDestroyContext_t = EGLBoolean (*)(EGLDisplay display, EGLContext context);
using eglMakeCurrent_t = EGLBoolean (*)(EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context);
using eglGetCurrentContext_t = EGLContext (*)();
using eglGetCurrentDisplay_t = EGLDisplay (*)();
using eglGetCurrentSurface_t = EGLSurface (*)(EGLint which);
inline constexpr EGLint egl_draw = 0x3059;
inline constexpr EGLint egl_read = 0x305A;
using eglQueryDevicesEXT_t = EGLBoolean (*)(EGLint max, EGLDeviceEXT* devices, EGLint* count);
using eglQueryDeviceAttribEXT_t = EGLBoolean (*)(EGLDeviceEXT device, EGLint attribute, EGLAttrib* value);
using eglQueryDeviceStringEXT_t = const char* (*)(EGLDeviceEXT device, EGLint name);
using eglGetPlatformDisplayEXT_t = EGLDisplay (*)(EGLenum platform, void* native, const EGLint* attributes);
using eglCreateImageKHR_t = EGLImageKHR (*)(EGLDisplay display, EGLContext context, EGLenum target, void* buffer,
                                            const EGLint* attributes);
using eglDestroyImageKHR_t = EGLBoolean (*)(EGLDisplay display, EGLImageKHR image);

// ---------------------------------------------------------------------------
// OpenGL 4.3 core (glCopyImageSubData, glTexStorage2D) and OES_EGL_image(_external).
// ---------------------------------------------------------------------------

inline constexpr unsigned gl_no_error = 0;
inline constexpr unsigned gl_texture_2d = 0x0DE1;
inline constexpr unsigned gl_texture_external_oes = 0x8D65;
inline constexpr unsigned gl_texture_mag_filter = 0x2800;
inline constexpr unsigned gl_texture_min_filter = 0x2801;
inline constexpr int gl_nearest = 0x2600;
inline constexpr unsigned gl_rgba8 = 0x8058;

using glGetError_t = unsigned (*)();
using glGenTextures_t = void (*)(int count, unsigned* textures);
using glDeleteTextures_t = void (*)(int count, const unsigned* textures);
using glBindTexture_t = void (*)(unsigned target, unsigned texture);
using glTexParameteri_t = void (*)(unsigned target, unsigned name, int value);
using glTexStorage2D_t = void (*)(unsigned target, int levels, unsigned format, int width, int height);
using glCopyImageSubData_t = void (*)(unsigned src, unsigned src_target, int src_level, int src_x, int src_y, int src_z,
                                      unsigned dst, unsigned dst_target, int dst_level, int dst_x, int dst_y, int dst_z,
                                      int width, int height, int depth);
using glFlush_t = void (*)();
using glFinish_t = void (*)();
using glEGLImageTargetTexture2DOES_t = void (*)(unsigned target, void* image);

}  // namespace farland::video::nvenc::abi
