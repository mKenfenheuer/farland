# farland

A modern Remote Desktop Protocol stack for Linux and Wayland, meant to succeed FreeRDP. It is written in C++ on its own sans-IO protocol core.

- **Server first:** a Wayland RDP server for any compositor, built on PipeWire, xdg-desktop-portal, libei and native Mutter/KWin/wlroots backends. Supports NLA (CredSSP with NTLMv2 or Kerberos), RDPGFX with ClearCodec, RFX Progressive, planar and AVC420/AVC444, clipboard, audio, multi-monitor and headless multi-session.
- **Then a client:** a native Wayland client with dmabuf rendering, fractional scaling, pointer constraints, IME, and RemoteApp windows.

Status: M1 (connection core over TLS, synthetic test desktop). See:

- [docs/PLAN.md](docs/PLAN.md): scope, architecture, reference material, testing, security, risks
- [docs/ROADMAP.md](docs/ROADMAP.md): milestones M0–M8 (server 1.0), C1–C5 (client), phase 3

## Building

Requires GCC ≥ 13 or Clang ≥ 17, Meson ≥ 1.3 with Ninja, and OpenSSL ≥ 3.0 development files (`libssl-dev`, `openssl-devel`). Catch2 is fetched automatically if the system does not provide it. On macOS, point pkg-config at Homebrew's OpenSSL: `export PKG_CONFIG_PATH=$(brew --prefix openssl@3)/lib/pkgconfig`.

```sh
meson setup build
meson compile -C build
meson test -C build
```

## Trying the server

M1 serves a synthetic test desktop over TLS (NLA follows in M2, Wayland capture in M4):

```sh
./build/apps/farland-server --port 3389           # prints the certificate fingerprint
xfreerdp3 /v:localhost:3389 /sec:tls /cert:tofu    # or mstsc / Windows App with NLA disabled
```

The certificate and key are created in `$XDG_CONFIG_HOME/farland/tls/` on first start. The test desktop shows color bars and a bouncing square; a crosshair follows the pointer and turns red while a button is held, and keys show up as colored cells at the bottom. `--log-level debug` logs every input event.

See [CONTRIBUTING.md](CONTRIBUTING.md) for sanitizer and fuzzing builds and the coding rules. See [docs/SPECS.md](docs/SPECS.md) for the specifications farland implements.

## License

Apache License 2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE).
