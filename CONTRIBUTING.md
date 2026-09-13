# Contributing to farland

## Build and test

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

Useful configurations:

| Purpose | Setup |
|---|---|
| Sanitizers | `CC=clang CXX=clang++ meson setup build-asan --buildtype=debug -Dhardening=false -Db_sanitize=address,undefined -Db_lundef=false` |
| Fuzzing | `CC=clang CXX=clang++ meson setup build-fuzz -Dfuzzing=true -Dhardening=false -Db_sanitize=address,undefined -Db_lundef=false`, then `./build-fuzz/fuzz/fuzz-ber fuzz/corpus/ber` |
| Warnings as errors (as in CI) | add `-Dwerror=true` |

On macOS, Apple clang has no libFuzzer; use Homebrew LLVM (`CC=$(brew --prefix llvm)/bin/clang`) with `-Db_sanitize=undefined`. Homebrew LLVM 20's AddressSanitizer runtime deadlocks at startup on macOS 26, inside `AsanInitInternal`, so ASan fuzzing only works on Linux. farland itself targets Linux, and macOS is only for working on the platform-independent libraries.

Install the git hooks once with `pre-commit install`. They run clang-format, the REUSE license check and whitespace fixes.

## Rules for code that touches network input

These rules exist because most FreeRDP CVEs are missing bounds checks in parsers (docs/PLAN.md §6).

1. **Read received bytes only through `farland::Reader`.** No raw pointers and no pointer arithmetic. The protocol libraries are built with `-Werror=unsafe-buffer-usage` under Clang.
2. **Malformed input is a `Result` error, never an exception, assertion or crash.** Use `FARLAND_TRY` to propagate. `FARLAND_ASSERT` is for our own programming errors only, and it stays active in release builds.
3. **Check every length and count from the wire against its container and a farland limit** before allocating or looping.
4. **Every new parser gets a fuzz target** in `fuzz/`, with seeds in `fuzz/corpus/<target>/`. Where encoder and decoder both exist, the target checks that values round-trip.
5. **Cite the specification section** next to each structure (docs/SPECS.md).
6. **Protocol code is sans-IO:** no sockets, threads or clocks in `proto`, `auth` or `channels`. Bytes and events in, bytes and events out.

## Style

- C++23. `clang-format` (version pinned in `.pre-commit-config.yaml`) decides formatting.
- `snake_case` for functions, variables and namespaces; `PascalCase` for types; members end in `_`.
- Every file starts with SPDX headers:
  ```cpp
  // SPDX-FileCopyrightText: 2026 <your name>
  // SPDX-License-Identifier: Apache-2.0
  ```
  Code translated from FreeRDP keeps FreeRDP's copyright lines and adds a note saying it was modified (see NOTICE).

## Tests

- Unit tests use Catch2 and live in `tests/`, mirroring `src/farland/`.
- Protocol tests use real bytes: spec examples where available, otherwise connection transcripts (`tests/data/transcripts/README.md`).
