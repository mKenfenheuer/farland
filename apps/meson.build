# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# The session loop lives in a library so the end-to-end test can drive it.
app_sources = files(
  'farland-server/nla.cpp',
  'farland-server/privsep_process.cpp',
  'farland-server/sandbox.cpp',
  'farland-server/session.cpp',
  'farland-server/session_audio.cpp',
  'farland-server/session_camera.cpp',
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
# Headless desktops for multi-session (headless.hpp): the dispatcher, the
# test pattern desktop, and one backend per compositor family. A backend is
# built when its file is there, its option allows it and its dependencies
# are found; the missing ones get stub factories (headless_unavailable.cpp)
# that fail with Errc::unsupported.
fs = import('fs')
app_sources += files('farland-server/headless.cpp', 'farland-server/test_desktop.cpp')
# Each backend's platform library (src/farland/platform/<name>/) defines its
# dependency object; where it does not exist, the backend is not built.
not_found_dep = dependency('', required: false)
headless_backends = [
  # name, define, dependencies it needs
  ['gnome', 'FARLAND_HAVE_GNOME_HEADLESS', [get_variable('farland_platform_mutter_dep', not_found_dep)]],
  ['plasma', 'FARLAND_HAVE_PLASMA_HEADLESS', [get_variable('farland_platform_kwin_dep', not_found_dep)]],
  ['wlroots', 'FARLAND_HAVE_WLROOTS_HEADLESS', [get_variable('farland_platform_wlroots_dep', not_found_dep)]],
]
headless_built = {}
headless_missing = false
foreach backend : headless_backends
  name = backend[0]
  source = 'farland-server' / name + '_headless.cpp'
  deps_found = true
  foreach dep : backend[2]
    deps_found = deps_found and dep.found()
  endforeach
  opt = get_option('headless-' + name)
  if fs.is_file(source) and opt.allowed() and deps_found
    app_sources += files(source)
    app_deps += backend[2]
    app_args += '-D' + backend[1] + '=1'
    headless_built += {name: true}
  else
    if opt.enabled()
      error('-Dheadless-@0@=enabled, but @1@ or its dependencies are missing'.format(name, source))
    endif
    headless_built += {name: false}
    headless_missing = true
  endif
endforeach
if headless_missing
  app_sources += files('farland-server/headless_unavailable.cpp')
endif
summary(headless_built, section: 'Headless desktops', bool_yn: true)
if farland_platform_kwin_dep.found()
  app_sources += files('farland-server/plasma_headless.cpp')
  app_deps += farland_platform_kwin_dep
  app_args += '-DFARLAND_HAVE_KWIN=1'
endif
if farland_platform_kwin_dep.found()
  # Lets KWin grant screen casting to the installed programs that attach to a
  # running Plasma session. One entry each: KWin matches the executable path.
  foreach program : ['farland-server', 'farland-agent']
    configure_file(
      input: meson.project_source_root() / 'data' / 'org.farland.server.desktop.in',
      output: 'org.farland.@0@.desktop'.format(program.split('-')[1]),
      configuration: {
        'BINDIR': get_option('prefix') / get_option('bindir'),
        'PROGRAM': program,
      },
      install: true,
      install_dir: get_option('datadir') / 'applications',
    )
  endforeach
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

# sd-bus for logind, GDM, the users' service managers and the takeover
# prompt (org.freedesktop.Notifications); optional and Linux only.
libsystemd_dep = dependency('libsystemd', required: false, include_type: 'system')

# farland-agent, the per-user session process of farlandd (docs/ROADMAP.md
# M7); its logic is a library for the tests.
agent_deps = [farland_app_dep, dependency('threads')]
agent_args = []
if libsystemd_dep.found()
  agent_deps += libsystemd_dep
  agent_args += '-DFARLAND_HAVE_LIBSYSTEMD=1'
endif
farland_agent_lib = static_library(
  'farland-agent',
  'farland-agent/agent.cpp',
  'farland-agent/prompt.cpp',
  dependencies: agent_deps,
  cpp_args: agent_args,
)
farland_agent_dep = declare_dependency(
  link_with: farland_agent_lib,
  include_directories: include_directories('farland-agent'),
  compile_args: agent_args,
  dependencies: agent_deps,
)
farland_agent_exe = executable(
  'farland-agent',
  'farland-agent/main.cpp',
  dependencies: farland_agent_dep,
  install: true,
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
# sd-login and sd-bus (above) for logind, GDM and the users' service
# managers, and libpam for farland's own login sessions: both optional,
# Linux only.
daemon_deps = [tomlplusplus_dep, farland_app_dep, dependency('threads')]
daemon_args = []
if libsystemd_dep.found()
  daemon_deps += libsystemd_dep
  daemon_args += '-DFARLAND_HAVE_LIBSYSTEMD=1'
endif
pam_dep = cxx.find_library(
  'pam',
  required: get_option('pam').require(host_machine.system() == 'linux', error_message: 'PAM sessions are Linux-only'),
  has_headers: ['security/pam_appl.h'],
)
if pam_dep.found()
  daemon_deps += pam_dep
  daemon_args += '-DFARLAND_HAVE_PAM=1'
endif
farland_daemon_lib = static_library(
  'farland-daemon',
  'farlandd/config.cpp',
  'farlandd/consent.cpp',
  'farlandd/daemon.cpp',
  'farlandd/enrol.cpp',
  'farlandd/launcher.cpp',
  'farlandd/logind.cpp',
  'farlandd/registry.cpp',
  'farlandd/session_store.cpp',
  'farlandd/session_view.cpp',
  'farlandd/seat_takeover.cpp',
  dependencies: daemon_deps,
  cpp_args: daemon_args,
)
farland_daemon_dep = declare_dependency(
  link_with: farland_daemon_lib,
  include_directories: include_directories('farlandd'),
  dependencies: daemon_deps,
)
# The PAM module the display manager's account stack asks through, so that
# a client holding a session is asked before somebody at the machine takes
# it back ([policy] seat_takeover). Installed beside the other PAM modules;
# nothing uses it until it is named in a PAM service.
if pam_dep.found() and libsystemd_dep.found()
  shared_module(
    'pam_farland',
    'farland-pam/pam_farland.c',
    name_prefix: '',
    dependencies: [pam_dep, libsystemd_dep],
    install: true,
    install_dir: get_option('libdir') / 'security',
  )
endif

farlandd_exe = executable(
  'farlandd',
  'farlandd/main.cpp',
  dependencies: farland_daemon_dep,
  cpp_args: '-DFARLAND_AGENT_PATH="@0@"'.format(get_option('prefix') / get_option('bindir') / 'farland-agent'),
  install: true,
  install_dir: get_option('sbindir'),
)
summary({'libsystemd (logind, GDM)': libsystemd_dep.found(), 'PAM sessions': pam_dep.found()},
  section: 'farlandd', bool_yn: true)

# farlandctl passwd without a user name enrols the caller through farlandd's
# D-Bus API, where libsystemd's sd-bus is there.
farlandctl_deps = [farland_auth_dep]
farlandctl_args = []
if libsystemd_dep.found()
  farlandctl_deps += libsystemd_dep
  farlandctl_args += '-DFARLAND_HAVE_LIBSYSTEMD=1'
endif
executable(
  'farlandctl',
  'farlandctl/main.cpp',
  dependencies: farlandctl_deps,
  cpp_args: farlandctl_args,
  install: true,
)
