# farland

A modern Remote Desktop Protocol stack for Linux and Wayland, meant to succeed FreeRDP. It is written in C++ on its own sans-IO protocol core.

- **Server first:** a Wayland RDP server for any compositor, built on PipeWire, xdg-desktop-portal, libei and native Mutter/KWin/wlroots backends. Supports NLA (CredSSP with NTLMv2 or Kerberos), RDPGFX with ClearCodec, RFX Progressive, planar and AVC420/AVC444, clipboard, audio, multi-monitor and headless multi-session.
- **Then a client:** a native Wayland client with dmabuf rendering, fractional scaling, pointer constraints, IME, and RemoteApp windows.

Status: M0 (foundations). See:

- [docs/PLAN.md](docs/PLAN.md): scope, architecture, reference material, testing, security, risks
- [docs/ROADMAP.md](docs/ROADMAP.md): milestones M0–M8 (server 1.0), C1–C5 (client), phase 3

## Building

Requires GCC ≥ 13 or Clang ≥ 17, and Meson ≥ 1.3 with Ninja. Catch2 is fetched automatically if the system does not provide it.

```sh
meson setup build
meson compile -C build
meson test -C build
```

See [CONTRIBUTING.md](CONTRIBUTING.md) for sanitizer and fuzzing builds and the coding rules. See [docs/SPECS.md](docs/SPECS.md) for the specifications farland implements.

## License

Apache License 2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE).
