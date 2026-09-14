# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# H.264 encoder backends (docs/PLAN.md §3.1). No H.264 library is needed to
# build: OpenH264 and NVENC are loaded at runtime, x264 (GPLv2+) is opt-in,
# and VA-API is used where libva is installed.
#
# This is glue around C libraries that hand out raw pointers, so it is not
# built with the safe-buffer warnings; the stream parsers live in farland-codec.

# dlopen: part of libc since glibc 2.34, libdl before.
dl_dep = dependency('dl', required: false)
x264_dep = dependency('x264', required: get_option('x264'))
# VA-API 1.14 is libva 2.14 (2022); vaSyncBuffer and DRM PRIME 2 are older.
# System headers: libva's structures have flexible array members.
vaapi_opt = get_option('vaapi')
libva_dep = dependency('libva', version: '>= 1.14', required: vaapi_opt, include_type: 'system')
libva_drm_dep = dependency('libva-drm', version: '>= 1.14', required: vaapi_opt, include_type: 'system')
have_vaapi = libva_dep.found() and libva_drm_dep.found()
# NVENC: libnvidia-encode, libcuda and libEGL come with the NVIDIA driver and
# are loaded at runtime, so there is nothing to look for; the option only
# decides whether the backend is compiled in.
have_nvenc = get_option('nvenc').require(
  host_machine.system() == 'linux',
  error_message: 'the NVENC backend is Linux-only',
).allowed()

video_sources = ['avc444_encoder.cpp', 'h264_bitstream.cpp', 'h264_encoder.cpp', 'openh264_encoder.cpp']
video_args = []
video_public_args = []
video_deps = [farland_codec_dep, dl_dep, x264_dep]
if x264_dep.found()
  video_sources += 'x264_encoder.cpp'
  video_args += '-DFARLAND_VIDEO_HAVE_X264=1'
  warning('x264 is enabled: libx264 is GPLv2+, so the farland binaries built here must be distributed under the GPL')
endif
if have_vaapi
  video_sources += 'vaapi_encoder.cpp'
  video_public_args += '-DFARLAND_HAVE_VAAPI=1'
  video_deps += [libva_dep, libva_drm_dep]
endif
if have_nvenc
  video_sources += 'nvenc_encoder.cpp'
  video_public_args += '-DFARLAND_HAVE_NVENC=1'
endif

farland_video_lib = static_library(
  'farland-video',
  video_sources,
  include_directories: farland_inc,
  dependencies: video_deps,
  cpp_args: video_args + video_public_args,
)

farland_video_dep = declare_dependency(
  link_with: farland_video_lib,
  include_directories: farland_inc,
  compile_args: video_public_args,
  dependencies: video_deps,
)

summary(
  {
    'OpenH264 (runtime-loaded)': true,
    'x264': x264_dep.found() ? 'YES - libx264 is GPLv2+: binaries of this build are GPL, not Apache-2.0' : false,
    'VA-API': have_vaapi,
    'NVENC (runtime-loaded)': have_nvenc,
  },
  section: 'H.264 encoders',
  bool_yn: true,
)
