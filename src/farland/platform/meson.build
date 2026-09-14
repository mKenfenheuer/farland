# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# Capture and input backends (docs/PLAN.md §3.3): the portable part, with the
# backend interface, the scancode table and the input translator.
farland_platform_lib = static_library(
  'farland-platform',
  'input_translator.cpp',
  'keymap.cpp',
  include_directories: farland_inc,
  dependencies: [farland_proto_dep, farland_channels_dep],
  cpp_args: farland_safe_buffer_args,
)

farland_platform_dep = declare_dependency(
  link_with: farland_platform_lib,
  include_directories: farland_inc,
  dependencies: [farland_proto_dep, farland_channels_dep],
)
