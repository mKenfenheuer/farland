# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# Every parser of network input gets a target here (CONTRIBUTING.md): the
# source is fuzz_<name>.cpp and the seeds live in corpus/<name>/.
fuzz_targets = {
  'ber': farland_base_dep,
  'per': farland_base_dep,
  'mcs_gcc': farland_proto_dep,
  'input': farland_proto_dep,
  'planar': farland_codec_dep,
  'server': farland_server_dep,
  'share': farland_proto_dep,
  'x224': farland_proto_dep,
}

foreach name, dep : fuzz_targets
  if get_option('fuzzing')
    executable(
      'fuzz-' + name,
      'fuzz_' + name + '.cpp',
      dependencies: dep,
      link_args: '-fsanitize=fuzzer',
    )
  else
    # Without libFuzzer, replay the seed corpus so every build exercises the
    # targets (and sanitizer builds check them).
    replay = executable(
      'fuzz-' + name,
      'fuzz_' + name + '.cpp',
      'standalone_main.cpp',
      dependencies: dep,
    )
    test('corpus-' + name, replay, args: [meson.current_source_dir() / 'corpus' / name], suite: 'fuzz-corpus')
  endif
endforeach
