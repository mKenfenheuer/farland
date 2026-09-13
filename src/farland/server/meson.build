# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# Server session engine (docs/PLAN.md §3.1): the sans-IO connection state
# machine, frame encoding and the synthetic test backend.
farland_server_lib = static_library(
  'farland-server',
  'connection.cpp',
  'frame_encoder.cpp',
  'test_pattern.cpp',
  include_directories: farland_inc,
  dependencies: [farland_proto_dep, farland_codec_dep],
  cpp_args: farland_safe_buffer_args,
)

farland_server_dep = declare_dependency(
  link_with: farland_server_lib,
  include_directories: farland_inc,
  dependencies: [farland_proto_dep, farland_codec_dep],
)
