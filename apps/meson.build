# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# The session loop lives in a library so the end-to-end test can drive it.
app_sources = files(
  'farland-server/nla.cpp',
  'farland-server/privsep_process.cpp',
  'farland-server/sandbox.cpp',
  'farland-server/session.cpp',
  'farland-server/session_audio.cpp',
  'farland-server/transport.cpp',
  'farland-server/unix_socket.cpp',
)
app_deps = [farland_server_dep, farland_auth_dep, farland_platform_dep]
app_args = []
# Session audio through the user's PipeWire.
if farland_pipewire_audio_dep.found()
  app_deps += farland_pipewire_audio_dep
  app_args += '-DFARLAND_HAVE_PIPEWIRE_AUDIO=1'
endif
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

# farlandd, the multi-session daemon (docs/ROADMAP.md M7). So far only its
# configuration; the executable follows in S1. toml++ is header-only; the
# wrap stands in where the distribution has no package.
tomlplusplus_dep = dependency(
  'tomlplusplus',
  version: '>= 3.3',
  allow_fallback: true,
  include_type: 'system',
  # The wrap's deprecated boolean compile_library has no default and so is
  # true, which builds a shared library (with our -Werror, which its code
  # does not survive under Clang 17 and later). Header-only is all we need.
  default_options: ['build_lib=false', 'compile_library=false', 'werror=false'],
)
farland_daemon_lib = static_library(
  'farland-daemon',
  'farlandd/config.cpp',
  dependencies: tomlplusplus_dep,
)
farland_daemon_dep = declare_dependency(
  link_with: farland_daemon_lib,
  include_directories: include_directories('farlandd'),
  dependencies: tomlplusplus_dep,
)

executable(
  'farlandctl',
  'farlandctl/main.cpp',
  dependencies: [farland_auth_dep],
  install: true,
)
