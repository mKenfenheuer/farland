# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# H.264 encoder backends (docs/PLAN.md §3.1). No H.264 library is needed to
# build: OpenH264 is loaded at runtime, and x264 (GPLv2+) is opt-in.
#
# This is glue around C libraries that hand out raw pointers, so it is not
# built with the safe-buffer warnings; the stream parsers live in farland-codec.

# dlopen: part of libc since glibc 2.34, libdl before.
dl_dep = dependency('dl', required: false)
x264_dep = dependency('x264', required: get_option('x264'))

video_sources = ['h264_encoder.cpp', 'openh264_encoder.cpp']
video_args = []
if x264_dep.found()
  video_sources += 'x264_encoder.cpp'
  video_args += '-DFARLAND_VIDEO_HAVE_X264=1'
  warning('x264 is enabled: libx264 is GPLv2+, so the farland binaries built here must be distributed under the GPL')
endif

farland_video_lib = static_library(
  'farland-video',
  video_sources,
  include_directories: farland_inc,
  dependencies: [farland_codec_dep, dl_dep, x264_dep],
  cpp_args: video_args,
)

farland_video_dep = declare_dependency(
  link_with: farland_video_lib,
  include_directories: farland_inc,
  dependencies: [farland_codec_dep, dl_dep, x264_dep],
)

summary(
  {
    'OpenH264 (runtime-loaded)': true,
    'x264': x264_dep.found() ? 'YES - libx264 is GPLv2+: binaries of this build are GPL, not Apache-2.0' : false,
  },
  section: 'H.264 encoders',
  bool_yn: true,
)
