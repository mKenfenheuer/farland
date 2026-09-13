# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

farland_codec_lib = static_library(
  'farland-codec',
  'planar.cpp',
  include_directories: farland_inc,
  dependencies: farland_base_dep,
  cpp_args: farland_safe_buffer_args,
)

farland_codec_dep = declare_dependency(
  link_with: farland_codec_lib,
  include_directories: farland_inc,
  dependencies: farland_base_dep,
)
