# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

unit_test_sources += files('test_farlandd.cpp', 'test_session.cpp')
# The scripted multi-session client needs TLS and NLA, which the proto-only
# support library does not link.
unit_test_sources += files('../support/rdp_test_client.cpp')
unit_test_deps += [farland_app_dep, dependency('threads')]
