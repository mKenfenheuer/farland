# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# Benchmarks are not tests and are not built by default:
#   meson compile -C build bench-progressive
executable(
  'bench-progressive',
  'bench_progressive.cpp',
  dependencies: farland_codec_dep,
  build_by_default: false,
)
