# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# PNG on the clipboard (png.cpp) takes deflate from zlib; without it images
# travel as BMP only.
zlib_dep = dependency('zlib', required: get_option('png'))
codec_args = farland_safe_buffer_args
codec_deps = [farland_base_dep]
if zlib_dep.found()
  codec_args += '-DFARLAND_HAVE_ZLIB=1'
  codec_deps += zlib_dep
endif

farland_codec_lib = static_library(
  'farland-codec',
  'avc420.cpp',
  'clear_decoder.cpp',
  'clear_encoder.cpp',
  'dib.cpp',
  'h264_nal.cpp',
  'planar.cpp',
  'png.cpp',
  'progressive_decoder.cpp',
  'progressive_encoder.cpp',
  'rfx_common.cpp',
  'rfx_kernels.cpp',
  'yuv.cpp',
  'yuv444.cpp',
  'zgfx.cpp',
  include_directories: farland_inc,
  dependencies: codec_deps,
  cpp_args: codec_args,
)

farland_codec_dep = declare_dependency(
  link_with: farland_codec_lib,
  include_directories: farland_inc,
  dependencies: codec_deps,
)

summary({'PNG (zlib)': zlib_dep.found()}, section: 'Clipboard', bool_yn: true)
