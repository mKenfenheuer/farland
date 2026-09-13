// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Built only where OpenH264's headers are installed (tests/video/meson.build):
// checks at compile time that openh264_abi.hpp matches the installed release.
// To check another release, point pkg-config at its headers.

#include <farland/video/openh264_abi.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <type_traits>
#include <wels/codec_api.h>
#include <wels/codec_app_def.h>
#include <wels/codec_def.h>
#include <wels/codec_ver.h>

namespace abi = farland::video::openh264::abi;

#define FARLAND_SAME_FIELD(ours, theirs, field)                                                                        \
    static_assert(offsetof(ours, field) == offsetof(theirs, field), #theirs "::" #field)
#define FARLAND_SAME_SIZE(ours, theirs) static_assert(sizeof(ours) == sizeof(theirs), #theirs)

// Constants.
static_assert(abi::screen_content_real_time == SCREEN_CONTENT_REAL_TIME);
static_assert(abi::rc_quality_mode == RC_QUALITY_MODE && abi::rc_bitrate_mode == RC_BITRATE_MODE &&
              abi::rc_off_mode == RC_OFF_MODE);
static_assert(abi::constant_id == CONSTANT_ID);
static_assert(abi::sm_single_slice == SM_SINGLE_SLICE && abi::sm_fixedslcnum_slice == SM_FIXEDSLCNUM_SLICE);
static_assert(abi::pro_baseline == PRO_BASELINE);
static_assert(abi::video_format_i420 == videoFormatI420);
static_assert(abi::video_frame_type_invalid == videoFrameTypeInvalid &&
              abi::video_frame_type_idr == videoFrameTypeIDR && abi::video_frame_type_i == videoFrameTypeI &&
              abi::video_frame_type_p == videoFrameTypeP && abi::video_frame_type_skip == videoFrameTypeSkip);
static_assert(abi::cm_result_success == cmResultSuccess);
static_assert(abi::spatial_layer_all == SPATIAL_LAYER_ALL);
static_assert(abi::wels_log_error == WELS_LOG_ERROR && abi::wels_log_warning == WELS_LOG_WARNING);
static_assert(abi::vf_undef == VF_UNDEF && abi::cp_bt709 == CP_BT709 && abi::trc_iec61966_2_1 == TRC_IEC61966_2_1 &&
              abi::cm_bt709 == CM_BT709);
static_assert(abi::option::svc_encode_param_ext == ENCODER_OPTION_SVC_ENCODE_PARAM_EXT);
static_assert(abi::option::bitrate == ENCODER_OPTION_BITRATE);
static_assert(abi::option::max_bitrate == ENCODER_OPTION_MAX_BITRATE);
static_assert(abi::option::trace_level == ENCODER_OPTION_TRACE_LEVEL);
static_assert(abi::option::trace_callback == ENCODER_OPTION_TRACE_CALLBACK);
static_assert(abi::option::get_statistics == ENCODER_OPTION_GET_STATISTICS);
static_assert(abi::max_spatial_layer_num == MAX_SPATIAL_LAYER_NUM);
static_assert(abi::max_layer_num_of_frame == MAX_LAYER_NUM_OF_FRAME);
static_assert(abi::max_slices_num_tmp == MAX_SLICES_NUM_TMP);
static_assert(std::is_same_v<abi::WelsTraceCallback, WelsTraceCallback>);

// Structures the same in every supported release.
FARLAND_SAME_SIZE(abi::OpenH264Version, OpenH264Version);
FARLAND_SAME_FIELD(abi::OpenH264Version, OpenH264Version, uMinor);
FARLAND_SAME_FIELD(abi::OpenH264Version, OpenH264Version, uRevision);

FARLAND_SAME_SIZE(abi::SSliceArgument, SSliceArgument);
FARLAND_SAME_FIELD(abi::SSliceArgument, SSliceArgument, uiSliceNum);
FARLAND_SAME_FIELD(abi::SSliceArgument, SSliceArgument, uiSliceMbNum);
FARLAND_SAME_FIELD(abi::SSliceArgument, SSliceArgument, uiSliceSizeConstraint);

FARLAND_SAME_SIZE(abi::SSpatialLayerConfig, SSpatialLayerConfig);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, iVideoWidth);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, iVideoHeight);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, fFrameRate);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, iSpatialBitrate);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, iMaxSpatialBitrate);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, uiProfileIdc);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, uiLevelIdc);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, iDLayerQp);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, sSliceArgument);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, bVideoSignalTypePresent);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, uiVideoFormat);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, bFullRange);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, bColorDescriptionPresent);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, uiColorPrimaries);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, uiTransferCharacteristics);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, uiColorMatrix);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, bAspectRatioPresent);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, eAspectRatio);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, sAspectRatioExtWidth);
FARLAND_SAME_FIELD(abi::SSpatialLayerConfig, SSpatialLayerConfig, sAspectRatioExtHeight);

FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iUsageType);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iPicWidth);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iPicHeight);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iTargetBitrate);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iRCMode);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, fMaxFrameRate);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iTemporalLayerNum);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iSpatialLayerNum);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, sSpatialLayers);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iComplexityMode);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, uiIntraPeriod);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iNumRefFrame);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, eSpsPpsIdStrategy);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, bPrefixNalAddingCtrl);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, bEnableSSEI);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, bSimulcastAVC);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iPaddingFlag);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iEntropyCodingModeFlag);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, bEnableFrameSkip);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iMaxBitrate);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iMaxQp);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iMinQp);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, uiMaxNalSize);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, bEnableLongTermReference);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iLTRRefNum);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iLtrMarkPeriod);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iMultipleThreadIdc);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, bUseLoadBalancing);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iLoopFilterDisableIdc);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iLoopFilterAlphaC0Offset);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iLoopFilterBetaOffset);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, bEnableDenoise);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, bEnableBackgroundDetection);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, bEnableAdaptiveQuant);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, bEnableFrameCroppingFlag);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, bEnableSceneChangeDetect);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, bIsLosslessLink);

FARLAND_SAME_FIELD(abi::SSourcePicture, SSourcePicture, iColorFormat);
FARLAND_SAME_FIELD(abi::SSourcePicture, SSourcePicture, iStride);
FARLAND_SAME_FIELD(abi::SSourcePicture, SSourcePicture, pData);
FARLAND_SAME_FIELD(abi::SSourcePicture, SSourcePicture, iPicWidth);
FARLAND_SAME_FIELD(abi::SSourcePicture, SSourcePicture, iPicHeight);
FARLAND_SAME_FIELD(abi::SSourcePicture, SSourcePicture, uiTimeStamp);

FARLAND_SAME_SIZE(abi::SBitrateInfo, SBitrateInfo);
FARLAND_SAME_FIELD(abi::SBitrateInfo, SBitrateInfo, iBitrate);

FARLAND_SAME_SIZE(abi::SEncoderStatistics, SEncoderStatistics);
FARLAND_SAME_FIELD(abi::SEncoderStatistics, SEncoderStatistics, uiAverageFrameQP);
FARLAND_SAME_FIELD(abi::SEncoderStatistics, SEncoderStatistics, iStatisticsTs);
FARLAND_SAME_FIELD(abi::SEncoderStatistics, SEncoderStatistics, iLastStatisticsFrameCount);

// Structures that changed between releases.
#if OPENH264_MAJOR == 2 && OPENH264_MINOR >= 3
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, bFixRCOverShoot);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, iIdrBitrateRatio);
#endif

#if OPENH264_MAJOR == 2 && OPENH264_MINOR >= 6
using Layer = abi::SLayerBSInfo26;
FARLAND_SAME_FIELD(Layer, SLayerBSInfo, rPsnr);
FARLAND_SAME_SIZE(abi::SEncParamExt, SEncParamExt);
FARLAND_SAME_FIELD(abi::SEncParamExt, SEncParamExt, bPsnrV);
FARLAND_SAME_SIZE(abi::SSourcePicture, SSourcePicture);
FARLAND_SAME_FIELD(abi::SSourcePicture, SSourcePicture, bPsnrV);
#else
using Layer = abi::SLayerBSInfo21;
// The library reads and writes a prefix of the 2.6 layout.
static_assert(sizeof(SEncParamExt) <= sizeof(abi::SEncParamExt));
static_assert(sizeof(SSourcePicture) <= sizeof(abi::SSourcePicture));
#endif

FARLAND_SAME_SIZE(Layer, SLayerBSInfo);
FARLAND_SAME_FIELD(Layer, SLayerBSInfo, uiQualityId);
FARLAND_SAME_FIELD(Layer, SLayerBSInfo, eFrameType);
FARLAND_SAME_FIELD(Layer, SLayerBSInfo, uiLayerType);
FARLAND_SAME_FIELD(Layer, SLayerBSInfo, iSubSeqId);
FARLAND_SAME_FIELD(Layer, SLayerBSInfo, iNalCount);
FARLAND_SAME_FIELD(Layer, SLayerBSInfo, pNalLengthInByte);
FARLAND_SAME_FIELD(Layer, SLayerBSInfo, pBsBuf);

using Frame = abi::SFrameBSInfo<Layer>;
FARLAND_SAME_SIZE(Frame, SFrameBSInfo);
FARLAND_SAME_FIELD(Frame, SFrameBSInfo, sLayerInfo);
FARLAND_SAME_FIELD(Frame, SFrameBSInfo, eFrameType);
FARLAND_SAME_FIELD(Frame, SFrameBSInfo, iFrameSizeInBytes);
FARLAND_SAME_FIELD(Frame, SFrameBSInfo, uiTimeStamp);

TEST_CASE("openh264_abi.hpp matches the installed OpenH264 headers")
{
    // The checks above run at compile time.
    CAPTURE(OPENH264_MAJOR, OPENH264_MINOR);
    SUCCEED();
}
