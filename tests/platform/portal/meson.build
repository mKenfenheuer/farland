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
    'test_portal_input.cpp',
    'test_portal_session.cpp',
    'test_restore_token.cpp',
  )
  unit_test_deps += farland_platform_portal_dep

  libeis_dep = dependency('libeis-1.0', required: false, include_type: 'system')
  if libeis_dep.found()
    unit_test_deps += declare_dependency(compile_args: '-DFARLAND_TEST_HAVE_LIBEIS=1', dependencies: libeis_dep)
  endif

  # Manual check against a real desktop portal; not run as a test.
  executable(
    'farland-portal-probe',
    'portal_probe.cpp',
    dependencies: farland_platform_portal_dep,
  )
endif
