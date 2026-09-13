# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

unit_test_sources += files('test_h264_encoder.cpp')
unit_test_deps += farland_video_dep

# Where OpenH264's headers are installed, check openh264_abi.hpp against them.
openh264_headers = dependency('openh264', required: false)
if openh264_headers.found()
  unit_test_sources += files('test_openh264_abi.cpp')
  unit_test_deps += openh264_headers.partial_dependency(compile_args: true, includes: true)
endif
