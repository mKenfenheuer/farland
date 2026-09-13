# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# The session loop lives in a library so the end-to-end test can drive it.
farland_app_lib = static_library(
  'farland-app',
  'farland-server/nla.cpp',
  'farland-server/privsep_process.cpp',
  'farland-server/sandbox.cpp',
  'farland-server/session.cpp',
  'farland-server/transport.cpp',
  dependencies: [farland_server_dep, farland_auth_dep],
)

farland_app_dep = declare_dependency(
  link_with: farland_app_lib,
  include_directories: include_directories('farland-server'),
  dependencies: [farland_server_dep, farland_auth_dep],
)

farland_server_exe = executable(
  'farland-server',
  'farland-server/main.cpp',
  dependencies: [farland_app_dep, dependency('threads')],
  install: true,
)

executable(
  'farlandctl',
  'farlandctl/main.cpp',
  dependencies: [farland_auth_dep],
  install: true,
)
