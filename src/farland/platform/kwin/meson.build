# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# The KWin backend (docs/PLAN.md §3.3, ROADMAP M7): headless Plasma sessions.
# A Wayland client of KWin: zkde_screencast_unstable_v1 turns outputs into
# PipeWire streams, kde_output_management_v2 sizes them, ext-data-control-v1
# shares the clipboard; input goes through KWin's EIS. PipeWire capture and
# libei come from the portal backend. Linux only.
#
# The protocol XMLs are vendored (protocols/, each under its own licence),
# so neither plasma-wayland-protocols nor a recent wayland-protocols is
# needed to build.
kwin_opt = get_option('kwin').require(
  host_machine.system() == 'linux',
  error_message: 'the KWin backend is Linux-only',
).require(
  farland_platform_portal_dep.found(),
  error_message: 'the KWin backend needs the portal backend (sd-bus, PipeWire, libei)',
)

wayland_client_dep = dependency('wayland-client', version: '>= 1.20', required: kwin_opt, include_type: 'system')
wayland_scanner = disabler()
if wayland_client_dep.found()
  wayland_scanner_dep = dependency('wayland-scanner', native: true, required: false)
  if wayland_scanner_dep.found()
    wayland_scanner = find_program(wayland_scanner_dep.get_variable(pkgconfig: 'wayland_scanner'), native: true,
      required: kwin_opt)
  else
    wayland_scanner = find_program('wayland-scanner', native: true, required: kwin_opt)
  endif
endif

farland_platform_kwin_dep = dependency('', required: false)
# The wlroots backend generates ext-data-control-v1 as well (src/meson.build
# configures it first). Where that library is built, KWin links it instead of
# generating the protocol a second time: the interface tables may exist only
# once in a binary.
shared_wayland_protocols_dep = get_variable('farland_wayland_protocols_dep', dependency('', required: false))
kwin_protocol_args = []
if shared_wayland_protocols_dep.found()
  kwin_protocol_args += '-DFARLAND_KWIN_SHARED_DATA_CONTROL=1'
endif
kwin_protocol_xmls = {}
if wayland_client_dep.found() and wayland_scanner.found()
  kwin_protocol_sources = []
  foreach name : ['ext-data-control-v1', 'kde-output-device-v2', 'kde-output-management-v2',
                  'zkde-screencast-unstable-v1']
    xml = files('protocols' / name + '.xml')
    # Every XML stays here for the tests, which generate server headers from
    # them; only the client side of a shared protocol is left out.
    kwin_protocol_xmls += {name: xml}
    if name != 'ext-data-control-v1' or not shared_wayland_protocols_dep.found()
      kwin_protocol_sources += custom_target(
        name + '-client-header',
        input: xml,
        output: name + '-client-protocol.h',
        command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
      )
      # The interface tables, as a header: protocols.cpp includes them, so
      # the project needs no C compiler.
      kwin_protocol_sources += custom_target(
        name + '-code',
        input: xml,
        output: name + '-protocol-code.h',
        command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
      )
    endif
  endforeach
  # System headers: the generated code is not ours to warn about (compiler
  # warnings and clang-tidy both skip it).
  kwin_protocol_inc = include_directories('.', is_system: true)
  kwin_protocol_dep = declare_dependency(
    sources: kwin_protocol_sources,
    include_directories: kwin_protocol_inc,
    compile_args: kwin_protocol_args,
    dependencies: [wayland_client_dep, shared_wayland_protocols_dep],
  )

  farland_platform_kwin_lib = static_library(
    'farland-platform-kwin',
    'data_control_clipboard.cpp',
    'kwin_eis.cpp',
    'kwin_launcher.cpp',
    'output_management.cpp',
    'protocols.cpp',
    'screencast.cpp',
    'wayland_connection.cpp',
    include_directories: farland_inc,
    dependencies: [farland_platform_portal_dep, kwin_protocol_dep],
  )

  farland_platform_kwin_dep = declare_dependency(
    link_with: farland_platform_kwin_lib,
    include_directories: farland_inc,
    dependencies: [farland_platform_portal_dep, kwin_protocol_dep],
  )
endif

summary({'KWin (headless Plasma)': farland_platform_kwin_dep.found()}, section: 'Platform backends', bool_yn: true)
