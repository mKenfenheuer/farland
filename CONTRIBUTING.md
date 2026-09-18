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
| Benchmarks (not run by `meson test`) | `meson setup build-rel --buildtype=release`, then `meson compile -C build-rel bench-progressive && ./build-rel/benchmarks/bench-progressive` |

GitHub CI runs one job only: a release build with `-Dwerror=true`, one `meson test` run, one run of the release binary, and the Debian package (`sh packaging/make-deb.sh BUILD_DIR OUTPUT_DIR`, needs `dpkg-dev`), which it installs to check that the `farlandd` service starts. That keeps runner minutes low, so everything else is on you before you push: the sanitizer, fuzz and clang-tidy builds above, the other compilers (GCC 13 to 16, Clang 19 to 21) and distributions, and the compositor tests.

On macOS, use Apple clang for the sanitizer builds (ASan, UBSan and TSan all work). Apple clang has no libFuzzer, though, so fuzz with Homebrew LLVM (`CC=$(brew --prefix llvm)/bin/clang`) and `-Db_sanitize=undefined` only: Homebrew LLVM 20's AddressSanitizer runtime deadlocks at startup on macOS 26, inside `AsanInitInternal`. ASan fuzzing therefore only works on Linux. farland itself targets Linux, and macOS is only for working on the platform-independent libraries.

Tests that need an H.264 encoder are skipped unless one is available. To run them, set `FARLAND_OPENH264_LIBRARY` to a libopenh264 (Cisco's prebuilt binaries from ciscobinary.openh264.org work) or configure with `-Dx264=enabled`. `FARLAND_FFMPEG` and `FARLAND_FFPROBE` point the H.264 quality tests at ffmpeg.

The VA-API backend is built when `libva-dev` (libva and libva-drm >= 2.14) is installed (`-Dvaapi=enabled` insists on it). Its tests need a GPU with a VA-API driver (`mesa-va-drivers` or `intel-media-va-driver`) and access to its render node, and skip otherwise; `FARLAND_VAAPI_DEVICE=/dev/dri/renderD129` picks the node on machines with several GPUs. `build/tests/video/farland-vaapi-bench` measures CPU time and latency of 4K encoding from dmabufs (`--cpu` adds the CPU conversion path).

The NVENC backend (`-Dnvenc`, on by default on Linux) needs nothing to build: libnvidia-encode, libcuda and libEGL come with the NVIDIA driver (520 or newer) and are loaded at runtime. Its tests skip without an NVIDIA GPU; the dmabuf tests also need `libgbm-dev` and `libegl-dev` and access to the GPU's render node (group `render`, or a desktop session on the seat). `FARLAND_VAAPI_DEVICE` picks the render node on machines with several GPUs. `build/tests/video/farland-nvenc-bench` measures CPU time and latency of 4K encoding from dmabufs (`--cpu` adds the CPU conversion path, `--preset 1..7` and `--low-latency` change the NVENC tuning).

The portal backend (Linux only) needs `libsystemd-dev`, `libei-dev` and `libpipewire-0.3-dev`, with `libgbm-dev` and `libegl-dev` optional for tiled dmabufs. Its tests skip what they cannot run:
- the portal session tests need `dbus-daemon`, `python3-dbus` and `python3-gi`, and run `tests/platform/portal/mock_portal.py` on a private bus;
- the libei tests need `libeis-dev`;
- the capture tests need the `pipewire` daemon, which they start privately; the ones that pass dmabufs on also need read-write access to `/dev/udmabuf` (logind grants it to the user at the seat on Ubuntu), and skip without it.

`build/tests/platform/portal/farland-portal-probe` tries a real portal session on a desktop.

The wlroots backend (Linux only) needs `libwayland-dev`, `libxkbcommon-dev` and, preferably, `wayland-protocols` (older releases fall back to the protocol copies in `src/farland/platform/wlroots/protocols/`). Its keymap tests need xkeyboard-config (`xkb-data`). Its compositor tests start headless `sway`, `labwc` and `cage` with the pixman renderer and skip the ones not installed; sway refuses to run as root, so run them as a user (`build/tests/farland-unit-tests "[wlroots]"`).

The Mutter backend (headless GNOME) is built with the portal backend, and `libxkbcommon-dev` adds keymaps. Its tests run `tests/platform/mutter/mock_mutter.py` on a private bus and skip without the same Python modules; `build/tests/platform/mutter/farland-gnome-headless-probe 1600x900 1600x900,1280x1024` starts a real headless GNOME Shell and walks it through monitor layouts.

farlandd (multi-session) uses `libsystemd-dev` for logind, GDM and the users' service managers, and `libpam0g-dev` (`-Dpam`) for its own login sessions; both are optional, and the end-to-end tests run it without either (`--no-pam`). The headless desktop backends are chosen with `-Dheadless-gnome`, `-Dheadless-plasma` and `-Dheadless-wlroots`; a missing one leaves a stub that fails at runtime.

Install the git hooks once with `pre-commit install`. They run clang-format, the REUSE license check and whitespace fixes.

## Rules for code that touches network input

These rules exist because most FreeRDP CVEs are missing bounds checks in parsers (docs/PLAN.md §6).

1. **Read received bytes only through `farland::Reader`.** No raw pointers and no pointer arithmetic. The protocol libraries are built with `-Werror=unsafe-buffer-usage` under Clang.
2. **Malformed input is a `Result` error, never an exception, assertion or crash.** Use `FARLAND_TRY` to propagate. `FARLAND_ASSERT` is for our own programming errors only, and it stays active in release builds.
3. **Check every length and count from the wire against its container and a farland limit** before allocating or looping.
4. **Every new parser gets a fuzz target** in `fuzz/`, with seeds in `fuzz/corpus/<target>/`. Where encoder and decoder both exist, the target checks that values round-trip.
5. **Cite the specification section** next to each structure (docs/SPECS.md).
6. **Protocol code is sans-IO:** no sockets, threads or clocks in `proto`, `auth` or `channels`. Bytes and events in, bytes and events out.
7. **Pre-authentication runs sandboxed.** Everything a client sends before NLA succeeds is parsed in the network process (`apps/farland-server/privsep_process.cpp`). That process cannot open files, create sockets or start processes, and gets randomness only from OpenSSL's `RAND_bytes`. Code on that path that needs another system call must add it to the seccomp allowlist in `apps/farland-server/sandbox.cpp`, with a comment saying why. Without that, the call fails with `EPERM`. Test it with the privilege-separation tests on Linux.

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
