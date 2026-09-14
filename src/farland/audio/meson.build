# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# Audio formats and encoders (docs/PLAN.md §3.1). Opus comes from libopus,
# loaded at runtime, so nothing is needed to build. Glue around a C library
# that takes raw pointers, so not built with the safe-buffer warnings.
audio_dl_dep = dependency('dl', required: false)

farland_audio_lib = static_library(
  'farland-audio',
  'opus.cpp',
  include_directories: farland_inc,
  dependencies: [farland_base_dep, audio_dl_dep],
)

farland_audio_dep = declare_dependency(
  link_with: farland_audio_lib,
  include_directories: farland_inc,
  dependencies: [farland_base_dep, audio_dl_dep],
)

summary({'Opus (runtime-loaded)': true}, section: 'Audio', bool_yn: true)
