# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# Sans-IO PDU codecs for both roles (docs/PLAN.md §3.1).
farland_proto_lib = static_library(
  'farland-proto',
  'autodetect.cpp',
  'bitmap.cpp',
  'capabilities.cpp',
  'client_info.cpp',
  'fastpath.cpp',
  'framing.cpp',
  'gcc.cpp',
  'input.cpp',
  'license.cpp',
  'mcs.cpp',
  'pointer.cpp',
  'security.cpp',
  'share.cpp',
  'x224.cpp',
  include_directories: farland_inc,
  dependencies: farland_base_dep,
  cpp_args: farland_safe_buffer_args,
)

farland_proto_dep = declare_dependency(
  link_with: farland_proto_lib,
  include_directories: farland_inc,
  dependencies: farland_base_dep,
)
