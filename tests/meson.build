# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

test_support_lib = static_library(
  'farland-test-support',
  'support/bytes.cpp',
  'support/client_pdus.cpp',
  'support/transcript.cpp',
  dependencies: farland_proto_dep,
)

test_support_dep = declare_dependency(
  link_with: test_support_lib,
  include_directories: include_directories('.'),
  dependencies: farland_proto_dep,
)

# Each library's test directory appends its sources and dependencies here.
unit_test_sources = files('support/test_transcript.cpp')
unit_test_deps = [test_support_dep, catch2_dep]

subdir('base')
subdir('proto')
subdir('codec')
subdir('video')
subdir('channels')
subdir('auth')
subdir('platform')
subdir('platform/portal')
subdir('server')
subdir('integration')

unit_tests = executable(
  'farland-unit-tests',
  unit_test_sources,
  dependencies: unit_test_deps,
  cpp_args: '-DFARLAND_TEST_DATA_DIR="@0@"'.format(meson.current_source_dir() / 'data'),
)

# The privilege-separation test starts farland-server as the network process.
test(
  'unit',
  unit_tests,
  suite: 'unit',
  env: {'FARLAND_SERVER': farland_server_exe.full_path()},
  depends: farland_server_exe,
)
