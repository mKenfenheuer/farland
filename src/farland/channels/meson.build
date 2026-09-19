# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# Virtual channels (docs/PLAN.md §3.1), sans-IO: static channel framing, the
# drdynvc multiplexer, the Graphics Pipeline (RDPGFX), Display Control, touch and
# pen input (RDPEI), the camera (RDPECAM) and the clipboard (cliprdr).
farland_channels_lib = static_library(
  'farland-channels',
  'audin.cpp',
  'clipboard_formats.cpp',
  'cliprdr.cpp',
  'cliprdr_server.cpp',
  'disp.cpp',
  'drdynvc.cpp',
  'dvc_server.cpp',
  'rdpecam.cpp',
  'rdpei.cpp',
  'rdpei_server.cpp',
  'rdpgfx.cpp',
  'rdpgfx_server.cpp',
  'rdpsnd.cpp',
  'rdpsnd_server.cpp',
  'svc.cpp',
  include_directories: farland_inc,
  dependencies: farland_base_dep,
  cpp_args: farland_safe_buffer_args,
)

farland_channels_dep = declare_dependency(
  link_with: farland_channels_lib,
  include_directories: farland_inc,
  dependencies: farland_base_dep,
)
