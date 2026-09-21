# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# Linux only, like the library. The session tests run mock_portal.py on a
# private dbus-daemon and skip without dbus-daemon, python3-dbus or PyGObject;
# the EiInput tests run against an in-process libeis server and skip without it;
# the capture tests start a private pipewire daemon and skip without one.
if farland_platform_portal_dep.found()
  unit_test_sources += files(
    'portal_fixture.cpp',
    'test_ei_input.cpp',
    'test_pipewire_capture.cpp',
    'test_pixel_formats.cpp',
    'test_portal_clipboard.cpp',
    'test_portal_input.cpp',
    'test_portal_session.cpp',
    'test_restore_token.cpp',
  )
  unit_test_deps += farland_platform_portal_dep

  libeis_dep = dependency('libeis-1.0', required: false, include_type: 'system')
  if libeis_dep.found()
    unit_test_deps += declare_dependency(compile_args: '-DFARLAND_TEST_HAVE_LIBEIS=1', dependencies: libeis_dep)
  endif
  # The same check ei_input.cpp makes, so the touch tests can tell whether
  # they are running against a libei that carries touches at all. Ubuntu
  # 24.04, which CI builds on, ships libei 1.2.
  if meson.get_compiler('cpp').has_header_symbol('libei.h', 'ei_touch_cancel', dependencies: libei_dep)
    unit_test_deps += declare_dependency(compile_args: '-DFARLAND_TEST_HAVE_EI_TOUCH=1')
  endif

  # Manual check against a real desktop portal; not run as a test.
  executable(
    'farland-portal-probe',
    'portal_probe.cpp',
    dependencies: farland_platform_portal_dep,
  )
  # Touches the shared monitor through EIS; also manual.
  executable(
    'farland-touch-probe',
    'touch_probe.cpp',
    dependencies: farland_platform_portal_dep,
  )
endif
