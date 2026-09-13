// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/video/h264_encoder.hpp>

#include <memory>
#include <span>
#include <string_view>

/// The OpenH264 backend. The library is opened with dlopen when an encoder is
/// created, so farland neither needs OpenH264 to build nor links it:
/// distributions ship it separately, and Cisco's prebuilt binary is the one
/// whose H.264 patent licence Cisco pays (docs/PLAN.md §7).
///
/// Supported releases are 2.1 to 2.6 (sonames 6, 7 and 8), checked with
/// WelsGetCodecVersionEx; see openh264_abi.hpp for why each needs its own
/// layout. The output is Constrained Baseline with CAVLC, the profile
/// FreeRDP's own server sends and every client decodes.
namespace farland::video::openh264 {

/// Names tried in order when BackendOptions::openh264_library is empty.
[[nodiscard]] std::span<const std::string_view> default_library_names() noexcept;

/// Loads OpenH264 (`library`, or the default names) and opens an encoder.
/// A missing library or an unsupported release is Errc::unsupported; the
/// dlopen error or version is logged under "video.openh264".
[[nodiscard]] Result<std::unique_ptr<H264Encoder>> create(const EncoderConfig& config, std::string_view library = {});

}  // namespace farland::video::openh264
