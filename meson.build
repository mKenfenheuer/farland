# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

project(
  'farland',
  'cpp',
  version: '0.0.1',
  license: 'Apache-2.0',
  license_files: ['LICENSE'],
  meson_version: '>= 1.3.0',
  default_options: [
    'cpp_std=c++23',
    'warning_level=3',
    'buildtype=debugoptimized',
    'b_ndebug=if-release',
    'b_pie=true',
  ],
)

cxx = meson.get_compiler('cpp')

# Platform baseline, docs/PLAN.md §2: GCC >= 13, Clang >= 19 (Clang 18 cannot use
# libstdc++'s std::expected; Ubuntu 24.04 ships clang-19). Apple clang (local
# development on macOS only) uses its own version numbers and is not gated.
if cxx.get_id() == 'gcc' and cxx.version().version_compare('< 13')
  error('farland needs GCC >= 13, found @0@'.format(cxx.version()))
endif
if cxx.get_id() == 'clang' and host_machine.system() != 'darwin' and cxx.version().version_compare('< 19')
  error('farland needs Clang >= 19, found @0@ (on Ubuntu 24.04: apt install clang-19, CXX=clang++-19)'.format(cxx.version()))
endif

add_project_arguments(
  cxx.get_supported_arguments(
    '-Wconversion',
    '-Wsign-conversion',
    '-Wshadow',
    '-Wnon-virtual-dtor',
    '-Wold-style-cast',
    '-Wcast-align',
    '-Wcast-qual',
    '-Wdouble-promotion',
    '-Wformat=2',
    '-Wimplicit-fallthrough',
    '-Wmissing-declarations',
    '-Woverloaded-virtual',
    '-Wduplicated-cond',
    '-Wlogical-op',
    '-Wextra-semi',
    # FARLAND_TRY names its temporaries with __COUNTER__, which every supported
    # compiler has; Clang 21 and later call it a C2y extension under -Wpedantic.
    '-Wno-c2y-extensions',
  ),
  language: 'cpp',
)

# GCC 13 and 14 report bogus buffer overflows, frees and uninitialized reads
# inside inlined std::vector code at -O2/-O3 (GCC bugs 105705, 106199, 107852),
# and GCC 15 still the bogus frees; GCC 16 and Clang do not. Only there are
# these warnings off.
if cxx.get_id() == 'gcc' and cxx.version().version_compare('< 15')
  add_project_arguments('-Wno-stringop-overflow', '-Wno-maybe-uninitialized', language: 'cpp')
endif
if cxx.get_id() == 'gcc' and cxx.version().version_compare('< 16')
  add_project_arguments('-Wno-free-nonheap-object', language: 'cpp')
endif

# Hardening, docs/PLAN.md §6. Sanitizer and fuzzing builds turn this off.
# Meson itself enables standard library assertions only while b_ndebug is off
# (libc++ in "fast" mode); farland keeps them in release builds too, and uses
# libc++'s stricter "extensive" mode.
if get_option('hardening')
  add_project_arguments(
    cxx.get_supported_arguments(
      '-D_GLIBCXX_ASSERTIONS',
      '-U_LIBCPP_HARDENING_MODE',
      '-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE',
      '-fstack-protector-strong',
      '-fstack-clash-protection',
      '-ftrivial-auto-var-init=zero',
      '-fcf-protection=full',
    ),
    language: 'cpp',
  )
  if get_option('optimization') != '0'
    add_project_arguments('-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=3', language: 'cpp')
  endif
  add_project_link_arguments(
    cxx.get_supported_link_arguments('-Wl,-z,relro', '-Wl,-z,now', '-Wl,-z,noexecstack'),
    language: 'cpp',
  )
endif

# Code that parses network input must not do raw pointer arithmetic
# (Clang's C++ Safe Buffers). Libraries opt in through this variable.
farland_safe_buffer_args = []
if cxx.get_id() == 'clang'
  farland_safe_buffer_args = cxx.get_supported_arguments('-Werror=unsafe-buffer-usage')
endif

if get_option('fuzzing')
  if cxx.get_id() != 'clang'
    error('-Dfuzzing=true needs clang (libFuzzer)')
  endif
  # Coverage feedback for all farland code, not only the fuzz entry points.
  add_project_arguments('-fsanitize=fuzzer-no-link', language: 'cpp')
endif

farland_inc = include_directories('src')

# TLS and the cryptographic primitives (docs/PLAN.md §2).
openssl_dep = dependency('openssl', version: '>= 3.0')

subdir('src')
subdir('apps')

catch2_dep = dependency(
  'catch2-with-main',
  version: '>= 3.4',
  required: get_option('tests').disable_if(get_option('fuzzing')),
  allow_fallback: true,
  # The wrap's own test suite is not ours to build (it breaks with Clang 19 and
  # libstdc++ 14).
  default_options: ['tests=false'],
  include_type: 'system',
)
if catch2_dep.found()
  subdir('tests')
endif

subdir('fuzz')
subdir('benchmarks')

summary(
  {
    'tests': catch2_dep.found(),
    'fuzzing (libFuzzer)': get_option('fuzzing'),
    'hardening': get_option('hardening'),
    'safe buffers enforced': farland_safe_buffer_args.length() > 0,
  },
  section: 'Configuration',
  bool_yn: true,
)
