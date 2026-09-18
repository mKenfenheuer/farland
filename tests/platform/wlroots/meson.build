# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# Linux only, like the library. The keymap tests need xkeyboard-config
# (xkb-data) and skip without it. The compositor tests start headless sway,
# labwc and cage with the pixman renderer and skip the ones not installed
# (sway also refuses to run as root).
if farland_platform_wlroots_dep.found()
  unit_test_sources += files(
    'test_wlroots_compositor.cpp',
    'test_wlroots_parts.cpp',
  )
  unit_test_deps += farland_platform_wlroots_dep
endif
