# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# Linux only, like the library. The protocol tests run the backend against a
# fake KWin built on libwayland-server (fake_kwin.cpp) and skip without it.
if farland_platform_kwin_dep.found()
  unit_test_sources += files('test_kwin_launcher.cpp')
  unit_test_deps += farland_platform_kwin_dep

  wayland_server_dep = dependency('wayland-server', required: false, include_type: 'system')
  if wayland_server_dep.found()
    foreach name, xml : kwin_protocol_xmls
      unit_test_sources += custom_target(
        name + '-server-header',
        input: xml,
        output: name + '-server-protocol.h',
        command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
      )
    endforeach
    unit_test_sources += files('fake_kwin.cpp', 'test_kwin_wayland.cpp')
    unit_test_deps += declare_dependency(
      dependencies: wayland_server_dep,
      include_directories: include_directories('.', is_system: true),
    )
  endif

  # Manual check against a real KWin (virtual outputs, resizing, and whether
  # KWin keeps drawing them off the seat); not run as a test.
  executable(
    'farland-plasma-headless-probe',
    'plasma_headless_probe.cpp',
    dependencies: farland_app_dep,
  )
endif
