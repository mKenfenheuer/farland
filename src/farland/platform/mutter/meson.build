# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# The Mutter backend (docs/PLAN.md §3.3, ROADMAP M7): headless GNOME through
# Mutter's own RemoteDesktop and ScreenCast D-Bus API, on top of the portal
# backend's sd-bus wrapper, PipeWire capture and libei input. Linux only, and
# only where the portal backend is built. libxkbcommon is optional: it turns
# the client's keyboard layout into the keymap Mutter's SetKeymap takes.
farland_platform_mutter_dep = dependency('', required: false)
if farland_platform_portal_dep.found()
  mutter_args = []
  mutter_deps = [farland_platform_portal_dep]
  xkbcommon_dep = dependency('xkbcommon', required: false, include_type: 'system')
  if xkbcommon_dep.found()
    mutter_args += '-DFARLAND_HAVE_XKBCOMMON=1'
    mutter_deps += xkbcommon_dep
  endif

  farland_platform_mutter_lib = static_library(
    'farland-platform-mutter',
    'headless_shell.cpp',
    'mutter_clipboard.cpp',
    'mutter_session.cpp',
    'xkb_keymap.cpp',
    include_directories: farland_inc,
    cpp_args: mutter_args,
    dependencies: mutter_deps,
  )

  farland_platform_mutter_dep = declare_dependency(
    link_with: farland_platform_mutter_lib,
    include_directories: farland_inc,
    compile_args: mutter_args,
    dependencies: farland_platform_portal_dep,
  )
endif

summary({'Mutter (headless GNOME)': farland_platform_mutter_dep.found()}, section: 'Platform backends',
  bool_yn: true)
