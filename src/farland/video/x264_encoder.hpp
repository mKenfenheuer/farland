// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/video/h264_encoder.hpp>

#include <memory>

/// The x264 backend, compiled only with -Dx264=enabled. libx264 is GPLv2+,
/// so a farland binary that contains it must be distributed under the GPL
/// (GPLv3, the version Apache-2.0 code can be combined with). It is never
/// part of the default build (docs/PLAN.md §2).
///
/// Settings follow the ZeroVDI libx264 bridge: preset veryfast with the
/// zerolatency tune, no B-frames, one reference frame, no scene-cut IDRs,
/// SPS/PPS on every IDR, AUD, Annex B, and CRF with a VBV cap. Threads are
/// slice threads, which add no frame delay.
namespace farland::video::x264 {

[[nodiscard]] Result<std::unique_ptr<H264Encoder>> create(const EncoderConfig& config);

}  // namespace farland::video::x264
