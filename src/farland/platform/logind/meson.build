# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# The seat side of a local session (docs/PLAN.md §3.3): logind's view of who
# has the screen at the machine, and the display manager's greeter. Shared by
# the compositor backends — taking a local session over works the same way on
# GNOME and on Plasma. Built on the portal backend's sd-bus wrapper, so it
# needs only that. Linux only.
farland_platform_logind_dep = dependency('', required: false)
if farland_platform_portal_dep.found()
  farland_platform_logind_lib = static_library(
    'farland-platform-logind',
    'seat.cpp',
    include_directories: farland_inc,
    dependencies: farland_platform_portal_dep,
  )

  farland_platform_logind_dep = declare_dependency(
    link_with: farland_platform_logind_lib,
    include_directories: farland_inc,
    dependencies: farland_platform_portal_dep,
  )
endif
