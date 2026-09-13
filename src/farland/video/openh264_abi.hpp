// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>

/// The part of OpenH264's encoder ABI that farland uses.
///
/// farland loads OpenH264 at runtime (docs/PLAN.md §3.1, §7) and must work
/// with whichever release the system or the user installed, and those
/// releases do not share one layout:
///
/// | soname | releases  | change                                                         |
/// |--------|-----------|----------------------------------------------------------------|
/// | 6      | 2.1 - 2.2 | baseline for farland                                           |
/// | 7      | 2.3 - 2.5 | SEncParamExt gains bFixRCOverShoot, iIdrBitrateRatio at the end |
/// | 8      | 2.6       | SEncParamExt and SSourcePicture gain bPsnrY/U/V at the end;     |
/// |        |           | SLayerBSInfo gains rPsnr[3], which moves every field of         |
/// |        |           | SFrameBSInfo after sLayerInfo[0]                               |
///
/// Compiling against one release's headers would therefore misread the
/// output of another, so this header declares both layouts and the encoder
/// picks one from WelsGetCodecVersionEx. Structures the library only reads
/// from, or only fills a prefix of, use the newest layout.
///
/// The declarations follow OpenH264's public headers codec_api.h,
/// codec_app_def.h and codec_def.h (Cisco Systems, BSD-2-Clause) and keep
/// their field names, so tests/video/test_openh264_abi.cpp can compare
/// offsets with the real headers where they are installed. Enumerations are
/// int-sized in the library and appear here as int constants.
namespace farland::video::openh264::abi {

// EUsageType
inline constexpr int screen_content_real_time = 1;
// RC_MODES
inline constexpr int rc_quality_mode = 0;
inline constexpr int rc_bitrate_mode = 1;
inline constexpr int rc_off_mode = -1;
// EParameterSetStrategy
inline constexpr int constant_id = 0;
// SliceModeEnum
inline constexpr int sm_single_slice = 0;
inline constexpr int sm_fixedslcnum_slice = 1;
// EProfileIdc
inline constexpr int pro_baseline = 66;
// EVideoFormatType
inline constexpr int video_format_i420 = 23;
// EVideoFrameType
inline constexpr int video_frame_type_invalid = 0;
inline constexpr int video_frame_type_idr = 1;
inline constexpr int video_frame_type_i = 2;
inline constexpr int video_frame_type_p = 3;
inline constexpr int video_frame_type_skip = 4;
// CM_RETURN
inline constexpr int cm_result_success = 0;
// LAYER_NUM
inline constexpr int spatial_layer_all = 4;
// Log levels (WELS_LOG_*).
inline constexpr int wels_log_error = 1 << 0;
inline constexpr int wels_log_warning = 1 << 1;
// VUI values: EVideoFormatSPS, EColorPrimaries, ETransferCharacteristics, EColorMatrix.
inline constexpr unsigned char vf_undef = 5;
inline constexpr unsigned char cp_bt709 = 1;
inline constexpr unsigned char trc_iec61966_2_1 = 13;  // sRGB
inline constexpr unsigned char cm_bt709 = 1;

/// ENCODER_OPTION (codec_app_def.h numbers them consecutively from 0).
namespace option {
inline constexpr int svc_encode_param_ext = 3;
inline constexpr int bitrate = 5;
inline constexpr int max_bitrate = 6;
inline constexpr int trace_level = 25;
inline constexpr int trace_callback = 26;
inline constexpr int get_statistics = 28;
}  // namespace option

inline constexpr int max_spatial_layer_num = 4;
inline constexpr int max_layer_num_of_frame = 128;
inline constexpr int max_slices_num_tmp = 35;  // (128 - (4 * 4 + 1 + 4)) / 3

struct OpenH264Version {
    unsigned int uMajor;
    unsigned int uMinor;
    unsigned int uRevision;
    unsigned int uReserved;
};

struct SSliceArgument {
    int uiSliceMode;
    unsigned int uiSliceNum;
    std::array<unsigned int, max_slices_num_tmp> uiSliceMbNum;
    unsigned int uiSliceSizeConstraint;
};

struct SSpatialLayerConfig {
    int iVideoWidth;
    int iVideoHeight;
    float fFrameRate;
    int iSpatialBitrate;
    int iMaxSpatialBitrate;
    int uiProfileIdc;
    int uiLevelIdc;
    int iDLayerQp;
    SSliceArgument sSliceArgument;
    bool bVideoSignalTypePresent;
    unsigned char uiVideoFormat;
    bool bFullRange;
    bool bColorDescriptionPresent;
    unsigned char uiColorPrimaries;
    unsigned char uiTransferCharacteristics;
    unsigned char uiColorMatrix;
    bool bAspectRatioPresent;
    int eAspectRatio;
    unsigned short sAspectRatioExtWidth;
    unsigned short sAspectRatioExtHeight;
};

/// 2.6 layout. Older releases read and write a prefix of it.
struct SEncParamExt {
    int iUsageType;
    int iPicWidth;
    int iPicHeight;
    int iTargetBitrate;
    int iRCMode;
    float fMaxFrameRate;
    int iTemporalLayerNum;
    int iSpatialLayerNum;
    std::array<SSpatialLayerConfig, max_spatial_layer_num> sSpatialLayers;
    int iComplexityMode;
    unsigned int uiIntraPeriod;
    int iNumRefFrame;
    int eSpsPpsIdStrategy;
    bool bPrefixNalAddingCtrl;
    bool bEnableSSEI;
    bool bSimulcastAVC;
    int iPaddingFlag;
    int iEntropyCodingModeFlag;
    bool bEnableFrameSkip;
    int iMaxBitrate;
    int iMaxQp;
    int iMinQp;
    unsigned int uiMaxNalSize;
    bool bEnableLongTermReference;
    int iLTRRefNum;
    unsigned int iLtrMarkPeriod;
    unsigned short iMultipleThreadIdc;
    bool bUseLoadBalancing;
    int iLoopFilterDisableIdc;
    int iLoopFilterAlphaC0Offset;
    int iLoopFilterBetaOffset;
    bool bEnableDenoise;
    bool bEnableBackgroundDetection;
    bool bEnableAdaptiveQuant;
    bool bEnableFrameCroppingFlag;
    bool bEnableSceneChangeDetect;
    bool bIsLosslessLink;
    // Since 2.3 (soname 7).
    bool bFixRCOverShoot;
    int iIdrBitrateRatio;
    // Since 2.6 (soname 8).
    bool bPsnrY;
    bool bPsnrU;
    bool bPsnrV;
};

/// 2.6 layout. Older releases read a prefix of it.
struct SSourcePicture {
    int iColorFormat;
    std::array<int, 4> iStride;
    std::array<unsigned char*, 4> pData;
    int iPicWidth;
    int iPicHeight;
    long long uiTimeStamp;
    // Since 2.6.
    bool bPsnrY;
    bool bPsnrU;
    bool bPsnrV;
};

/// SLayerBSInfo up to 2.5.
struct SLayerBSInfo21 {
    unsigned char uiTemporalId;
    unsigned char uiSpatialId;
    unsigned char uiQualityId;
    int eFrameType;
    unsigned char uiLayerType;
    int iSubSeqId;
    int iNalCount;
    int* pNalLengthInByte;
    unsigned char* pBsBuf;
};

/// SLayerBSInfo since 2.6.
struct SLayerBSInfo26 {
    unsigned char uiTemporalId;
    unsigned char uiSpatialId;
    unsigned char uiQualityId;
    int eFrameType;
    unsigned char uiLayerType;
    int iSubSeqId;
    int iNalCount;
    int* pNalLengthInByte;
    unsigned char* pBsBuf;
    std::array<float, 3> rPsnr;
};

/// SFrameBSInfo, with the SLayerBSInfo of the loaded release.
template <class Layer>
struct SFrameBSInfo {
    int iLayerNum;
    std::array<Layer, max_layer_num_of_frame> sLayerInfo;
    int eFrameType;
    int iFrameSizeInBytes;
    long long uiTimeStamp;
};

struct SBitrateInfo {
    int iLayer;
    int iBitrate;
};

struct SEncoderStatistics {
    unsigned int uiWidth;
    unsigned int uiHeight;
    float fAverageFrameSpeedInMs;
    float fAverageFrameRate;
    float fLatestFrameRate;
    unsigned int uiBitRate;
    unsigned int uiAverageFrameQP;
    unsigned int uiInputFrameCount;
    unsigned int uiSkippedFrameCount;
    unsigned int uiResolutionChangeTimes;
    unsigned int uiIDRReqNum;
    unsigned int uiIDRSentNum;
    unsigned int uiLTRSentNum;
    long long iStatisticsTs;
    unsigned long iTotalEncodedBytes;
    unsigned long iLastStatisticsBytes;
    unsigned long iLastStatisticsFrameCount;
};

/// An ISVCEncoder object starts with a pointer to its vtable. OpenH264
/// implements it as a C++ class; these entries are its virtual methods in
/// declaration order, with `this` as the first argument (the Itanium ABI
/// that codec_api.h's C declarations also rely on).
struct ISVCEncoderVtbl;
using ISVCEncoder = const ISVCEncoderVtbl*;

struct ISVCEncoderVtbl {
    int (*Initialize)(ISVCEncoder* self, const void* param);
    int (*InitializeExt)(ISVCEncoder* self, const SEncParamExt* param);
    int (*GetDefaultParams)(ISVCEncoder* self, SEncParamExt* param);
    int (*Uninitialize)(ISVCEncoder* self);
    int (*EncodeFrame)(ISVCEncoder* self, const SSourcePicture* picture, void* frame_bs_info);
    int (*EncodeParameterSets)(ISVCEncoder* self, void* frame_bs_info);
    /// The C++ method is ForceIntraFrame(bool bIDR, int iLayerId = -1) and
    /// passes iLayerId on to ForceCodingIDR. codec_api.h's C view omits
    /// iLayerId, so C callers leave it undefined; farland passes -1.
    int (*ForceIntraFrame)(ISVCEncoder* self, bool idr, int layer);
    int (*SetOption)(ISVCEncoder* self, int option, void* value);
    int (*GetOption)(ISVCEncoder* self, int option, void* value);
};

using WelsTraceCallback = void (*)(void* context, int level, const char* message);
using WelsCreateSVCEncoderFn = int (*)(ISVCEncoder** encoder);
using WelsDestroySVCEncoderFn = void (*)(ISVCEncoder* encoder);
using WelsGetCodecVersionExFn = void (*)(OpenH264Version* version);

}  // namespace farland::video::openh264::abi
