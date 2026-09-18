# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# Linux only, like the library. The tests run mock_mutter.py on a private
# dbus-daemon and skip without dbus-daemon, python3-dbus or PyGObject.
if farland_platform_mutter_dep.found()
  unit_test_sources += files(
    'mutter_fixture.cpp',
    'test_mutter_clipboard.cpp',
    'test_mutter_session.cpp',
  )
  unit_test_deps += farland_platform_mutter_dep
endif

# Manual check against a real GNOME Shell (resizing, adding and removing
# virtual monitors); not run as a test.
if farland_platform_mutter_dep.found()
  executable(
    'farland-gnome-headless-probe',
    'gnome_headless_probe.cpp',
    dependencies: farland_app_dep,
  )
endif
