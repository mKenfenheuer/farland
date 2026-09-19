# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# Server session engine (docs/PLAN.md §3.1): the sans-IO connection state
# machine, frame encoding and the synthetic test backend.
farland_server_lib = static_library(
  'farland-server',
  'audio_input.cpp',
  'audio_playback.cpp',
  'autodetect.cpp',
  'broker.cpp',
  'camera.cpp',
  'clipboard_bridge.cpp',
  'clipboard_files.cpp',
  'compositor.cpp',
  'connection.cpp',
  'cursor_encoder.cpp',
  'display_control.cpp',
  'display_layout.cpp',
  'dynamic_channels.cpp',
  'frame_encoder.cpp',
  'frame_scheduler.cpp',
  'graphics_pipeline.cpp',
  'loopback_clipboard.cpp',
  'preauth.cpp',
  'privsep.cpp',
  'quality_controller.cpp',
  'scroll_detector.cpp',
  'test_pattern.cpp',
  'touch_input.cpp',
  include_directories: farland_inc,
  dependencies: [farland_proto_dep, farland_codec_dep, farland_channels_dep, farland_video_dep, farland_audio_dep],
  cpp_args: farland_safe_buffer_args,
)

farland_server_dep = declare_dependency(
  link_with: farland_server_lib,
  include_directories: farland_inc,
  dependencies: [farland_proto_dep, farland_codec_dep, farland_channels_dep, farland_video_dep, farland_audio_dep],
)
