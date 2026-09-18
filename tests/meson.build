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
subdir('audio')
subdir('channels')
subdir('auth')
subdir('platform')
subdir('platform/portal')
subdir('platform/kwin')
subdir('platform/mutter')
subdir('platform/wlroots')
subdir('server')
subdir('integration')
subdir('apps')

# PipeWire's pkg-config hands every user -fno-strict-overflow (libspa-0.2.pc),
# and GCC then miscompiles the RemoteFX reference encoder: GCC 16 gets
# std::bit_width wrong at -O2, GCC 15 a hand-written equivalent at -O3. The
# flag says nothing our code needs, so the last word is ours. Dependency
# arguments come before a target's own, so this cancels it.
unit_test_args = [
  '-DFARLAND_TEST_DATA_DIR="@0@"'.format(meson.current_source_dir() / 'data'),
  # data/farland.toml, the file the package installs as /etc/farland/farland.toml.
  '-DFARLAND_SOURCE_DATA_DIR="@0@"'.format(meson.project_source_root() / 'data'),
]
unit_test_args += cxx.get_supported_arguments('-fstrict-overflow')

unit_tests = executable(
  'farland-unit-tests',
  unit_test_sources,
  dependencies: unit_test_deps,
  cpp_args: unit_test_args,
)

# The privilege-separation test starts farland-server as the network process.
test(
  'unit',
  unit_tests,
  suite: 'unit',
  # The codec round trips take over 30 s unoptimised (debug and sanitizer
  # builds on small CI machines).
  timeout: 300,
  env: {
    'FARLAND_SERVER': farland_server_exe.full_path(),
    'FARLAND_DAEMON': farlandd_exe.full_path(),
    'FARLAND_AGENT': farland_agent_exe.full_path(),
  },
  depends: [farland_server_exe, farlandd_exe, farland_agent_exe],
)
