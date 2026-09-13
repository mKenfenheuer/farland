# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# Every parser of network input gets a target here (CONTRIBUTING.md). Seeds
# live in corpus/<name>/.
fuzz_targets = ['ber', 'per']

foreach name : fuzz_targets
  if get_option('fuzzing')
    executable(
      'fuzz-' + name,
      'fuzz_' + name + '.cpp',
      dependencies: farland_base_dep,
      link_args: '-fsanitize=fuzzer',
    )
  else
    # Without libFuzzer, replay the seed corpus so every build exercises the
    # targets (and sanitizer builds check them).
    replay = executable(
      'fuzz-' + name,
      'fuzz_' + name + '.cpp',
      'standalone_main.cpp',
      dependencies: farland_base_dep,
    )
    test('corpus-' + name, replay, args: [meson.current_source_dir() / 'corpus' / name], suite: 'fuzz-corpus')
  endif
endforeach
