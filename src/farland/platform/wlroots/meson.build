# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# The wlroots backend (docs/PLAN.md §3.3): drives sway, labwc or cage as a
# Wayland client, without a portal. ext-image-copy-capture (or
# wlr-screencopy) for frames and the cursor, the virtual keyboard and pointer
# protocols for input, data-control for the clipboard and
# wlr-output-management for sizes. Linux only.
#
# Two libraries: wayland/ is the generic Wayland client part (connection,
# shared memory, data-control clipboard), the rest is wlroots-specific.
# Glue around C libraries that hand out raw pointers, so it is not built with
# the safe-buffer warnings.
wlroots_opt = get_option('wlroots').require(
  host_machine.system() == 'linux',
  error_message: 'the wlroots backend is Linux-only',
)

# System headers: wayland-client's and xkbcommon's must not trip our warnings.
wayland_client_dep = dependency('wayland-client', version: '>= 1.20', required: wlroots_opt, include_type: 'system')
wayland_scanner_dep = dependency('wayland-scanner', version: '>= 1.20', native: true, required: wlroots_opt)
# xkb_keymap_key_get_mods_for_level() arrived in xkbcommon 1.0.
xkbcommon_dep = dependency('xkbcommon', version: '>= 1.0', required: wlroots_opt, include_type: 'system')
wayland_protocols_dep = dependency('wayland-protocols', required: false)

farland_platform_wayland_dep = dependency('', required: false)
farland_platform_wlroots_dep = dependency('', required: false)
if wayland_client_dep.found() and wayland_scanner_dep.found() and xkbcommon_dep.found() and add_languages(
  'c',
  native: false,
  required: wlroots_opt,
)
  subdir('protocols')

  farland_platform_wayland_lib = static_library(
    'farland-platform-wayland',
    'wayland/connection.cpp',
    'wayland/data_control.cpp',
    'wayland/shm.cpp',
    include_directories: farland_inc,
    dependencies: [farland_platform_dep, farland_wayland_protocols_dep],
  )
  farland_platform_wayland_dep = declare_dependency(
    link_with: farland_platform_wayland_lib,
    include_directories: farland_inc,
    dependencies: [farland_platform_dep, farland_wayland_protocols_dep],
  )

  farland_platform_wlroots_lib = static_library(
    'farland-platform-wlroots',
    'compositor.cpp',
    'output_capture.cpp',
    'outputs.cpp',
    'virtual_input.cpp',
    'xkb_keymap.cpp',
    include_directories: farland_inc,
    dependencies: [farland_platform_wayland_dep, xkbcommon_dep],
  )
  farland_platform_wlroots_dep = declare_dependency(
    link_with: farland_platform_wlroots_lib,
    include_directories: farland_inc,
    dependencies: [farland_platform_wayland_dep, xkbcommon_dep],
  )
endif

summary({'wlroots': farland_platform_wlroots_dep.found()}, section: 'Platform backends', bool_yn: true)
