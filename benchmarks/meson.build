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

# The M5 exit benchmark: bitrate, CPU, latency and quality per codec over a
# generated content corpus, plus the "text at 2 Mbit/s" check.
#   meson compile -C build bench-codecs
executable(
  'bench-codecs',
  'bench_codecs.cpp',
  dependencies: [farland_codec_dep, farland_video_dep],
  build_by_default: false,
)
