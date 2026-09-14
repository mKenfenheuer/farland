# farland

A modern Remote Desktop Protocol stack for Linux and Wayland, meant to succeed FreeRDP. It is written in C++ on its own sans-IO protocol core.

- **Server first:** a Wayland RDP server for any compositor, built on PipeWire, xdg-desktop-portal, libei and native Mutter/KWin/wlroots backends. Supports NLA (CredSSP with NTLMv2 or Kerberos), RDPGFX with ClearCodec, RFX Progressive, planar and AVC420/AVC444, clipboard, audio, multi-monitor and headless multi-session.
- **Then a client:** a native Wayland client with dmabuf rendering, fractional scaling, pointer constraints, IME, and RemoteApp windows.

Status: M4 in progress (sharing the running Wayland desktop through xdg-desktop-portal, PipeWire and libei), on top of the Graphics Pipeline with Progressive, planar and AVC420, NLA and privilege-separated pre-authentication. See:

- [docs/PLAN.md](docs/PLAN.md): scope, architecture, reference material, testing, security, risks
- [docs/ROADMAP.md](docs/ROADMAP.md): milestones M0–M8 (server 1.0), C1–C5 (client), phase 3

## Building

Requires GCC ≥ 13 or Clang ≥ 19 (on Ubuntu 24.04, `clang-19`), Meson ≥ 1.3 with Ninja, and OpenSSL ≥ 3.0 development files (`libssl-dev`, `openssl-devel`). Catch2 is fetched automatically if the system does not provide it. H.264 needs no library at build time: OpenH264 is loaded at runtime, and the x264 backend (`-Dx264=enabled`) is off by default because it makes the binaries GPL. On macOS, point pkg-config at Homebrew's OpenSSL: `export PKG_CONFIG_PATH=$(brew --prefix openssl@3)/lib/pkgconfig`.

```sh
meson setup build
meson compile -C build
meson test -C build
```

## Trying the server

farland-server serves a synthetic test desktop, or the running Wayland desktop with `--share` (below). Clients authenticate with NLA against a user file of NT hashes:

```sh
./build/apps/farlandctl passwd alice                # asks for the password; add --domain to pin a domain
./build/apps/farland-server --port 3389             # prints the certificate fingerprint
xfreerdp3 /v:localhost:3389 /u:alice /p:'…' /cert:tofu   # or mstsc / Windows App
```

- **Files:** the users live in `$XDG_CONFIG_HOME/farland/users` (mode 0600, `user:domain:NT hash`; an empty domain matches any), and the certificate and key in `$XDG_CONFIG_HOME/farland/tls/`. Both are created on first use. The server rereads the user file for every connection.
- **`--allow-tls-only`** also admits clients without NLA, which then reach the session without logging in; use it only for testing.
- **Privilege separation:** each client is served by a separate network process. That process handles TLS and NLA and, on Linux, runs as `nobody` when started as root, with Landlock and a seccomp filter. It asks the main process to verify NTLM responses and never sees a password hash. `--no-privsep` handles clients in the main process instead, for debugging.
- **Graphics:** clients that run the Graphics Pipeline (mstsc, Windows App, FreeRDP with `/gfx`) get RDPGFX with RemoteFX Progressive tiles. `--gfx-codec planar` sends lossless planar tiles, and `--gfx-codec avc420` sends H.264 through OpenH264 loaded at runtime (`--openh264 FILE`; Cisco's prebuilt `libopenh264.so.8` works). Clients without GFX, or without H.264 for `avc420`, fall back to Progressive or to planar bitmap updates. Every 5 seconds the log shows the GFX frame rate, the frames in flight and the round trip.
- **Sharing the desktop:** `--share` shows and controls the running Wayland desktop instead of the test pattern, and serves one session at a time. It uses xdg-desktop-portal: ScreenCast over PipeWire for the picture and the cursor, and RemoteDesktop with libei for input. It is tested on GNOME and meant for KDE and any other compositor whose portal supports RemoteDesktop. The first start shows the portal's permission dialog on the desktop; farland then keeps a restore token in `$XDG_STATE_HOME/farland/portal-restore-token`, so later starts connect without asking. `--virtual-monitor` shares a new virtual monitor (1920x1080 for now) where the portal offers one; it keeps its own token in `portal-restore-token-virtual`. This needs a Linux build with the portal backend (`libsystemd-dev`, `libpipewire-0.3-dev`, `libei-dev`).
- **The test desktop** shows color bars and a bouncing square. A crosshair follows the pointer and turns red while a button is held, and keys show up as colored cells at the bottom. `--log-level debug` logs every input event.

See [CONTRIBUTING.md](CONTRIBUTING.md) for sanitizer and fuzzing builds and the coding rules. See [docs/SPECS.md](docs/SPECS.md) for the specifications farland implements.

## License

Apache License 2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE).
