# farland: roadmap

The phases follow the decisions in [PLAN.md](PLAN.md): the server first, in C++, on our own protocol core. Durations are rough estimates for 1–2 full-time engineers and assume the macRDP and ZeroVDI code can be ported. Every milestone ends with a tagged pre-release and a passing interop matrix.

```
Phase 1: server 1.0                                         Phase 2: client          Phase 3
M0 ─ M1 ─ M2 ─ M3 ─ M4 ─ M5 ─ M6 ─ M7 ─ M8 ─► server 1.0     C1 ─ C2 ─ C3 ─ C4 ─ C5 ─► 1.0   U1 UDP, AV1, RAIL server
~2   ~5   ~4   ~7   ~6   ~7   ~7   ~7   ~4  weeks (≈ 12 months)       (≈ 9 months)
```

**Status (2026-09-19):** M0 to M3 are done; each has a status note below that lists what differs from the plan and what is still untested. M4 is implemented and works on GNOME and Plasma; the latency target is still to be measured. M5 and M6 are implemented; what is left of both needs mstsc, Windows App or hardware the test machines do not have. M7's multi-session daemon works (S0, S1).

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
- **Status: done, with one difference.** GitHub CI is deliberately one job (a release build, one test run and one run of the release binary), to keep runner minutes down. The other build variants run locally and on the test machines: Apple clang and Homebrew LLVM on macOS (ASan+UBSan and TSan), GCC 13 to 16 and Clang 19 to 21 on Linux, clang-tidy, the fuzz targets and the compositor tests.

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
    - `--share` serves one session at a time. It shared one monitor and did not follow a resized desktop; since M6 it shares every monitor picked in the portal dialog and follows the client's layout (see M6).
    - The session read each frame into CPU memory; since M5, AVC420 on VA-API takes the captured dmabufs directly.
    - Unicode input is not typed through libei, which has no text input.
    - A virtual monitor was always 1920x1080; since M6 it takes the client's size on GNOME (KWin's portal keeps 1920x1080).
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
- **Status: implemented except the items under Open.**
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
    - Mixed mode on Progressive surfaces, with the codec chosen per tile from how that tile behaves over time (`--no-clearcodec`, `--no-refine`, `--no-video-regions`, `--no-lossless-still` switch the parts off):
      - Every changed 64x64 tile is classified by counting its colours, stopping at the 49th, and by a motion counter (+2 for a frame that changed it, -1 for one that did not, capped at 12).
      - Tiles with few colours (text, UI) go through ClearCodec, one region per run of adjacent tiles in a row.
      - Tiles from a motion count of 8 (four frames of change in a row) that hold too many colours for ClearCodec are moving picture and go through H.264 on the same surface: one AVC420 picture of the whole surface whose regionRects list only those tiles, so ClearCodec and Progressive pixels elsewhere are never painted over, an IDR included. The encoder is made when the first video region appears and dropped again after 90 frames without one; tiles that stop moving are marked changed at once and come back sharp through Progressive or ClearCodec. The Progressive refinement of those tiles is discarded, so no upgrade paints over the H.264 pixels.
      - Damage of at most 12 tiles skips the coarse first pass: those tiles go out at full Progressive quality in one TILE_FIRST (`Encoder::Pass::direct`, bit-identical to a single-pass tile) and owe no upgrade. A caret, a spinner or a clock repaints the same few tiles over and over, and the coarse-first ladder never caught up with it, so exactly what the eye rests on stayed at the quality of a first pass. Tiles that are already hot keep the ladder, which costs less per frame.
      - The rest goes through Progressive, coarse first. A frame's leftover bytes (16 KB per frame) refine the Progressive tiles that did not change. Frames keep coming while refinement is pending.
      - Lossless convergence: once a Progressive tile is at full quality and has stood still for three frames, it goes out once more as lossless planar (48 KB per frame, round-robin over the surface), so a desktop that stands still ends up pixel-exact instead of keeping the quantization noise of its tier. ClearCodec tiles are exact already and are skipped. Frames keep coming until every still tile is exact, and stop there.
      - Tiles that switch to ClearCodec, to H.264 or to the lossless pass, and areas moved by a scroll, drop their Progressive refinement, so no upgrade paints over them.
    - Rate control accounts for the two pictures per AVC444 frame: `QualityController::Config::pictures_per_frame` halves the encoder's VBV cap, since both views go through one encoder at the session's nominal frame rate and would otherwise produce twice the tier's budget.
    - The exit benchmark (`benchmarks/bench_codecs.cpp`, `meson compile -C build bench-codecs`): bitrate, CPU, latency and PSNR per codec over a generated content corpus (a dense page of text from a 16-glyph alphabet, a UI, a photo, a mixed desktop, and a video sequence), and the "text at 2 Mbit/s" check. The corpus is generated, so the numbers are reproducible without sample files. Measured on Kubuntu 26.04, GCC 15, one core of a 4-core VM, 1920x1080, release build, for a whole frame repainted at once:

      | corpus | codec | KB/frame | kbit/s at 30 fps | CPU ms | latency | PSNR | over glyphs |
      | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
      | text | ClearCodec | 322.5 | 79251 | 36.1 | 36 ms | exact | exact |
      | text | Progressive `quant_default` | 1013.3 | 249018 | 35.0 | 35 ms | 40.1 | 35.3 |
      | text | Progressive refined | 1055.6 | 259432 | 365.2 | 1567 ms | 40.1 | 35.3 |
      | ui | ClearCodec | 40.5 | 9942 | 11.4 | 11 ms | exact | exact |
      | ui | Progressive `quant_default` | 124.9 | 30700 | 20.6 | 21 ms | 49.8 | 37.3 |
      | ui | Progressive refined | 156.0 | 38328 | 142.7 | 200 ms | 49.8 | 37.3 |
      | photo | planar (lossless) | 6076.0 | 1493237 | 44.0 | 44 ms | exact | - |
      | photo | Progressive `quant_default` | 337.9 | 83045 | 35.1 | 35 ms | 39.4 | - |
      | photo | Progressive `quant_highest` | 1559.4 | 383230 | 78.3 | 78 ms | 44.6 | - |
      | photo | Progressive refined | 384.0 | 94381 | 263.2 | 667 ms | 39.4 | - |
      | photo (30 frames of motion) | AVC420, OpenH264, 6 Mbit/s cap | 19.0 | 4663 | 13.0 | 21 ms | - | - |
      | photo (30 frames of motion) | AVC420, OpenH264, 2 Mbit/s cap | 8.2 | 2013 | 9.9 | 20 ms | - | - |
      | mixed desktop | ClearCodec | 761.3 | 187086 | 28.9 | 29 ms | exact | exact |
      | mixed desktop | Progressive `quant_default` | 160.1 | 39337 | 22.0 | 22 ms | 46.2 | 37.3 |
      | mixed desktop (30 frames of motion) | AVC420, 6 Mbit/s cap | 3.9 | 968 | 7.5 | 17 ms | - | - |

      Reading it: a whole frame repainted 30 times a second is the worst case and nothing but H.264 fits a link; what a desktop actually sends is a few changed tiles per frame. The refined Progressive row shows what the coarse-first ladder costs in time: a full-frame repaint of a photo needs 21 frames (667 ms) of upgrades before the picture is complete, which is exactly why small damage now skips the ladder. ClearCodec is pixel-exact on text and UI at a quarter of the bytes of Progressive, and Progressive is an order of magnitude smaller than either lossless codec on a photo.
    - **Text at 2 Mbit/s: met.** Typing into a full page of text (three 64x64 tiles repainted per frame) costs 1978 bytes per frame, 475 kbit/s at 30 fps, and is pixel-exact: ClearCodec, which is where mixed mode sends text tiles. Repainting the whole page at once costs 322 KB, so a full-page repaint takes about 1.3 s of a 2 Mbit/s link; it stays exact and happens once, not per frame. Progressive on the same page reaches 35.3 dB over the glyphs, which is what ClearCodec exists to avoid.
  - Open:
    - AVC444, ClearCodec, Progressive refinement, the H.264 video regions and the lossless pass against mstsc and Windows App.
    - Zero-copy covers AVC420 only; AVC444 would need the 4:4:4 split on the GPU, and the mixed-mode video regions read pixels because the classifier needs them.
  - Tested:
    - FreeRDP 3 answers every auto-detect request. mstsc and Windows App are not tested with auto-detect yet, and no tier change has been seen on a real slow link (only in simulated traces).
    - Live on Kubuntu 26.04 (2026-09-19), `--headless plasma` at 1280x800 with a 960x540 video (`ffplay -f lavfi testsrc2`) playing in it, one FreeRDP 3.31 client, the same picture for each run:

      | pipeline | while the video runs | desktop at rest |
      | --- | ---: | ---: |
      | mixed mode (the default) | 3.5 Mbit/s, peak 4.3 | 1 kbit/s |
      | `--no-video-regions` | 4.4 Mbit/s, peak 4.8 | 1 kbit/s |
      | `--no-video-regions --no-lossless-still --no-refine` | 14–16 Mbit/s | 1 kbit/s |

      H.264 takes the video tiles and saves about a fifth of the bytes against Progressive alone, while the clock in the corner of the picture stays sharp through ClearCodec, and the checkerboard in it shows the 4:2:0 chroma of the H.264 region and nothing else: the codecs composite on one surface without painting over each other. At rest the pipeline goes quiet at 1 kbit/s once the lossless pass has made the picture exact, which is what it is for.
    - Live on Ubuntu 26.04 (GNOME 50) with `--headless gnome` and the same video: the session comes up, the picture renders and the frame rate holds 30 fps.
    - **Ubuntu's and Kubuntu's `freerdp3` package is built with `WITH_GFX_H264=OFF`**, so it advertises RDPGFX_CAPS_FLAG_AVC_DISABLED and no AVC path can be exercised against it. The live AVC runs above used a FreeRDP 3.31 built from source with `-DWITH_GFX_H264=ON -DWITH_OPENH264=ON`, started with `/gfx:AVC444` (plain `/gfx` leaves AVC off).

### M6: Channels (~7 weeks)
- **disp (MS-RDPEDISP):** dynamic resize and **multi-monitor**. Virtual monitors come from Mutter `RecordVirtual`, the KWin virtual output or the portal `VIRTUAL` source; otherwise letterboxing.
- **cliprdr (MS-RDPECLIP):**
  - Text, HTML and images through the portal **Clipboard** interface, or a Wayland data device in headless sessions.
  - File copy (FileContents), exposed through a FUSE or temp-dir staging area.
- **rdpsnd (MS-RDPEA):** PipeWire monitor capture; PCM, then Opus/AAC formats where the client supports them. Wave-confirm flow control.
  - **Status: implemented, tried with FreeRDP only.**
    - Sans-IO codec and server (`channels::rdpsnd`, `RdpsndServer`): formats, quality mode, training, Wave2 for version 8 clients and WaveInfo/Wave for older ones, wave confirm, close, volume. UDP (Crypt Key, Wave Encrypt, UDP Wave) is not implemented.
    - Transport: AUDIO_PLAYBACK_DVC first, as Windows servers and gnome-remote-desktop do, and the "rdpsnd" static channel when the client refuses it or has no drdynvc. AUDIO_PLAYBACK_LOSSY_DVC needs UDP (phase 3).
    - Formats: Opus 48 kHz stereo (libopus loaded at runtime, BSD) for clients asking for dynamic or medium quality, PCM 48/44.1/22.05 kHz stereo otherwise. FreeRDP decodes Opus when built with it; Windows clients do not take Opus. AAC (0xA106), which Windows clients decode, would need an AAC encoder: fdk-aac's licence is not Apache-compatible and FFmpeg's is LGPL, so it is left for an optional runtime-loaded backend.
    - Capture: the default sink's monitor (`stream.capture.sink`) in the user's PipeWire, only while a client plays; PipeWire resamples.
    - Flow control: 20 ms packets; audio unconfirmed beyond the lowest backlog of the last seconds plus 100 ms is dropped, not queued (400 ms before the first confirmation, 1 s at most). 1 s of digital silence stops the stream with a Close PDU. In dynamic quality the Opus bitrate follows the auto-detected bandwidth.
- **audin (MS-RDPEAI):** a PipeWire virtual source.
  - **Status: implemented, tried with FreeRDP only.** AUDIO_INPUT with 16-bit PCM (48/44.1 kHz mono preferred), opened when the client sets INFO_AUDIOCAPTURE. The samples go to a virtual source (`farland-microphone`, a stream node of media.class Audio/Source: with Audio/Source/Virtual, WirePlumber stalls Pulse streams opened after it) with a 40 ms jitter buffer, which exists while the client records.
- **rdpei (MS-RDPEI):** touch and pen, delivered as EIS touch events.
- Optional: **rdpecam** as a PipeWire virtual camera; **ainput**. rdpecam is done (below); ainput is not.
- **Exit:** clipboard works in both directions for text, images and files; audio stays in sync (under 100 ms) and the microphone works from Windows App and FreeRDP.
- **Status: all four channels implemented and tried with FreeRDP, and rdpecam with them; the exit needs mstsc and Windows App, the portal clipboard on a live desktop, a real touch device and a real camera.**
- **Status of rdpecam: implemented; not yet tried with a real camera.**
  - Done:
    - The codec ([MS-RDPECAM] 2.2, `channels/rdpecam`): every PDU of both the device enumeration channel and a device channel, with the two-byte shared header, decoded strictly. List PDUs (stream descriptions, media types, properties) take as many entries as fill the message, and a partial entry is an error; a media type with a zero frame rate or aspect ratio is refused, as FreeRDP's client refuses it; device and channel names must be terminated and are bounded; a sample is capped at 64 MB. Fuzz target `rdpecam`, which also drives the media type choice.
    - `server::CameraServer`: the enumeration channel, the version handshake, one device channel, and the sample loop of 3.1.5.6 — one Sample Request outstanding at a time, the next asked for only once the frame has been consumed, so a slow consumer slows the client down instead of queueing frames. A camera offered while one is in use waits and is taken up when that one goes; a frame whose size is not what the media type calls for gives the camera up with a clear message rather than handing a short buffer to a consumer.
    - Media types: the largest uncompressed picture within `CameraOptions` (1920x1080 by default), and among equal sizes the frame rate closest to 30 and the cheapest layout (I420, NV12, YUY2, RGB24, RGB32). The description goes back to the client exactly as it arrived, so its flags are ones the client's own validation accepts. **H.264 and MJPG are skipped**: farland has no decoder for either, and needs none, because the client converts — FreeRDP's V4L client offers uncompressed formats for a camera whose hardware produces MJPG.
    - `platform::VideoSink` and a PipeWire `Video/Source` node (`platform/portal/pipewire_camera.cpp`, sharing the stream plumbing with the virtual microphone): applications that use PipeWire list "farland-camera" while the client streams. A `/dev/video` node would need v4l2loopback and is out of farland's reach.
    - `--no-camera`, `[camera] enabled` in the configuration file, and the setting carried to per-user sessions through the broker.
  - Differences from the plan:
    - The direction of the version handshake is the opposite of what the message names suggest: the **client** sends CAM_SELECT_VERSION_REQUEST when it opens the enumeration channel, and the server answers with CAM_SELECT_VERSION_RESPONSE. FreeRDP's client logs "unknown MessageId=0x03" at a server that sends the request itself.
    - One camera at a time, and no properties (zoom, focus, brightness): the PDUs decode, but nothing asks for them.
  - Tested:
    - Unit tests for the codec against the bytes FreeRDP's client reads and writes, and an in-process end-to-end test: a scripted client opens the channels through drdynvc, offers a camera, negotiates a media type and answers sample requests, with frames split across DYNVC_DATA_FIRST and DYNVC_DATA as a real client must (a 640x480 NV12 frame is 450 KB, far past a drdynvc PDU).
    - Live on Kubuntu 26.04 against FreeRDP 3.31 built with its rdpecam V4L client (`/dvc:rdpecam`): the enumeration channel opens and the version handshake settles on version 2, with no complaint from the client.
  - Not tested yet: a real camera end to end. Both test machines have Secure Boot on, so the unsigned `v4l2loopback` module cannot be loaded to stand in for a webcam, and neither has one. What is untested is therefore the device channel against a real client: the stream and media type lists, Start Streams and the sample loop.
- **`ainput` is not implemented.** It is FreeRDP's own channel and adds nothing the RDP input paths do not already carry.
- **Status of disp: implemented.**
  - Done:
    - The [MS-RDPEDISP] codec (`channels/disp`): the capabilities and monitor layout PDUs with every field (orientation, desktop and device scale, physical size), decoded strictly (the header length, MonitorLayoutSize 40, 1 to MaxNumMonitors monitors that fill the PDU), with a fuzz target that also runs the server's validation.
    - `server::DisplayControl` on "Microsoft::Windows::RDS::DisplayControl": sends the capabilities (8 monitors, 8 x 3840 x 2160 pixels in all), validates layouts (widths even, 200 to 8192, no overlaps, one primary, a bounding box of at most 16384) and ignores invalid ones, and debounces a dragged window edge: a layout applies 300 ms after the last one, or 1 s after the first of a burst.
    - `server::DisplayLayout`: the client's monitors from CS_MONITOR/CS_MONITOR_EX at connect time or from Display Control, normalised to a desktop starting at 0,0, and where each screen shows: its own size, or scaled down and centred (letterboxed) on its monitor.
    - The Graphics Pipeline lays out one surface per screen with its own encoders (per-monitor surfaces map cleanly to separately captured streams, and keep each H.264 picture within what hardware encoders take), mapped where the screen shows: pictures larger than their monitor go through MapSurfaceToScaledOutput where the client allows it, or are scaled on the CPU (a box filter) otherwise. Borders and monitors without a picture are black surfaces of their own, since ResetGraphics leaves the rest of the output undefined. A new layout sends ResetGraphics with the monitor list and new surfaces; one RDPGFX frame carries every screen.
    - Clients without GFX get one composed picture and are reactivated (Deactivate All, Demand Active) at the new desktop size.
    - The test pattern shows on every monitor, and takes pointer input on the one under the pointer.
    - The portal desktop shares every monitor picked in the dialog (`multiple`), each captured separately, ordered left to right, each on one client monitor; absolute pointer motion goes to the right stream through libei's regions (`EiInput::set_outputs` with the letterboxed places) or the Notify* layout. The cursor comes from the screen that shows it, mapped onto its place.
    - `--virtual-monitor` sizes the virtual monitor to the client's monitor and resizes it with the client's window: the capture renegotiates its PipeWire format with the requested size first (as gnome-remote-desktop does), which Mutter follows; a producer of another fixed size (KWin's 1920x1080 virtual output) still matches the size range offered after it.
    - A shared desktop with fixed monitors shows them side by side at their size to single-monitor clients until they send a layout (as before M6 for one monitor).
  - Differences from the plan:
    - A portal session has one virtual monitor (xdg-desktop-portal-gnome and -kde both), so with `--virtual-monitor` only the client's primary monitor gets a picture. Several virtual monitors need Mutter's own `RecordVirtual` (allowed several times per session and not restricted to trusted callers in Mutter 50), which comes with the Mutter backend (M7).
    - KWin's virtual output stays 1920x1080: xdg-desktop-portal-kde creates it at that size and KWin does not follow the consumer's format.
    - The Monitor Layout PDU ([MS-RDPBCGR] 2.2.12.1) is not sent to clients without GFX; they keep the layout they asked for.
  - Tested:
    - Unit tests for the codec (a FreeRDP-style two-monitor layout, strict decoding), the layout rules and letterboxing, the debounce, the scaler and compositor, and the multi-surface pipeline (surfaces, scaled maps, black borders, one frame for all screens, a new layout); the fuzz corpus. GCC 15 with `-Werror` on Ubuntu 26.04 and Kubuntu 26.04, Apple clang with ASan and UBSan.
    - Live on Ubuntu 26.04 with FreeRDP 3.31 (`/dynamic-resolution`, in Xvfb, its window resized with xdotool) and the test pattern: every settled layout gives a ResetGraphics and a new surface (1024x768, 1280x720, then a burst of five sizes applied as two layouts), and the pattern renders at each size. Without GFX (`/bpp:16`) the connection is reactivated at the new size.
    - Live on Plasma 6.6 (xdg-desktop-portal-kde) with `--share` and FreeRDP 3: the 1280x800 monitor fills a 1280x800 window, is centred with black borders in a 1600x900 one, and scaled to 800x500 with borders above and below in an 800x800 one.
  - Not tested yet: mstsc and Windows App (resizing and "use all my monitors"); a client with several monitors (Xvfb gave FreeRDP no valid multi-monitor layout); sharing several monitors (the test machines have one each); `--virtual-monitor` resizing on GNOME (no desktop session was logged in on the GNOME host) and on Plasma (its portal dialog needs a person); the zero-copy AVC420 path with several screens.
- **Status of rdpei: implemented; not yet tried with a real touch client.**
  - Done:
    - The codec ([MS-RDPEI] 2.2): the five variable-length integers, SC_READY and CS_READY (versions 1.0 to 3.0, multipen flags), touch and pen events with every optional field, suspend, resume and dismiss-hovering. Decoding is strict: `pduLength`, counts and field presence must add up exactly, with at most 128 frames, 256 touch or 4 pen contacts per frame and 64 KiB per PDU. Fuzz target `rdpei`.
    - `RdpeiServer`: SC_READY 3.0, and the contact state machine of 3.1.1.1 for every touch contact and pen, with at most the client's `maxTouchContacts` (capped at 32). A contact that breaks it is canceled and the rest of its transaction ignored (3.2.5.3). Contacts are released on suspend, on a new CS_READY, when the channel closes and at the end of the session. Frame timestamps are not replayed.
    - An `InputSink` touch API (down, motion, up and cancel per slot, framed by `flush()`), implemented by `EiInput` with libei touches mapped through the same regions and outputs as the absolute pointer. Without a touchscreen device, the first finger drives the pointer.
    - Pen: libei (1.5) has no tablet devices, so the pen moves the pointer and holds the left button while it touches (the right one with the barrel button); pressure, tilt and rotation are dropped.
    - The test pattern shows touch and pen contacts.
  - Tested:
    - Unit tests with the spec's integer examples, and an in-process end-to-end test: a scripted client opens the channel through drdynvc, and its touches reach an `InputSink` through `TouchInput` and the translator.
    - `EiInput` against an in-process libeis server with a touchscreen.
    - Live on Plasma 6.6 (2026-09-14): `farland-touch-probe` touched the shared monitor through the portal's EIS connection, and a full-screen Qt client got the tap, a swipe, a two-finger spread and a cancel at the right positions. KWin's EIS supports touch and cancel; Mutter 50's reads touches but not cancels (a cancel becomes an up there), from the symbols it imports; GNOME was not tried live.
  - Not tested yet: a real touch or pen client (Windows App on a tablet, mstsc on a Surface), and pen input anywhere.
- **Status of cliprdr: implemented; the portal clipboard is not yet tried on a live desktop.**
  - Done:
    - The protocol ([MS-RDPECLIP]): all PDUs with strict decoding and limits, long and short format names, delayed rendering both ways, one outstanding request per direction with time-outs, file contents by size and range with stream IDs, Lock/Unlock with clip data IDs, huge files; a fuzz target with a seed corpus.
    - Formats: CF_UNICODETEXT and CF_TEXT ↔ text/plain (UTF-8, CRLF/LF), "HTML Format" ↔ text/html, CF_DIB/CF_DIBV5/"PNG" ↔ image/png and image/bmp (an own PNG codec on zlib, optional), FileGroupDescriptorW ↔ text/uri-list and x-special/gnome-copied-files.
    - Files: the client's are staged when the desktop pastes them, in a 0700 directory under `$XDG_RUNTIME_DIR/farland/` with names checked against escapes and a size cap (1 GiB per paste); the desktop's are served from the paths it names, re-checked (device, inode, no symlinks) on every read. No FUSE: a large paste waits until every file is fetched.
    - Desktop: org.freedesktop.portal.Clipboard (RequestClipboard before Start, SetSelection, SelectionRead/Write, the owner and transfer signals), tested against a mock portal.
    - The test pattern has a loopback clipboard: what the client copies is fetched at once and offered back.
  - Tested:
    - FreeRDP 3.31 (xfreerdp3) against the loopback, with text, HTML, an image (CF_DIB to PNG and BMP and back, pixel-exact) and files (a file and a folder with 3 MB, staged on the server and read back through FreeRDP's FUSE mount), on Ubuntu 26.04. The portal path is tested against the mock portal; against GNOME 50 it reaches Start with RequestClipboard, but the permission dialog was not answered, so the real desktop clipboard and mstsc/Windows App are still untested.
  - Not tested yet: the portal clipboard on a live desktop (the portal's permission dialog was not accepted in the test), mstsc and Windows App.

### M7: Sessions, headless, deployment (~7 weeks)
Multi-session with headless desktops behind one port (decided 2026-09-14).
- **System daemon (`farlandd`)**, root, one system unit:
  - Owns the port, reads `/etc/farland/farland.toml` (`[server]`, `[auth]`, `[session]`, `[policy]`, `[graphics]`, `[network]`, `[audio]`, `[clipboard]`) and the credential store, runs PAM, and keeps the session registry.
  - Spawns the network processes as farland-server does: X.224, TLS and NLA stay sandboxed behind privsep.
  - A D-Bus control API (`org.farland.Farland1`) with polkit, used by `farlandctl sessions|terminate|passwd`.
- **Hand-over by descriptor passing:**
  - The network process keeps TLS and relays the plaintext RDP stream. farlandd passes that socket over SCM_RIGHTS to the user's **`farland-agent`**, which runs the rest of the connection (broker protocol: `src/farland/server/broker.hpp`).
  - One TCP connection that works with every client; no TLS key or credential leaves root and the sandbox.
  - Server Redirection with **RDSTLS** stays an option for spreading sessions over several hosts later.
- **`farland-agent`**, one per session, a user unit inside the user's logind session: owns the desktop backend, runs the session loop for each connection, and keeps the desktop between connections. It gets no sandbox of its own before M8.
- **Headless sessions** per user:
  - **GNOME through GDM:** `RemoteDisplayFactory.CreateUserDisplay` creates a headless session logged in through gdm-autologin (GDM 50.0 has no preauthenticated user for `CreateRemoteDisplay`). The Mutter backend drives it without a portal dialog: RemoteDesktop, ScreenCast `RecordVirtual` and EIS input.
  - **Plasma, sway, labwc and cage through farland's own PAM and logind session** (`pam_systemd` marks it remote): `kwin_wayland --virtual`, wlroots compositors on the headless backend, and cage with one kiosk application.
  - The desktop starts at the client's size and monitors, and disp resizes it later. The keymap comes from CS_CORE `keyboardLayout`.
  - **Status of the GNOME backend: implemented, tried with FreeRDP on GNOME 50.1.** `start_gnome_headless()` (apps/farland-server/gnome_headless.hpp, `src/farland/platform/mutter/`) drives Mutter's RemoteDesktop and ScreenCast D-Bus API: one virtual monitor per client monitor (`RecordVirtual`, is-platform, sized through PipeWire format negotiation), added and removed as the client's monitors change (`Desktop::screens_follow_monitors()`), input through `ConnectToEIS`, `SetKeymap` from the layout (with libxkbcommon), and Mutter's clipboard for cliprdr. It attaches to the session it runs in or launches a bare `gnome-shell --headless --no-x11` on a private session bus (`farland-server --headless gnome`). A connection attaching to a session takes it over: the session's monitors become exactly the client's (ours primary) and the seat's own monitors go off, while the session stays active on its seat, which GNOME needs to draw it at all — attaching activates it through logind (`Session.Activate`, then `Seat.SwitchTo`), and a logind watch ends the connection when something else takes the seat. That logind side is shared with Plasma now (`src/farland/platform/logind/`): `SeatWatch`, `activate_user_session()` and `switch_seat_to_greeter()`, which tries GDM's `CreateTransientDisplay` and then the freedesktop display manager API SDDM and LightDM implement. Disconnecting puts the seat's monitors back. Detaching puts the seat's monitors back beside ours and lets ours go with the session: switching the seat on and ours off in one configuration makes Mutter 50.1 fall over its cursor plane (SIGSEGV after `maybe_update_cursor_plane: assertion 'crtc_state_impl' failed`), which took the user's session with it. Still to do: the GDM route through farlandd (S2), dmabuf capture on a GPU host, and placing the virtual monitors as the client arranges its monitors (Mutter puts new ones to the right).
- **Authentication:**
  - Default: **self-enrolment.** `farlandctl passwd` asks farlandd over D-Bus. Polkit (`auth_self`) and a PAM password check confirm the user, and farlandd stores the NT hash with the local account (`user:domain:NT hash[:local account]`).
  - Delegated login with PAM (PLAN §3.5) is dropped: NTLM needs the NT hash before a password could ever reach PAM.
  - Kerberos: the SPNEGO/GSSAPI acceptor with a keytab (moved here from M2), with the principal mapped to a local user.
- **Session broker:**
  - The NLA identity picks the user's one session (`max_sessions_per_user = 1`). A second connection takes over and ends the first with ERRINFO_DISCONNECTED_BY_OTHERCONNECTION, after `[policy] takeover` asked whoever is using it.
  - The agent sends a fresh **auto-reconnect cookie** (Save Session Info) on every connection and checks a returning one; NLA stays authoritative.
  - A user already logged in on a local seat: `on_local_session = separate | attach | replace | refuse` (default separate). `attach` takes that session over (it stays active on its seat, whose monitors go off for the connection); `separate` leaves it alone and starts a second, bare headless shell for the same user (GDM refuses a second session, so no gnome-session); `replace` ends it through logind and starts a headless one through GDM.
  - **Takeover consent** (`takeover = ask | always | never`, default ask): farlandd holds the new connection and sends the session's agent a ConsentRequest; the agent asks over `org.freedesktop.Notifications` ("Hand over your session?", the connecting user, the client's address and name, and a countdown in the default button) and answers. `takeover_timeout` (30 s) and `takeover_on_timeout` (allow) decide when nobody answers or no notification service does. A session no client holds is resumed without asking.
  - Policies: `disconnected_timeout`, `idle_timeout`, `max_sessions`. Logging out ends the compositor, then the agent, then the GDM display or PAM session.
- **Operations:**
  - systemd units, the PAM file, D-Bus and polkit policies, and the KWin desktop file.
  - journald structured logging, Prometheus metrics (fps, bitrate, RTT, queue depth).
  - Packaging as deb, rpm and an AUR recipe; a container image for the test backend.
- **Phases:**
  - S0: the broker protocol with descriptor passing, Save Session Info and the ARC verifier, the TOML config, and the local-account column.
  - S1 (done): farlandd and the agent through farland's own PAM, with the test pattern. Two users; reconnecting resumes. Tested with `--no-pam` in the unit tests and as root on the GNOME test host (not in a CI container with `pam_permit`).
  - S2: GNOME through GDM (`CreateUserDisplay`; farlandd's side is done) with the Mutter backend and disp resize.
  - S3: Plasma. The backend is in (`start_plasma_headless`, `farland-server --headless plasma [--attach]`, tried on Plasma 6.6 with FreeRDP): KWin's virtual backend with `plasma_session` on a private D-Bus bus (Plasma's systemd boot shares the user's systemd instance with a local session, and the user's bus has its unique names taken), `zkde_screencast_unstable_v1` capture with a desktop file for `X-KDE-Wayland-Interfaces`, resizing through custom modes of `kde_output_management_v2`, EIS input, an ext-data-control clipboard. **Taking a local session over is done and tried on KWin 6.6.6** (docs/PLASMA-TAKEOVER.md): attached, the client gets a virtual output per client monitor (`stream_virtual_output_with_description`, version 4 of the protocol; KWin prefixes its own `Virtual-` to the name) rather than the seat's screen mirrored, added and removed as the client's monitors change, laid out with the client's first screen primary so that the panel and new windows are there (`OutputManagement::request_layout()`, put back on the way out), and the seat gets the display manager's login screen. KWin goes on drawing a virtual output at its full frame rate while the session is not active on its seat — measured, not assumed, and the opposite of what a window left on the seat's own screen suggests — so nothing has to keep it alive; the seat's own screen stays switched on, because KWin refuses every output configuration off the seat and remembers the ones it applies. What it does need is `packaging/kwin` (`make-kwin-deb.sh`, one commit on 6.6.6): KWin will not *create* a virtual output off the seat, because `DrmBackend::applyOutputChanges()` tests the seat's pipelines first and throws the whole configuration away, so a client that let go cannot take the session again until somebody logs in at the machine. farlandd starting a Plasma session per user is open.
  - S4: wlroots and cage, with headless sway in CI.
  - S5: policies, the D-Bus API and polkit, `farlandctl` commands, metrics, units and packaging; then Kerberos.
  - S6 (optional): Server Redirection and RDSTLS for several hosts.
- **Status of the wlroots backend (S4): implemented for one user; the multi-session launch (PAM, logind, the agent) is S1's.**
  - Done:
    - `start_wlroots_headless()` (`apps/farland-server/wlroots_headless.hpp`) and `farland-server --headless sway|labwc|cage`: the compositor starts on the headless backend (`WLR_BACKENDS=headless`, `WLR_LIBINPUT_NO_DEVICES=1`, pixman unless a render node is given) in its own process group, with its Wayland socket found through `/proc`, and stops with the desktop; `--headless-attach` uses the session's compositor instead.
    - A Wayland client layer (`platform/wlroots/wayland/`: connection and dispatch in farland's poll loop, shared-memory buffers, the data-control clipboard) and protocol glue from wayland-scanner, taken from the installed wayland-protocols where it has them, with MIT-licensed copies of the wlroots protocols and stand-ins for older releases.
    - Capture: ext-image-copy-capture-v1 on an output source, damage-driven with up to three buffers and damage hints for each, and the cursor from a pointer cursor session (wlroots' headless outputs take "hardware" cursors, so the frames come without it); wlr-screencopy with the cursor drawn in where the compositor lacks it (cage 0.2).
    - Input: zwp_virtual_keyboard_v1 with a keymap compiled with xkbcommon from the client's layout, modifier state kept by farland, and Unicode typed through the layout's key and modifiers or a spare key the keymap gets on demand (keycodes up to 255 first, for Xwayland); zwlr_virtual_pointer_v1 with absolute motion mapped through the letterboxed screen places onto the compositor's layout, buttons and wheel notches. No virtual touch protocol exists, so touch drives the pointer.
    - Clipboard through ext-data-control-v1, or zwlr-data-control-unstable-v1.
    - Sizes through wlr-output-management custom modes; the desktop is resizable and follows display control.
  - Differences from the plan:
    - One screen per desktop: `HeadlessOptions` names one monitor size, and a desktop's screen count must not change while it is open, so further client monitors stay black. `WLR_HEADLESS_OUTPUTS` starts more outputs (the launcher supports it) once the options carry the client's monitors.
    - No dmabuf capture yet: frames come in shared memory, so AVC420 reads them from CPU memory.
  - Tested: unit tests for the keymap (layouts, modifiers, spare keys), pointer mapping, damage, cursor pixels and the launch setup; compositor tests against headless sway 1.11, labwc 0.9.3 and cage 0.2.1 (frames, resizing, cursor positions, key bindings with modifiers, a typed €, a clipboard round trip). Live on Ubuntu 26.04 with FreeRDP 3.31: sway's terminal opened and typed into, resized to the client's window, the clipboard in both directions; labwc's menu at the pointer; cage running foot.
- **Exit:** several users log into separate headless GNOME or Plasma sessions through one port, and disconnecting and reconnecting resumes each session.
- **Status (2026-09-17): S0 and S1 done; the GNOME route of S2 and self-enrolment from S5 work; the Debian package from S5 builds in CI (`packaging/make-deb.sh`) and installs the service.**
  - Done:
    - `farlandd` (`apps/farlandd/`):
      - The port, the TLS identity and the credential store.
      - Per client, the sandboxed network process of farland-server (TLS and NLA). The NLA identity is mapped to a local account (the store's account column, else the user name).
      - `[policy]`: one session per account, `max_sessions`, `on_local_session` (logind: a graphical session on a seat).
      - Takeover by a second connection (ERRINFO_DISCONNECTED_BY_OTHERCONNECTION), `idle_timeout` and `disconnected_timeout`, in a registry without I/O (`registry.hpp`, tested with a fake clock).
      - `[policy] takeover`: the consent prompt. farlandd holds the connection while the session's agent asks the person using it (broker ConsentRequest, ConsentCancel and ConsentReply; `farlandd/consent.hpp` and `farland-agent/prompt.hpp`, the bookkeeping without I/O or a clock of its own). A held session is one with a client on it, or an attached local session. Cancelling refuses the new connection with ERRINFO_SERVER_DENIED_CONNECTION; a holder who disconnects meanwhile takes the question down and the takeover goes ahead.
      - Refused clients get their Set Error Info from a sandboxed process that runs the connection until it can be sent.
      - `farlandd --no-pam` runs every session as the calling user, for development and CI.
    - Session launch (`launcher.hpp`, `logind.hpp`):
      - Plasma, sway, labwc, cage and the test pattern: farlandd starts itself as a session helper. The helper runs PAM (`farland`: account, setcred, open_session with type wayland, class user, the desktop and PAM_RHOST, so pam_systemd registers a remote session), runs the agent as the user, and closes the session after it.
      - GNOME: GDM's `CreateUserDisplay` (what `gnome-headless-session@.service` does), then the agent as a transient unit in the user's service manager (sd-bus to `user@.host`) with the token in its environment, attached to that session's Mutter. `DestroyUserDisplay` ends it.
      - `on_local_session = "attach"` starts the agent the same way in the user's local session.
    - `farland-agent` (`apps/farland-agent/`):
      - Greets farlandd with its token; farlandd checks the token and the peer's uid.
      - Takes each connection's plaintext socket and reads the MCS Connect Initial to start the desktop at the client's size, through `start_headless_desktop` (`headless.hpp`: one factory per backend, stubs for the missing ones) or the test pattern desktop.
      - Runs the existing session loop per connection and keeps the desktop between connections.
      - Reports Disconnect, Stats and SessionEnded, and ends with the compositor (logout).
    - Configuration: everything farland-server takes on the command line that a daemon-run session needs is a key in `farland.toml` — `[graphics]` (GFX and bitmap codec, H.264 encoder, OpenH264 library, render node, zero-copy, ClearCodec, refinement, frame rate), `[network] autodetect`, `[audio]`, `[clipboard]`, `[policy] activation_timeout` and `[server] log_level`. The defaults are farland-server's, except `activation_timeout`, which is 60 s rather than 30 because a GDM session takes 10-20 s to start. `farlandd --check-config` prints every effective setting, one per line. The shipped `data/farland.toml` lists every key with its default and is checked against the parser by a test.
    - The broker protocol gained `Terminate` (daemon to agent) and `Settings` (daemon to agent, right after the agent's Hello and before the first connection), so the agent gets the configuration without reading `/etc`; the version is 2.
    - Auto-reconnect: `server::Connection` passes on the client's ARC cookie and sends Save Session Info. The agent checks the cookie against the session's `arc::Secret` and rotates it on every activation. NLA stays authoritative; a mismatch is logged.
    - Self-enrolment: `farlandctl passwd` calls `org.farland.Farland1.EnrolSelf` on the system bus. farlandd asks polkit (`enrol-self`, auth_self; farlandctl starts pkttyagent), checks the account password with PAM and stores the NT hash with the account.
    - `data/`: `farlandd.service`, the PAM file, the D-Bus and polkit policies, an example `farland.toml`.
    - `farlandctl sessions` and `farlandctl terminate` (S5): two more methods on the control API, `ListSessions` and `TerminateSession`. The questions arrive on the D-Bus thread and the answers live in the daemon's loop, so a `SessionView` sits between them: the loop publishes a snapshot of its registry after every turn, and a request to end a session waits there until the loop picks it up (no I/O, no clock, tested on its own). Anyone may list and end their **own** sessions; anything else needs the polkit action `org.farland.Farland1.manage-sessions` (auth_admin), and a caller without it sees only their own in the listing rather than an error. `terminate` takes a session id or `--user USER` for every session of an account, and ends them the way a policy timeout does (broker `Terminate`, EndReason `terminated`).
    - **Upgrading restarts the daemon.** The package used to run only `systemctl enable --now`, which does nothing to a service that is already running: an upgrade left the old `farlandd` talking the broker protocol to new agents on disk. That is silent until the protocol changes, and then every session fails to start with no useful message. `postinst` now runs `try-restart` as well. Sessions still do not survive it (below).
  - Tested:
    - Unit tests for the registry and policies, the launcher's command lines and environments, and self-enrolment's store update.
    - The agent in process against a scripted farlandd: handover, takeover, cookies, Terminate, logout, and the takeover question with an asker of the test's own.
    - `[policy] takeover`: the decisions with a fake clock, and the notification itself against a fake service on a private bus (`tests/apps/mock_notifications.py`): Yes, Cancel, dismissal, a timeout, a desktop with no service, and a question withdrawn while it stands.
    - End to end with the real farlandd, network processes and agents (`--no-pam`, test pattern) and a scripted TLS/NLA client:
      - Two users get separate sessions.
      - A dropped client returns to its session (keys typed before still shown, cookie verified).
      - A second connection takes over.
      - A client beyond `max_sessions` receives ERRINFO_SERVER_DENIED_CONNECTION.
    - Builds: macOS (Apple clang), Ubuntu 26.04 (GCC 15, Clang 21).
    - Live as root on Ubuntu 26.04 with GDM 50.0 and FreeRDP 3.31:
      - PAM route with the test pattern: logind sessions of type wayland, class user, Remote=yes. Two users at once, reconnect with the typed keys still there, takeover, idle and disconnected timeouts, refusal of a user logged in at the seat. Stopping farlandd closes the PAM sessions.
      - GNOME route with the Mutter backend of S2: a headless GNOME session per user through GDM, reconnect (the virtual monitor follows the new client size), logout ending the farland session, and stopping farlandd ending the GNOME session.
      - Self-enrolment with polkit, including a refused wrong password.
  - Differences from the plan:
    - GDM 50.0 has no `preauthenticated-user` for `CreateRemoteDisplay`: it only starts a greeter there. farlandd uses `CreateUserDisplay`, which logs the user in through `gdm-autologin`.
    - FreeRDP 3.31 does not announce Set Error Info support, so it shows its generic "logged off" message for takeovers, timeouts and refusals, rather than the precise reason.
  - Fixed after a live run (2026-09-19):
    - Handing the seat back created a **new greeter every time**. `switch_seat_to_greeter()` called GDM's `CreateTransientDisplay` unconditionally, and GDM makes another transient display on each call: four takeover attempts on the test host left four greeter sessions, each with a GNOME Shell of its own, until Mutter stopped answering D-Bus and every later connection died with "the desktop did not start". It now looks for a session of class `greeter` on the seat and activates that one, and only creates a display when the seat has none.
  - Open:
    - The Plasma and wlroots backends and their live test (S3, S4).
    - `on_local_session = "attach"` live.
    - mstsc and Windows App.
    - The rest of S5: metrics, and packaging beyond the Debian one (rpm, AUR, a container image).
    - Sessions do not survive a farlandd restart, which now matters more: every package upgrade restarts the daemon and so disconnects everyone.
    - The agent answers the MCS Connect Initial only once its desktop has started, which takes GDM 10–20 s; FreeRDP needs `/timeout`.

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
