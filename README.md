# farland

A modern Remote Desktop Protocol stack for Linux and Wayland, meant to succeed FreeRDP. It is written in C++ on its own sans-IO protocol core.

- **Server first:** a Wayland RDP server for any compositor, built on PipeWire, xdg-desktop-portal, libei and native Mutter/KWin/wlroots backends. Supports NLA (CredSSP with NTLMv2 or Kerberos), RDPGFX with ClearCodec, RFX Progressive, planar and AVC420/AVC444, clipboard, audio, multi-monitor and headless multi-session.
- **Then a client:** a native Wayland client with dmabuf rendering, fractional scaling, pointer constraints, IME, and RemoteApp windows.

Status: M6 implemented (channels: resize and multi-monitor, clipboard, audio and microphone, touch and pen; tried with FreeRDP, not yet with mstsc or Windows App), on top of M5 (codecs and quality: AVC444, VA-API and NVENC, ClearCodec, network auto-detect with quality tiers), desktop sharing through xdg-desktop-portal, PipeWire and libei (M4, working on GNOME and Plasma), the Graphics Pipeline with Progressive, planar and AVC420, NLA and privilege-separated pre-authentication. The groundwork for headless multi-session (M7) is in. See:

- [docs/PLAN.md](docs/PLAN.md): scope, architecture, reference material, testing, security, risks
- [docs/ROADMAP.md](docs/ROADMAP.md): milestones M0–M8 (server 1.0), C1–C5 (client), phase 3

## Building

Requires GCC ≥ 13 or Clang ≥ 19 (on Ubuntu 24.04, `clang-19`), Meson ≥ 1.3 with Ninja, and OpenSSL ≥ 3.0 development files (`libssl-dev`, `openssl-devel`). Catch2 is fetched automatically if the system does not provide it. H.264 needs no library at build time: OpenH264 is loaded at runtime, and the x264 backend (`-Dx264=enabled`) is off by default because it makes the binaries GPL. H.264 on the GPU through VA-API is built when libva is installed (`libva-dev`, `libva-devel`; `-Dvaapi=disabled` turns it off). On macOS, point pkg-config at Homebrew's OpenSSL: `export PKG_CONFIG_PATH=$(brew --prefix openssl@3)/lib/pkgconfig`.

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
- **Graphics:** clients that run the Graphics Pipeline (mstsc, Windows App, FreeRDP with `/gfx`) get RDPGFX with RemoteFX Progressive tiles. `--gfx-codec planar` sends lossless planar tiles.
  - **H.264:** `--gfx-codec avc420` sends H.264, and `--gfx-codec avc444` sends full-colour 4:4:4 H.264 (AVC444 and AVC444v2, checked against FreeRDP's decoder; not yet tried with mstsc).
  - **Encoders:** H.264 runs on the GPU where one encodes it, through NVENC on NVIDIA or VA-API on AMD and Intel, and otherwise through OpenH264 loaded at runtime (`--openh264 FILE`; Cisco's prebuilt `libopenh264.so.8` works). `--h264-encoder auto|nvenc|vaapi|openh264|x264` and `--render-node PATH` choose explicitly. With `--share` and AVC420, the captured frames go to a GPU encoder as dmabufs, without a copy through CPU memory (`--no-zero-copy` turns that off).
  - **Text and pictures:** on Progressive surfaces, tiles with few colours (text, UI) go through ClearCodec and stay sharp, while pictures go out coarse first and are refined while they stand still (`--no-clearcodec`, `--no-refine`).
  - **Fallbacks:** clients without GFX, or without H.264, fall back to Progressive or to planar bitmap updates.
  - **Scrolling:** on Progressive and planar surfaces, a scrolled document is moved on the client and only the uncovered rows are encoded.
  - **Monitors and resizing:** the client's monitors (CS_MONITOR: FreeRDP `/multimon` or `/monitors`, mstsc and Windows App with "use all my monitors") apply at connect time, and the Display Control channel ([MS-RDPEDISP]) resizes the desktop and changes the monitors while connected (FreeRDP `/dynamic-resolution`, a resized mstsc or Windows App window). A burst of layouts while a window edge is dragged is applied once it settles. GFX clients get one surface per monitor and a ResetGraphics with the monitor list; clients without GFX are reactivated at the new size. The test pattern shows on every monitor.
  - **Network:** clients that support network auto-detect have their round trip and bandwidth measured (`--autodetect full|continuous|off`). Together with the frame acknowledgements, this picks one of four quality tiers (frame rate, Progressive quantisation, H.264 bitrate).
  - **Log:** every 5 seconds it shows the GFX frame rate, the frames in flight, the round trip, the measured network and the tier.
- **Sharing the desktop:** `--share` shows and controls the running Wayland desktop instead of the test pattern, and serves one session at a time. Every monitor picked in the portal dialog is shared, each on one of the client's monitors from left to right (a single one on the client's primary monitor). A client with one monitor and no display control sees the shared monitors side by side at their size. Where a monitor and the client's monitor differ in size, the picture is centred with black borders, and scaled down when it is larger (by the client where it supports MapSurfaceToScaledOutput, otherwise by farland); pointer input follows the picture. It uses xdg-desktop-portal: ScreenCast over PipeWire for the picture and the cursor, and RemoteDesktop with libei for input. It is tested on GNOME 50 and Plasma 6.6, and meant for any other compositor whose portal supports RemoteDesktop. The first start shows the portal's permission dialog on the desktop; farland then keeps a restore token in `$XDG_STATE_HOME/farland/portal-restore-token`, so later starts connect without asking. `--virtual-monitor` shares a new virtual monitor where the portal offers one: on GNOME it takes the size of the client's (primary) monitor and follows the client's window when it is resized, since Mutter sizes a virtual monitor to what its screen cast consumer asks for; KWin's portal creates it at 1920x1080 and keeps that size, so there it is scaled and centred. A portal session has one virtual monitor, so further client monitors stay black. It keeps its own token in `portal-restore-token-virtual`. This needs a Linux build with the portal backend (`libsystemd-dev`, `libpipewire-0.3-dev`, `libei-dev`).
- **Touch and pen:** clients with a touchscreen or a pen (Windows App on a tablet, mstsc on a Surface) send their touch and pen frames over the RDPEI channel. With `--share`, fingers become libei touches on the compositor's touchscreen device (Plasma 6.6 and GNOME offer one), mapped like the pointer; where there is none, the first finger drives the pointer. libei has no tablet devices, so the pen moves the pointer and holds the left button while it touches (the right one with the barrel button pressed); pressure and tilt are dropped.
- **The test desktop** shows color bars and a bouncing square. A crosshair follows the pointer and turns red while a button is held, keys show up as colored cells at the bottom, and touch and pen contacts as squares. `--log-level debug` logs every input event.
- **Audio:** what the desktop plays goes to the client's speakers, and the client's microphone shows up as a local audio source named "farland microphone" (`farland-microphone`) while the client records. Both use the user's own PipeWire daemon, with `--share` and with the test desktop. Playback runs over the AUDIO_PLAYBACK_DVC dynamic channel, or the rdpsnd static channel where the client has no such channel. Clients that ask for dynamic or medium quality get Opus (libopus, loaded at runtime), and clients that ask for high quality (FreeRDP's default) or have no Opus get 16-bit PCM. Silence sends nothing, and the client's wave confirmations pace the stream: audio is dropped rather than queued when the network falls behind. `--no-audio` and `--no-microphone` turn either off; the client decides too (mstsc's "play on this computer" and "record from this computer", FreeRDP's `/sound` and `/microphone`).
- **Clipboard:** with `--share`, text, HTML, images and files are copied both ways between the client and the desktop (cliprdr, through the portal's Clipboard interface), and only transferred when someone pastes. Files the client copies are fetched when the desktop pastes them, into a private directory under `$XDG_RUNTIME_DIR/farland/` (up to 1 GiB per paste), and removed when the session ends; files the desktop copies are read directly, symlinks inside folders are not followed. PNG images need zlib (`-Dpng`); without it images go as BMP. `--no-clipboard` turns the clipboard off. The test desktop has a loopback clipboard instead: what the client copies is fetched and offered back, so pasting it again shows the round trip through the server.

See [CONTRIBUTING.md](CONTRIBUTING.md) for sanitizer and fuzzing builds and the coding rules. See [docs/SPECS.md](docs/SPECS.md) for the specifications farland implements.

## License

Apache License 2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE).
