# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

test_support_lib = static_library(
  'farland-test-support',
  'support/bytes.cpp',
  'support/transcript.cpp',
  dependencies: farland_base_dep,
)

test_support_dep = declare_dependency(
  link_with: test_support_lib,
  include_directories: include_directories('.'),
  dependencies: farland_base_dep,
)

unit_tests = executable(
  'farland-unit-tests',
  'base/test_ber.cpp',
  'base/test_hexdump.cpp',
  'base/test_log.cpp',
  'base/test_per.cpp',
  'base/test_reader.cpp',
  'base/test_writer.cpp',
  'support/test_transcript.cpp',
  dependencies: [test_support_dep, catch2_dep],
  cpp_args: '-DFARLAND_TEST_DATA_DIR="@0@"'.format(meson.current_source_dir() / 'data'),
)

test('unit', unit_tests, suite: 'unit')
