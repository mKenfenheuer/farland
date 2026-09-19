# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# Process glue under apps/: local sockets and farlandd's configuration.
unit_test_sources += files(
  'test_agent.cpp',
  'test_consent.cpp',
  'test_daemon_config.cpp',
  'test_launcher.cpp',
  'test_session_view.cpp',
  'test_registry.cpp',
  'test_seat_takeover.cpp',
  'test_unix_socket.cpp',
)
unit_test_deps += [farland_app_dep, farland_daemon_dep, farland_agent_dep, dependency('threads')]
