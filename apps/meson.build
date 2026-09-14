# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# The session loop lives in a library so the end-to-end test can drive it.
app_sources = files(
  'farland-server/nla.cpp',
  'farland-server/privsep_process.cpp',
  'farland-server/sandbox.cpp',
  'farland-server/session.cpp',
  'farland-server/transport.cpp',
)
app_deps = [farland_server_dep, farland_auth_dep, farland_platform_dep]
app_args = []
# Sharing the running desktop (--share) needs the portal backend.
if farland_platform_portal_dep.found()
  app_sources += files('farland-server/portal_desktop.cpp')
  app_deps += farland_platform_portal_dep
  app_args += '-DFARLAND_HAVE_PORTAL=1'
endif

farland_app_lib = static_library(
  'farland-app',
  app_sources,
  dependencies: app_deps,
  cpp_args: app_args,
)

farland_app_dep = declare_dependency(
  link_with: farland_app_lib,
  include_directories: include_directories('farland-server'),
  compile_args: app_args,
  dependencies: app_deps,
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
