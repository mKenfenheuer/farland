# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

unit_test_sources += files(
  'h264_test_support.cpp',
  'test_avc444_encoder.cpp',
  'test_h264_bitstream.cpp',
  'test_h264_encoder.cpp',
)
unit_test_deps += farland_video_dep

# Where OpenH264's headers are installed, check openh264_abi.hpp against them.
openh264_headers = dependency('openh264', required: false)
if openh264_headers.found()
  unit_test_sources += files('test_openh264_abi.cpp')
  unit_test_deps += openh264_headers.partial_dependency(compile_args: true, includes: true)
endif

# VA-API: the tests make dmabufs through libva itself and, with GBM, through
# GBM. The benchmark is run by hand (farland-vaapi-bench --help).
if have_vaapi
  vaapi_test_args = []
  vaapi_test_deps = [farland_video_dep, libva_dep, libva_drm_dep]
  test_gbm_dep = dependency('gbm', required: false, include_type: 'system')
  if test_gbm_dep.found()
    vaapi_test_args += '-DFARLAND_TEST_HAVE_GBM=1'
    vaapi_test_deps += test_gbm_dep
  endif
  vaapi_test_source_lib = static_library(
    'farland-vaapi-test-source',
    'vaapi_test_source.cpp',
    cpp_args: vaapi_test_args,
    dependencies: vaapi_test_deps,
  )
  vaapi_test_source_dep = declare_dependency(link_with: vaapi_test_source_lib, dependencies: vaapi_test_deps)
  unit_test_sources += files('test_vaapi_encoder.cpp')
  unit_test_deps += vaapi_test_source_dep

  executable(
    'farland-vaapi-bench',
    'vaapi_bench.cpp',
    dependencies: [vaapi_test_source_dep, farland_codec_dep],
  )
endif

# NVENC: the tests and the benchmark make dmabufs with GBM on the NVIDIA
# render node and fill tiled ones through EGL and OpenGL. The benchmark is
# run by hand (see the top of nvenc_bench.cpp).
if have_nvenc
  nvenc_test_args = []
  nvenc_test_deps = [farland_video_dep]
  nvenc_gbm_dep = dependency('gbm', required: false, include_type: 'system')
  nvenc_egl_dep = dependency('egl', required: false, include_type: 'system')
  if nvenc_gbm_dep.found() and nvenc_egl_dep.found()
    nvenc_test_args += '-DFARLAND_TEST_HAVE_GBM=1'
    nvenc_test_deps += [nvenc_gbm_dep, nvenc_egl_dep]
  endif
  nvenc_test_source_lib = static_library(
    'farland-nvenc-test-source',
    'nvenc_test_source.cpp',
    cpp_args: nvenc_test_args,
    dependencies: nvenc_test_deps,
  )
  nvenc_test_source_dep = declare_dependency(link_with: nvenc_test_source_lib, dependencies: nvenc_test_deps)
  unit_test_sources += files('test_nvenc_encoder.cpp')
  unit_test_deps += nvenc_test_source_dep

  # nvenc_abi.hpp against eglext.h and, where FFmpeg's nv-codec-headers are
  # installed, nvEncodeAPI.h.
  if nvenc_egl_dep.found()
    unit_test_sources += files('test_nvenc_abi.cpp')
    unit_test_deps += nvenc_egl_dep.partial_dependency(compile_args: true, includes: true)
    ffnvcodec_dep = dependency('ffnvcodec', required: false)
    if ffnvcodec_dep.found()
      unit_test_deps += declare_dependency(
        compile_args: '-DFARLAND_TEST_HAVE_FFNVCODEC=1',
        dependencies: ffnvcodec_dep.partial_dependency(compile_args: true, includes: true),
      )
    endif
  endif

  executable(
    'farland-nvenc-bench',
    'nvenc_bench.cpp',
    dependencies: [nvenc_test_source_dep, farland_codec_dep],
  )
endif
