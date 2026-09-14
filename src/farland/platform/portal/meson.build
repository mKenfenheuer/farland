# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# The portal backend (docs/PLAN.md §3.3): xdg-desktop-portal RemoteDesktop and
# ScreenCast over sd-bus, PipeWire capture and libei input. Linux only.
#
# Glue around C libraries that hand out raw pointers, so it is not built with
# the safe-buffer warnings.
portal_opt = get_option('portal').require(
  host_machine.system() == 'linux',
  error_message: 'the portal backend is Linux-only',
)

# sd-bus from libsystemd, or from basu on systems without systemd.
sd_bus_dep = dependency('libsystemd', 'basu', required: portal_opt, include_type: 'system')
# ei_region_get_mapping_id() arrived in libei 1.1.
# System headers: libei 1.5's own header trips -Wshadow.
libei_dep = dependency('libei-1.0', version: '>= 1.1', required: portal_opt, include_type: 'system')
pipewire_dep = dependency('libpipewire-0.3', version: '>= 1.0', required: portal_opt, include_type: 'system')

farland_platform_portal_dep = dependency('', required: false)
if sd_bus_dep.found() and libei_dep.found() and pipewire_dep.found()
  portal_compile_args = sd_bus_dep.name() == 'basu' ? ['-DFARLAND_SD_BUS_BASU'] : []
  portal_deps = [farland_codec_dep, sd_bus_dep, libei_dep, pipewire_dep]
  portal_private_args = []
  portal_private_deps = []
  # Tiled dmabufs: GBM imports and maps them, EGL says which modifiers work.
  # Without both, the capture offers LINEAR dmabufs and shared memory only.
  gbm_dep = dependency('gbm', required: false, include_type: 'system')
  egl_dep = dependency('egl', required: false, include_type: 'system')
  if gbm_dep.found() and egl_dep.found()
    portal_private_args += '-DFARLAND_HAVE_GBM=1'
    portal_private_deps += [gbm_dep, egl_dep]
  endif

  farland_platform_portal_lib = static_library(
    'farland-platform-portal',
    'dmabuf_reader.cpp',
    'ei_input.cpp',
    'pipewire_capture.cpp',
    'pixel_formats.cpp',
    'portal_bus.cpp',
    'portal_input.cpp',
    'portal_session.cpp',
    'restore_token.cpp',
    include_directories: farland_inc,
    cpp_args: portal_compile_args + portal_private_args,
    dependencies: portal_deps + portal_private_deps,
  )

  farland_platform_portal_dep = declare_dependency(
    link_with: farland_platform_portal_lib,
    include_directories: farland_inc,
    compile_args: portal_compile_args,
    dependencies: portal_deps,
  )
endif

summary({'portal': farland_platform_portal_dep.found()}, section: 'Platform backends', bool_yn: true)
