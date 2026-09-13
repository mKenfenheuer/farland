# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# The session loop lives in a library so the end-to-end test can drive it.
farland_app_lib = static_library(
  'farland-app',
  'farland-server/session.cpp',
  dependencies: [farland_server_dep, farland_auth_dep],
)

farland_app_dep = declare_dependency(
  link_with: farland_app_lib,
  include_directories: include_directories('farland-server'),
  dependencies: [farland_server_dep, farland_auth_dep],
)

executable(
  'farland-server',
  'farland-server/main.cpp',
  dependencies: [farland_app_dep, dependency('threads')],
  install: true,
)
