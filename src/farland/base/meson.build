# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

farland_base_lib = static_library(
  'farland-base',
  'ber.cpp',
  'error.cpp',
  'hexdump.cpp',
  'log.cpp',
  'per.cpp',
  include_directories: farland_inc,
  cpp_args: farland_safe_buffer_args,
)

farland_base_dep = declare_dependency(
  link_with: farland_base_lib,
  include_directories: farland_inc,
)
