# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

farland_codec_lib = static_library(
  'farland-codec',
  'avc420.cpp',
  'h264_nal.cpp',
  'planar.cpp',
  'progressive_decoder.cpp',
  'progressive_encoder.cpp',
  'rfx_common.cpp',
  'yuv.cpp',
  'zgfx.cpp',
  include_directories: farland_inc,
  dependencies: farland_base_dep,
  cpp_args: farland_safe_buffer_args,
)

farland_codec_dep = declare_dependency(
  link_with: farland_codec_lib,
  include_directories: farland_inc,
  dependencies: farland_base_dep,
)
