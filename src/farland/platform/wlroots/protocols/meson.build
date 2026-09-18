# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# Wayland protocol glue from wayland-scanner. The staging and stable
# protocols come from the installed wayland-protocols where it has them; the
# copies here (MIT, see REUSE.toml) stand in for older releases and carry the
# wlroots protocols no package installs.
#
# The generated code goes into headers (the private code as *-protocol-code.h,
# compiled once by protocols.c) in a system include directory, so neither the
# compiler warnings nor clang-tidy look at it.
fs = import('fs')
wayland_scanner = find_program(wayland_scanner_dep.get_variable('wayland_scanner'), native: true)
wayland_protocols_dir = ''
if wayland_protocols_dep.found()
  wayland_protocols_dir = wayland_protocols_dep.get_variable('pkgdatadir')
endif

wayland_protocol_xmls = []
foreach protocol : [
  # ext-image-capture-source refers to its toplevel handles.
  'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
  'staging/ext-image-capture-source/ext-image-capture-source-v1.xml',
  'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml',
  'staging/ext-data-control/ext-data-control-v1.xml',
  'stable/linux-dmabuf/linux-dmabuf-v1.xml',
  'wlr-screencopy-unstable-v1.xml',
  'wlr-virtual-pointer-unstable-v1.xml',
  'wlr-data-control-unstable-v1.xml',
  'wlr-output-management-unstable-v1.xml',
  'virtual-keyboard-unstable-v1.xml',
]
  installed = wayland_protocols_dir / protocol
  if wayland_protocols_dir != '' and fs.is_file(installed)
    wayland_protocol_xmls += installed
  elif fs.is_file(fs.name(protocol))
    wayland_protocol_xmls += files(fs.name(protocol))
  endif
endforeach

wayland_protocol_headers = []
foreach xml : wayland_protocol_xmls
  name = fs.stem(xml)
  wayland_protocol_headers += custom_target(
    name + '-client-header',
    input: xml,
    output: name + '-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  wayland_protocol_headers += custom_target(
    name + '-code',
    input: xml,
    output: name + '-protocol-code.h',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach

# linux-dmabuf (for dmabuf capture) is optional: without it the capture
# uses shared memory only.
wayland_have_linux_dmabuf = false
foreach xml : wayland_protocol_xmls
  if fs.stem(xml) == 'linux-dmabuf-v1'
    wayland_have_linux_dmabuf = true
  endif
endforeach

wayland_protocols_lib = static_library(
  'farland-wayland-protocols',
  'protocols.c',
  wayland_protocol_headers,
  c_args: wayland_have_linux_dmabuf ? ['-DFARLAND_HAVE_LINUX_DMABUF=1'] : [],
  include_directories: include_directories('.', is_system: true),
  dependencies: wayland_client_dep,
)
farland_wayland_protocols_dep = declare_dependency(
  link_with: wayland_protocols_lib,
  # Generated headers first, so that every user waits for them.
  sources: wayland_protocol_headers,
  include_directories: include_directories('.', is_system: true),
  compile_args: wayland_have_linux_dmabuf ? ['-DFARLAND_HAVE_LINUX_DMABUF=1'] : [],
  dependencies: wayland_client_dep,
)
