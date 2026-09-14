# farland: roadmap

The phases follow the decisions in [PLAN.md](PLAN.md): the server first, in C++, on our own protocol core. Durations are rough estimates for 1–2 full-time engineers and assume the macRDP and ZeroVDI code can be ported. Every milestone ends with a tagged pre-release and a passing interop matrix.

```
Phase 1: server 1.0                                         Phase 2: client          Phase 3
M0 ─ M1 ─ M2 ─ M3 ─ M4 ─ M5 ─ M6 ─ M7 ─ M8 ─► server 1.0     C1 ─ C2 ─ C3 ─ C4 ─ C5 ─► 1.0   U1 UDP, AV1, RAIL server
~2   ~5   ~4   ~7   ~6   ~7   ~7   ~7   ~4  weeks (≈ 12 months)       (≈ 9 months)
```

**Status (2026-09-14):** M0 to M3 are done; each has a status note below that lists what differs from the plan and what is still untested. M4 is implemented and works on GNOME and Plasma; the latency target is still to be measured. M5 is in progress.

Some milestones can overlap: once M3 is done, M5 (codecs) and M6 (channels) can run in parallel with M4 and M7 if there are two engineers.

---

## Phase 1: server

### M0: Foundations (~2 weeks): done
- Set up the repo with Meson, clang-format/clang-tidy and a pre-commit hook. CI on GCC and Clang: debug, ASan+UBSan, TSan and hardened-release builds.
- CI containers: Ubuntu 24.04 (the oldest baseline: GCC 13, Clang 19 from `clang-19`, Meson 1.3), Debian 13, current Fedora and Arch, each with GCC and Clang. Fedora 40 is past end of life and its repositories are archived, so current Fedora stands in for it. A build that needs anything newer than the baseline fails CI.
- SPDX license headers (Apache-2.0) on every file, checked in CI with the REUSE tool. Files translated from FreeRDP keep FreeRDP's copyright notice and carry a "modified" note (see NOTICE).
- `farland-base`: bounded `Reader`/`Writer`, `expected` error model, BER/PER/DER codecs, logging, and a hex-dump test helper.
- Fuzz harness scaffolding (libFuzzer plus a corpus directory layout), and a transcript-replay test harness.
- Spec index: map the MS-* documents to sections, and follow the convention of citing `[MS-RDPBCGR] 2.2.1.3.2` style references in code.
- **Exit:** CI green on all build variants; the first fuzz target (BER/PER) runs nightly.
- **Status: done.** The CI workflow has all the build variants and a nightly fuzz job, but it has not run on GitHub yet because nothing has been pushed. The same builds pass locally: Apple clang and Homebrew LLVM on macOS (including ASan+UBSan and TSan), and GCC 15 on Ubuntu 26.04.

### M1: Connection core, TLS only (~5 weeks): done
- **X.224:** full `RDP_NEG_REQ`/`RSP`/`FAILURE` handling, skipping the cookie and routing token, and correct `RSP` flags (`EXTENDED_CLIENT_DATA_SUPPORTED`, `DYNVC_GFX_PROTOCOL_SUPPORTED`).
- **TLS server** (OpenSSL 3): self-signed certificate generated with the serverAuth EKU, or a user-supplied certificate; the fingerprint is shown in `farlandctl`.
- **MCS/GCC with real parsers:**
  - Client blocks: CS_CORE (all optional fields), CS_CLUSTER, CS_SECURITY, CS_NET, CS_MONITOR/EX, CS_MCS_MSGCHANNEL, CS_MULTITRANSPORT.
  - Server blocks: SC_CORE (echoing `requestedProtocols`), SC_SECURITY, SC_NET, SC_MCS_MSGCHANNEL.
  - Channel ID allocation that cannot collide with the user channel.
  - Honour skip-channel-join.
- **Client Info PDU:** fully parsed, including autologon, time zone, extended info and the ARC cookie.
- **Licensing:** `STATUS_VALID_CLIENT`.
- **Capability exchange:** Demand Active with General, Bitmap (32 bpp), Order, Pointer, Input, VirtualChannel, Share, Font and MultifragmentUpdate. The client's Confirm Active is fully parsed into the session. SurfaceCommands, BitmapCodecs and FrameAcknowledge are advertised only once they are used (M3), and LargePointer with the cursor pipeline (M4): advertising a capability the server never exercises only widens the attack and interop surface.
- **Finalization:** wait for the client's Synchronize/Control/FontList before replying; Set Error Info on disconnect; Deactivate-All/reactivation.
- **Fast-path and slow-path input**, Refresh Rect, Suppress Output, Shutdown Request, heartbeat.
- **Legacy output:** fast-path bitmap updates with the **planar** codec, as the first working pixels.
- Sans-IO state machine, plus a `test` backend with a synthetic pattern and an input log.
- **Exit:**
  - mstsc (Windows 11), Windows App (macOS) and FreeRDP 2 and 3 connect over TLS-only, show the test pattern, and input shows up in the log.
  - The §4.1 regression tests pass.
  - Fuzz targets exist for X.224, GCC, capability sets and fast-path input.
- **Status: done, with these differences:**
  - `farland-server --fingerprint` prints the certificate fingerprint; `farlandctl` does not.
  - The server sends heartbeat PDUs since M5, only to clients that set the heartbeat flag.
- **Tested:**
  - FreeRDP 3 (xfreerdp3 3.31) connects over TLS, shows the test pattern and delivers input.
  - The §4.1 regression tests pass.
  - The X.224, MCS/GCC, capability (share), input, planar and connection fuzz targets exist.
- **Not tested yet:** mstsc, Windows App and FreeRDP 2.

### M2: NLA (~4 weeks): done except Kerberos
- **CredSSP acceptor**, TSRequest v2–6:
  - pubKeyAuth using the nonce hash (v5+) or pubKey+1 (v2–4), bound to the PKCS#1 key.
  - Sends `errorCode` TSRequests on failure, with correct NTSTATUS values.
  - Decodes TSCredentials types 1, 2 and 6, and rejects the unsupported ones cleanly.
- **NTLMv2 server:**
  - Full AV pairs, and the negotiated flags intersected with the client's.
  - **MIC verification**, **channel-binding verification** (`tls-server-end-point`), and a target-name check.
  - Username comparison; macRDP accepted any username that came with the right password.
- **HYBRID_EX** with an Early User Authorization Result. The server refuses standard RDP security.
- **Credential store:** NT hash at mode 0600; `farlandctl passwd`.
- **SPNEGO and Kerberos acceptor** via GSSAPI with a keytab (can slip into M7 if needed).
- **Pre-auth privilege separation:** a separate unprivileged process with seccomp/landlock that hands off the file descriptor and TLS session.
- **Exit:**
  - The whole client matrix connects with NLA.
  - A wrong password shows the client's native "logon failure" message.
  - Fuzz targets for TSRequest and NTLM run nightly.
- **Status: done except Kerberos.** The Kerberos acceptor moves to M7, where the other authentication modes are. SPNEGO is done: it negotiates NTLM, including the mechListMIC exchange.
- **Differences from the plan:**
  - Privilege separation relays the decrypted stream over a socket pair. It does not hand over the socket and TLS session, because a TLS session cannot move between processes without kernel TLS.
  - The network process switches to `nobody` when started as root. On Linux it runs under Landlock and a seccomp allowlist.
  - It sends NTLM responses to the main process for checking and never sees a hash. The main process accepts only an identity it verified itself.
  - The NTLM target name is recorded but not enforced.
  - Only password credentials can be delegated. Smart-card and Remote Guard credentials are decoded and then refused.
- **Tested:**
  - FreeRDP 3 connects with NLA over HYBRID and over HYBRID_EX, with and without a domain.
  - A wrong password gives `ERRCONNECT_LOGON_FAILURE`.
  - A TLS-only client gets `HYBRID_REQUIRED_BY_SERVER`.
  - End-to-end tests cover the in-process and privilege-separated paths.
  - The fuzz targets are `credssp` (TSRequest and SPNEGO), `ntlm` and `privsep`.
- **Not tested yet:** mstsc, Windows App and FreeRDP 2.

### M3: Graphics pipeline, first GFX codecs (~7 weeks): done
- **drdynvc server**, caps v1–v3: 1/2/4-byte channel IDs, `DATA_FIRST`/`DATA`, Close, and reading compressed client data.
- **RDPGFX server:**
  - Caps 8.0 through 11.x with correct lengths.
  - Command set: ResetGraphics, Create/Delete surface, Map surface to output (including scaled), Start/End frame, WireToSurface1/2, SolidFill, SurfaceToSurface, and SurfaceToCache/CacheToSurface/Evict.
  - CacheImportOffer/Reply.
  - A **ZGFX compressor**; the earlier servers only sent raw segments.
- **Frame scheduler:**
  - Damage-driven, with a frames-in-flight window, `queueDepth`/QoE-aware pacing and `SUSPEND_FRAME_ACK`.
  - Re-encode dropped frames on ack (from macRDP).
  - A single writer per channel.
- **Codecs:**
  - **Planar** (lossless).
  - **RFX Progressive**, ported from macRDP, respecting the 16 KB mstsc cap.
  - **AVC420** on OpenH264 and x264 software backends, with real region rectangles and QP metadata.
- Legacy fallback for clients without GFX: fast-path bitmap updates with planar (planar is not a SurfaceBits codec).
- **Exit:**
  - A 1080p test-pattern animation reaches all clients through GFX.
  - FreeRDP's decoders verify our codec output: bit-exact for planar, PSNR ≥ 40 dB for Progressive at its final quality stage.
  - The drdynvc and rdpgfx client→server fuzzers run.
- **Status: done, with these gaps:**
  - The session does not use SurfaceToSurface or the surface cache yet, although the RDPGFX layer implements both. It answers CacheImportOffer with an empty reply.
  - Pacing uses the frames-in-flight window and SUSPEND_FRAME_ACKNOWLEDGEMENT; queueDepth and QoE acknowledgements are not used yet. Dropped frames are not re-encoded as such: their damage stays pending, so the next frame covers it.
  - AVC420 always encodes the whole surface as one H.264 picture; region rectangles tell the client which parts to copy. OpenH264 is loaded at runtime, and x264 is an opt-in build because it makes the binaries GPL.
- **Tested:**
  - FreeRDP 3 (xfreerdp3 3.31) runs the test pattern at 1920×1080 over RDPGFX 10.7 with Progressive and with planar: 30 fps with frames acknowledged in 7–8 ms. drdynvc runs at version 3.
  - A scripted client in the unit tests checks the whole path (drdynvc, caps, ZGFX, surface setup, frames): planar pixel for pixel, Progressive to at least 30 dB, and the AVC420 region layout.
  - Independent decoders:
    - FreeRDP's `zgfx.c` and ZeroVDI's decoder reproduce the ZGFX output exactly.
    - ZeroVDI's decoder reproduces the Progressive output bit for bit.
    - ffmpeg decodes both H.264 backends at 44–47 dB.
  - The fuzz targets are `svc`, `drdynvc`, `zgfx`, `rdpgfx`, `progressive` and `avc420`.
- **Not tested yet:**
  - mstsc and Windows App.
  - AVC420 against a real client. Ubuntu's FreeRDP is built without H.264; against it, the server falls back to Progressive as intended.

### M4: Wayland capture and input, portal backend (~6 weeks)
- **PipeWire consumer:**
  - Format and modifier negotiation, dmabuf import (EGL/GBM mmap for CPU codecs), SHM fallback.
  - `SPA_META_VideoDamage` feeds the damage tracker.
  - `SPA_META_Cursor` feeds the **cursor pipeline**: Pointer PDUs, LargePointer and position updates, and the cursor is never baked into the video.
- **Portal session:**
  - RemoteDesktop + ScreenCast, with persisted **restore tokens**.
  - Multiple streams, one per monitor.
- **libei via `ConnectToEIS`:**
  - An RDP scancode→evdev table (e0 prefixes, Pause, and so on).
  - Absolute pointer mapped per EIS region, with letterbox mapping when resolutions differ.
  - Buttons 1–5, v120 scroll (vertical and horizontal), and touch.
  - Falls back to the portal's `Notify*` methods.
- **Unicode input:** best effort (see PLAN §7).
- `farland-server --share`: a user-session service that mirrors the running desktop.
- **Exit:**
  - Full control of GNOME 48+ and Plasma 6.x desktops from mstsc and FreeRDP.
  - Glass-to-glass latency on a LAN is at most 50 ms at 1080p60 with AVC420.
- **Status: implemented; works on GNOME with mstsc, Windows App, ZeroVDI and FreeRDP, and on Plasma with FreeRDP. The latency target is not measured yet.**
  - Done:
    - The backend interface (`FrameSource`, `CursorSource`, `InputSink`).
    - The portal client: RemoteDesktop and ScreenCast over sd-bus, restore tokens, and the Notify* fallback.
    - PipeWire capture: eight pixel layouts, shared memory, and LINEAR or GBM-imported dmabufs, with damage and cursor metadata.
    - libei input, with absolute pointers per region and no stuck keys.
    - The scancode→evdev table, checked against FreeRDP's.
    - The cursor pipeline: Pointer and LargePointer capabilities, 32 bpp pointers and a pointer cache.
    - `farland-server --share`.
  - Tested:
    - In Docker, against a mock portal, a private PipeWire daemon and an in-process EIS server.
    - An end-to-end test of the session with a fake desktop.
    - On a real desktop (2026-09-14): GNOME 50 on Ubuntu 26.04 (xdg-desktop-portal 1.21, PipeWire 1.6, libei 1.5), in a VM without a GPU, so over shared memory:
      - mstsc, Windows App and ZeroVDI control the shared monitor with keyboard, mouse and cursor shapes; FreeRDP 3 shows it over RDPGFX 10.7 with Progressive.
      - The restore token skips the dialog on later starts.
      - `--virtual-monitor` gets a new 1920x1080 monitor from mutter and shares it.
    - Plasma 6.6 on Kubuntu 26.04 (xdg-desktop-portal-kde), in a VM with a virtio GPU: FreeRDP 3 shows the desktop and controls it with keyboard and mouse through libei. KWin sends LINEAR dmabufs, which farland maps and reads; this is the first run of the dmabuf path.
  - Differences from the plan:
    - `--share` shares one monitor and serves one session at a time. A resized desktop is not followed yet; that comes with the disp channel (M6).
    - The session read each frame into CPU memory; since M5, AVC420 on VA-API takes the captured dmabufs directly.
    - Unicode input is not typed through libei, which has no text input.
    - A virtual monitor is always 1920x1080; it does not take the client's size yet.
  - Not tested yet: mstsc on Plasma, tiled (GPU-imported) dmabufs, AVC420 against a real client, input on the virtual monitor, and the latency exit criterion.

### M5: Codecs 2, quality and efficiency (~7 weeks)
- **VA-API encoder:** dmabuf → VASurface zero-copy (Intel/AMD), with the colour conversion on the GPU. **NVENC** optional.
- **True AVC444 v1/v2:** luma plus chroma streams, split per MS-RDPEGFX 3.3.8.3. Colour matrix and range verified against FreeRDP.
- **ClearCodec encoder:** residual, bands, V-bar and short V-bar caches, the RLEX subcodec, and the glyph cache. The ZeroVDI `clear.js` and FreeRDP decoders serve as oracles.
- **Region classifier and mixed mode:**
  - Text/UI regions go to Clear; static content goes to Progressive with UPGRADE passes; video regions go to AVC.
  - Scroll and move detection maps to SurfaceToSurface.
- **Progressive completed:** real `TILE_UPGRADE`, RLGR3, reduce-extrapolate, SIMD.
- **Congestion control:** auto-detect RTT and bandwidth (MS-RDPBCGR 2.2.14) drive a quality and bitrate ladder (the ZeroVDI tier design).
- **Exit:**
  - Benchmarks for bitrate, CPU and latency are published per codec, together with a content corpus.
  - Text is sharp at low bandwidth (2 Mbit/s).
  - VA-API runs at 4K30 with less than 10% of one core.
- **Status: in progress.**
  - Done:
    - Network auto-detect ([MS-RDPBCGR] 2.2.14): connect-time RTT and bandwidth measurement, continuous RTT probes and bandwidth measured on real frames, the MCS message channel and heartbeats. Only for clients that advertise it; `--autodetect full|continuous|off`.
    - The quality ladder (`QualityController`, ZeroVDI's four tiers): queueing delay, slow frame acknowledgements, the client's queue depth and the measured bandwidth set frame rate, Progressive quantisation and H.264 bitrate, with hysteresis.
    - Scroll detection on Progressive and planar surfaces:
      - When at least 4 tiles changed, `detect_vertical_scroll` compares row hashes per 64-pixel column strip against the last frame and verifies the move byte by byte.
      - A found move goes out as one SurfaceToSurface, and only the uncovered rows are encoded.
      - H.264 surfaces leave motion to the encoder.
    - The VA-API H.264 encoder (`--h264-encoder`, `--render-node`):
      - Constrained baseline, main and high, CBR/VBR/CQP, and packed headers where the driver needs them (radeonsi).
      - Zero-copy input from dmabufs (linear and tiled modifiers), with the colour conversion on the GPU; `ColorSpace` sets the matrix and range.
      - Measured on an RX 6900 XT (Mesa 26 radeonsi): 4K30 from dmabufs costs 0.41 ms of CPU per frame, 1.2% of one core.
    - Zero-copy from the screen capture to the H.264 encoder (AVC420 with an encoder that takes dmabufs):
      - The session asks the capture for dmabufs (`FrameAccess`). The capture then reads nothing: it keeps the newest buffer dequeued and hands its descriptors to the session, which gives it back with the next frame. It holds at most two buffers, and only on streams with four or more, so the producer never runs dry.
      - The AVC420 regions come from the capture's damage (`SPA_META_VideoDamage`, merged over skipped frames), since there are no pixels to diff; an IDR or an invalidation still lists the whole surface.
      - Everything else keeps reading frames into CPU memory: Progressive, planar, AVC444 (its 4:4:4 split runs on the CPU), bitmap updates, shared memory and cropped streams. A buffer the encoder refuses is read for that one frame; after three in a row the session stays on the CPU path. `--no-zero-copy` turns it off.
      - The capture offers the modifiers the encoder's GPU imports (EGL on the encoder's render node), and the encoder drops its cached imports when PipeWire renegotiates the buffers.
      - Measured on GNOME 50 with the RX 6900 XT (a 1280x800 VM monitor with a full-screen test video): mutter hands out tiled dmabufs (modifier `0x200000020801b03`), which VA-API imports and encodes correctly. The whole server process costs 8.5 ms of CPU per frame at 15 fps, against 36.7 ms per frame when the same buffers are read into CPU memory (`--no-zero-copy`), which then keeps up with only 5 fps. How the 8.5 ms splits between the session, ZGFX, TLS and PipeWire is not measured yet.
    - True AVC444 and AVC444v2 (`--gfx-codec avc444`):
      - The full-chroma picture is split into the main and auxiliary views per [MS-RDPEGFX] 3.3.8.3, and both go through one H.264 encoder, as the spec requires.
      - Main-view chroma is the 2x2 average, as FreeRDP sends it. A policy chooses LC 0/1/2 per frame and can hold chroma back for later.
      - v2 is used only for widths that are a multiple of 32; above that, implementations differ on the auxiliary view's layout.
      - Negotiation falls back to AVC420, then Progressive.
      - FreeRDP 3.31 decodes the streams (worst RGB PSNR 31 dB on coloured text, against 18.5 dB for 4:2:0 on the same content).
      - The AVC420 colour conversion was checked on the way: full-range BT.709 with FreeRDP's coefficients, as the spec requires.
      - On the tiers "low" and "minimal" the quality ladder holds chroma back until the picture stops changing.
    - The ClearCodec encoder and a hardened decoder ([MS-RDPEGFX] 2.2.4.1), used for text and UI tiles (below):
      - Residual, bands with V-bar and short V-bar caches mirroring the client's, the RLEX subcodec, and the glyph cache.
      - The encoder costs every strip in each layer and takes the cheapest. A column seen a second time goes into the V-bar cache, after which recurring text costs about 2 bytes per column.
      - NSCodec is left out: photo-like content goes to Progressive or AVC.
      - 4604 streams (all patterns, cache overflow and eviction) decode pixel-exact in FreeRDP 3.15 and in ZeroVDI's clear.js.
    - Progressive completed:
      - Refinement passes: a tile goes out first at the coarsest of four quality stages (TILE_FIRST), then TILE_UPGRADE passes within a byte budget refine it. At full quality it is bit-identical to a single-pass tile.
      - The reduce-extrapolate DWT in the encoder; refinement requires it, because FreeRDP reads upgrades in that band layout.
      - SIMD: hand-written SSE2, AVX2 and NEON colour conversion, and vectorised DWT, quantisation and RLGR, chosen at run time and checked bit for bit against the scalar code. A 1080p frame encodes 2 to 3.4 times faster (x86-64, `quant_default`: 46.8 ms before, 13.8 ms after; `bench-progressive`).
      - FreeRDP 3.15's decoder with its generic C primitives gives identical pixels on every frame, first passes and upgrades alike. FreeRDP's own SSE and NEON code differs from its C code by at most 1.
      - RLGR3 is not used: the Progressive stream cannot signal it, and FreeRDP always decodes RLGR1.
    - The NVENC H.264 encoder (`--h264-encoder nvenc`; tried first where built, as it fails fast without an NVIDIA driver):
      - The NVENC API 12.0 and CUDA are loaded at runtime from the driver (520 or newer), so building needs no NVIDIA SDK or CUDA toolkit. The API declarations are checked against NVIDIA's header.
      - CUDA on driver 595 cannot import dmabufs. So the dmabuf goes through EGL and OpenGL into a texture registered with CUDA, and a small kernel converts it to NV12, bit-identical to the CPU conversion.
      - Measured on an RTX 3090 (driver 595): 4K30 from tiled dmabufs costs 1.07 ms of CPU per frame (3.2% of one core) at 10 ms latency with the P4 preset. Through CPU memory it costs 16 ms (48%).
    - Mixed mode on Progressive surfaces (`--no-clearcodec`, `--no-refine` switch it off):
      - Every changed 64x64 tile is classified by counting its colours, stopping at the 49th. Tiles with few colours (text, UI) go through ClearCodec, one region per run of adjacent tiles in a row; the rest go through Progressive, coarse first.
      - A frame's leftover bytes (16 KB per frame) refine the Progressive tiles that did not change. Frames keep coming while refinement is pending, and stop once everything is at full quality.
      - Tiles that switch to ClearCodec, and areas moved by a scroll, drop their Progressive refinement, so no upgrade paints over them.
  - Open:
    - AVC444, ClearCodec and Progressive refinement against mstsc and Windows App.
    - Video regions through H.264 on the same surface as ClearCodec and Progressive (only whole-surface AVC so far).
    - The exit benchmarks: bitrate, CPU and latency per codec on a content corpus, and text at 2 Mbit/s.
    - Rate control does not yet account for the two pictures per AVC444 frame.
    - Zero-copy covers AVC420 only; AVC444 would need the 4:4:4 split on the GPU.
  - Tested: FreeRDP 3 answers every auto-detect request. mstsc and Windows App are not tested with auto-detect yet, and no tier change has been seen on a real slow link (only in simulated traces).

### M6: Channels (~7 weeks)
- **disp (MS-RDPEDISP):** dynamic resize and **multi-monitor**. Virtual monitors come from Mutter `RecordVirtual`, the KWin virtual output or the portal `VIRTUAL` source; otherwise letterboxing.
- **cliprdr (MS-RDPECLIP):**
  - Text, HTML and images through the portal **Clipboard** interface, or a Wayland data device in headless sessions.
  - File copy (FileContents), exposed through a FUSE or temp-dir staging area.
- **rdpsnd (MS-RDPEA):** PipeWire monitor capture; PCM, then Opus/AAC formats where the client supports them. Wave-confirm flow control.
- **audin (MS-RDPEAI):** a PipeWire virtual source.
- **rdpei (MS-RDPEI):** touch and pen, delivered as EIS touch events.
- Optional: **rdpecam** as a PipeWire virtual camera; **ainput**.
- **Exit:** clipboard works in both directions for text, images and files; audio stays in sync (under 100 ms) and the microphone works from Windows App and FreeRDP.

### M7: Sessions, headless, deployment (~7 weeks)
- **System daemon (`farlandd`)**, plus a D-Bus control API, polkit, `farlandctl`, and a TOML config.
- **Headless sessions** per user:
  - Launchers for `mutter --headless`, `kwin_wayland --virtual`, sway/labwc headless, and cage (kiosk).
  - The keymap is taken from CS_CORE `keyboardLayout`.
- **Authentication modes:** per-server credential, Kerberos (the SPNEGO/GSSAPI acceptor with a keytab, moved here from M2), and delegated login with PAM (PLAN §3.5).
- **Session broker:** multiple concurrent sessions, reconnect to an existing session through the **auto-reconnect cookie**, idle and disconnect policies.
- **Hand-over** from the system daemon to a user session via Server Redirection + **RDSTLS**.
- **Operations:**
  - systemd units, journald structured logging, Prometheus metrics (fps, bitrate, RTT, queue depth).
  - Packaging as deb, rpm and an AUR recipe; a container image for the test backend.
- **Exit:** several users log into separate headless GNOME or Plasma sessions through one port, and disconnecting and reconnecting resumes each session.

### M8: Hardening and server 1.0 (~4 weeks, plus fuzzing throughout)
- Submit to OSS-Fuzz; run a 72-hour fuzzing campaign with no open crashes; get an **external security review** of pre-auth, NLA and client→server parsers.
- Tune performance: frame pacing, encoder latency, memory caps per session.
- Documentation: admin guide, per-desktop setup guide (GNOME, KDE, sway/Hyprland), troubleshooting with mstsc error codes.
- **Exit criteria for 1.0:**
  - The full client matrix is green: mstsc, Windows App on macOS/iOS/Android, FreeRDP 2/3, Remmina, GNOME Connections, KRDC, ZeroVDI web.
  - No known high-severity issues.

---

## Phase 2: native Wayland client (~9 months)

| Milestone | Content |
|---|---|
| **C1: Client core** | The client role of the sans-IO state machines. CredSSP initiator with NTLM (port ZeroVDI `NtlmClient`, plus channel bindings and flag intersection) and **Kerberos/SPNEGO via GSSAPI**. TLS verification with a known_hosts/TOFU store (the ZeroVDI `HostCertificatePolicy` model). Correct GCC/Client Info (time zone, ARC cookie), licensing, server redirection (port ZeroVDI's parser and tests), auto-reconnect |
| **C2: Decoders and presentation** | Decoders: planar, interleaved RLE, ClearCodec and Progressive (port ZeroVDI's C#/JS, the FreeRDP-faithful versions), ZGFX/MPPC/NCRUSH/XCRUSH, AVC420/444 through **VA-API** (FFmpeg fallback) with GPU YUV444 composition. Wayland presentation via **linux-dmabuf-v1** + Vulkan or EGL, `wp_fractional_scale_v1` + viewporter, `wp_presentation` timing. The decoders get fuzzed from day one; this is the area where FreeRDP's CVEs cluster |
| **C3: Wayland-native input and UX** | xkb keymap → scancodes, `keyboard-shortcuts-inhibit`, `relative-pointer` + `pointer-constraints`, `text-input-v3` (IME → Unicode events), `xdg-decoration`, `color-management-v1`, tablet and touch → rdpei, multimon (fullscreen per output + disp layout), a GTK4 or Qt connection manager |
| **C4: Channels** | cliprdr (text/HTML/images/files via `wl_data_device` + primary selection), rdpsnd/audin (PipeWire), rdpdr drive and smartcard (PC/SC), rdpecam, USB (urbdrc, stretch goal) |
| **C5: Enterprise** | RD Gateway (RDG over HTTP + WebSocket only, no RPC-over-HTTP), **Entra ID/RDSAAD** login, RDSTLS, Restricted Admin / Remote Credential Guard, **RemoteApp (RAIL) with each window as its own `xdg_toplevel`**, `.rdp` file import, and a `libfarland` C API for Remmina/GNOME Connections/KRDC plugins |

**Client 1.0 exit:** daily-drivable against Windows 11/Server 2025 and farland servers, with feature parity with `sdl-freerdp` on the commonly used channels, and better Wayland integration.

---

## Phase 3: beyond parity
- **U1: UDP transport:** MS-RDPEUDP (reliable and lossy) and MS-RDPEMT multitransport, for both roles. Neither FreeRDP version has this.
- **AV1** GFX codec (encode with VA-API/NVENC, decode with dav1d).
- Server-side **RemoteApp** (RAIL) for Wayland applications in headless sessions.
- Server-side rdpdr: client drives mounted with FUSE.
- HDR and wide-gamut paths, where the protocol allows them.

---

## Not in scope (deliberately)
- Standard RDP security (RC4) and FIPS mode.
- Legacy GDI drawing orders (primary/secondary/alternate) and glyph/bitmap caches in the client.
- RemoteFX classic as a server codec (NSCodec decode only if a client-side need appears).
- TSMF.
- RPC-over-HTTP gateway (TSG).
- X11 capture backends. Use Xwayland-in-headless-compositor or the existing tools instead.
- Win32 emulation of any kind.
