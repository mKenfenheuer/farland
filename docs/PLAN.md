# farland: project plan

farland is a modern RDP implementation for Linux, meant to succeed FreeRDP on Wayland desktops. It is written in C++ with its own protocol core. It ships **server first**: a Wayland/PipeWire RDP server that works on any compositor. The native Wayland client is built second, on the same protocol core.

This document covers scope, architecture, reference material and risks. The phased milestones are in [ROADMAP.md](ROADMAP.md).

---

## 1. Why another RDP stack

| | FreeRDP 3.x (Sep 2026, v3.31) | gnome-remote-desktop / KRdp | **farland goal** |
|---|---|---|---|
| Wayland server | None in-tree. The shadow server supports X11, Mac and Windows only; there is no PipeWire or portal code | Tied to one desktop each; both built on `libfreerdp` peer APIs | Works on any compositor through portals, with native wlroots, Mutter and KWin backends |
| Wayland client | `wlfreerdp` is deprecated (wl_shm only; no fractional scale, relative pointer or IME). The SDL3 client is the official path, but it gets its Wayland support indirectly through SDL | n/a | Native Wayland client: dmabuf rendering, fractional-scale, pointer-constraints, text-input-v3, RemoteApp windows as `xdg_toplevel` |
| ClearCodec encoder | Stub (`clear.c:1273` "TODO: not implemented") | none | A real encoder for text and UI content |
| AVC444 | Present | GRD: yes | True 4:4:4 (v1/v2), hardware encoded, zero-copy from dmabuf |
| UDP transport (MS-RDPEUDP) | Not implemented; multitransport is always declined | none | Planned for phase 3 |
| Codebase | About 104k lines of WinPR (Win32 emulation) plus about 92k lines of core. Around 55 CVEs in 2026, mostly in codec and parser bounds checks | — | No Win32 emulation; small sans-IO core, fuzzed from day one |

**What sets farland apart:** it does not depend on any one compositor, it does not link `libfreerdp`, it treats content-aware mixed-codec encoding as a core feature, it includes a multi-session broker, and it enforces memory-safety discipline in C++.

---

## 2. Decisions

| Decision | Choice | Notes |
|---|---|---|
| First deliverable | **Server** | The client follows once the server has shipped (phase 2) |
| Language | **C++23** (GCC ≥ 13, Clang ≥ 19) | `std::span`, `std::expected`, `std::byte`; see §6 on how memory-safety risk is handled |
| Protocol core | **Our own**, sans-IO | ZeroVDI, macRDP, FreeRDP and IronRDP serve as behavioural references only |
| Build | **Meson** (+ wraps) | Consistent with the PipeWire, libei and GNOME/wlroots ecosystems, and pkg-config native. Keep the set of build options small; FreeRDP has about 240 |
| TLS/crypto | **OpenSSL 3** | TLS server/client, X.509 generation, and the NTLM primitives via EVP. MD4 needs the legacy provider or its own implementation, because OpenSSL 3 disables MD4 by default |
| Kerberos/SPNEGO | **System GSSAPI** (MIT krb5) | We do not implement Kerberos ourselves, unlike FreeRDP's in-tree package |
| NTLM | **Our own implementation** | The server has to verify NTLMv2 against a stored NT hash; ports of macRDP's and ZeroVDI's code are available |
| License | **Apache-2.0** | Compatible with reading and translating FreeRDP code; translated files keep FreeRDP's copyright notices and are marked as modified. An optional x264 backend makes that build's combined binary GPLv3, so x264 stays opt-in and is never the default |
| Platform baseline | **2024-era distributions:** Debian 13, Ubuntu 24.04, Fedora 40, RHEL 10 and later | GCC ≥ 13 / Clang ≥ 19 (Clang 18 cannot use libstdc++'s `std::expected`), PipeWire ≥ 1.0, xdg-desktop-portal ≥ 1.18 (Clipboard portal), libei ≥ 1.0. RHEL 9 and Ubuntu 22.04 are not supported: no compatibility shims |
| Public API | **No stable API until the client phase** | Internal C++ APIs can change freely during server 1.0. A versioned `libfarland` C API ships with the client (C5), for Remmina, GNOME Connections and KRDC plugins. The server is controlled through D-Bus and `farlandctl` only |

All foundational decisions are settled (2026-09-13).

---

## 3. Architecture

```
                         ┌────────────────────────── farland-server ──────────────────────────┐
 RDP client ──TCP──►  ┌──────────────┐    ┌────────────────────────────────────────────────┐ │
 (mstsc, Windows App, │ pre-auth     │ fd │ session process                                │ │
  FreeRDP, web)       │ process      ├───►│  ┌──────────┐  ┌──────────┐  ┌──────────────┐  │ │
                      │ X.224 · TLS  │    │  │ proto    │  │ channels │  │ graphics     │  │ │
                      │ CredSSP/NLA  │    │  │ core     │◄►│ gfx disp │◄►│ scheduler    │  │ │
                      │ (sandboxed)  │    │  │ (sans-IO)│  │ clip snd │  │ + encoders   │  │ │
                      └──────────────┘    │  └──────────┘  └──────────┘  └──────▲───────┘  │ │
                                          │         platform backend (capture + input)     │ │
                                          │   portal+PipeWire+libei │ wlroots │ Mutter │ KWin │ │
                                          └────────────────────────────────────────────────┘ │
                         └─────────────────────────────────────────────────────────────────────┘
```

### 3.1 Libraries (one repo, several static/shared libs)

| Library | Contents | Shared with the client? |
|---|---|---|
| `farland-base` | Bounded `Reader`/`Writer` over `std::span`, `expected<T, Error>`, BER/PER/DER codecs, logging, and small containers | yes |
| `farland-proto` | **Sans-IO** PDU types (encode and decode) for BCGR, plus connection state machines for both roles: X.224, MCS/GCC, security headers, licensing, capability sets, fast-path, share/data PDUs, redirection, auto-detect and heartbeat. Bytes go in, events and bytes come out; it never touches sockets or threads | yes |
| `farland-auth` | TLS wrapper (OpenSSL), CredSSP acceptor/initiator (TSRequest v2–6), NTLMv2 (server and client, with MIC and channel bindings), SPNEGO/Kerberos via GSSAPI, RDSTLS, and credential stores | yes |
| `farland-channels` | Static virtual channel framing, the drdynvc multiplexer (v1–v3), and channel protocols, each split into a shared core and client/server roles: rdpgfx, disp, cliprdr, rdpsnd, audin, rdpei, rdpecam, and later rdpdr and rail | yes |
| `farland-codec` | Encoders first, decoders in phase 2: planar, RFX progressive, ClearCodec, AVC420/444 bitstream wrapping, ZGFX, MPPC/NCRUSH/XCRUSH (decode only), plus colour conversion and SIMD primitives (Google Highway or hand-written SSE4/AVX2/NEON) | yes |
| `farland-video` | H.264 encoder backends behind one interface: **VA-API** (zero-copy dmabuf → VASurface), NVENC, OpenH264 (runtime-loaded), and optionally x264. AV1 later | yes (decode in phase 2) |
| `farland-server` | Session engine: surface model, damage tracker, region classifier, codec selection, frame scheduler with flow control, congestion control, and pointer/cursor pipeline | no |
| `farland-platform-*` | Capture and input backends (§3.3) | no |
| `farlandd` / `farlandctl` | Daemon, TOML config, systemd units, D-Bus control API, and CLI | no |

### 3.2 Core design rules

1. **Sans-IO protocol core.** Each state machine exposes `feed(span<const byte>) → events` and `poll_transmit() → bytes`. Transport, TLS and threads live outside. That makes every protocol path unit-testable with recorded byte streams, and fuzzable. macRDP and ZeroVDI both mixed parsing with blocking I/O, and that made them hard to test.
2. **One parser per structure, never byte-scanning.** Both earlier servers found CS_CORE/CS_NET by scanning for `01 C0`/`03 C0` bytes, and read optional CS_CORE fields at fixed offsets. farland parses GCC blocks by their headers and length, including every optional CS_CORE field (desktopScaleFactor and the others), plus CS_CLUSTER, CS_MONITOR, CS_MONITOR_EX, CS_MCS_MSGCHANNEL and CS_MULTITRANSPORT.
3. **Structured concurrency.** Use an event loop per session (io_uring or epoll via a thin reactor; or Asio if we want it) plus a worker pool for encoding. Each channel has exactly one writer. Both macRDP and ZeroVDI had races where two DVC messages interleaved on the wire, and unlocked channel maps.
4. **Opaque, typed configuration from day one.** FreeRDP v2→v3 had to replace a public struct of 387 settings. farland uses versioned config schemas and opaque handles in any public API.
5. **Negotiate from the peer's capabilities; never echo them back blindly.** Both earlier servers echoed the client's GFX caps flags and ignored Confirm Active. farland stores the negotiated capabilities in a `Negotiated` object that the rest of the session reads.
6. **Treat every client input as hostile**, including the ones that only appear before authentication: X.224 cookies and routing tokens, GCC, and NTLM AV pairs. All of them go through the pre-auth process, which runs unprivileged.

### 3.3 Platform backends (server)

| Backend | Capture | Input | Virtual monitors / headless | Target |
|---|---|---|---|---|
| **portal** (default) | xdg-desktop-portal ScreenCast → PipeWire stream (dmabuf with modifiers, SHM fallback, `SPA_META_VideoDamage`, `SPA_META_Cursor`) | RemoteDesktop portal `ConnectToEIS` → **libei** (keyboard as evdev keycodes, absolute pointer per region, button, scroll v120, touch). Falls back to the portal's `Notify*` methods | ScreenCast source type `VIRTUAL` where the portal supports it. Restore tokens for unattended access | GNOME, KDE, and any compositor with portal RemoteDesktop support |
| **mutter** | `org.gnome.Mutter.ScreenCast` (`RecordVirtual`, `RecordMonitor`) | `org.gnome.Mutter.RemoteDesktop` + EIS | Headless `mutter --headless`, with virtual monitors sized to the client (disp/multimon) | GNOME system/headless sessions |
| **kwin** | `zkde_screencast_unstable_v1` (`stream_virtual_output`) + PipeWire | EIS / `org_kde_kwin_fake_input` | `kwin_wayland --virtual` | Plasma headless sessions |
| **wlroots** | `ext-image-copy-capture-v1` + `ext-image-capture-source-v1` (fallback `wlr-screencopy`) | `zwp_virtual_keyboard_v1` + `zwlr_virtual_pointer_v1` | Headless sway/labwc with `wlr-output-management` | sway, Hyprland, labwc, river |
| **test** | Synthetic patterns and recorded frame files | Event log | n/a | CI, codec and interop tests |

The backend interface grew out of ZeroVDI's `IProtocolSource`, redesigned to fix its weaknesses:
- **Frames:** `FrameSource` delivers zero-copy `Frame{dmabuf | shm, format, modifier, damage region list}`. ZeroVDI instead allocated one `byte[]` per rectangle.
- **Cursor:** a separate `CursorSource` provides cursor shape, hotspot and position. Pointer updates are sent as Pointer PDUs (including large pointers) and are never baked into the video. macRDP baked them in while also advertising the pointer capability.
- **Input:** `InputSink` takes RDP scancodes through a fixed scancode→evdev table, so the compositor applies the layout. Unicode input is handled on a best-effort basis (§7).
- **Resize:** `resize(layout)` with multi-monitor layouts.

### 3.4 Graphics pipeline (server)

```
PipeWire frame (dmabuf) ──► damage regions ──► region classifier ──► per-region codec ──► RDPGFX commands ──► ZGFX ──► drdynvc
                                  │                (text/UI, static,        │
                                  │                 video, scroll)          ├─ ClearCodec   (text/UI, small regions)
                                  └─ scroll/move detection ─► SurfaceToSurface ├─ Progressive (static photos, refined over time with UPGRADE passes)
                                                                             ├─ AVC420/444  (video/high-motion; VA-API zero-copy)
                                                                             ├─ Planar      (lossless fallback, cursor-sized regions)
                                                                             └─ SolidFill / CacheToSurface (cheap wins)
```

- **RDPGFX server:**
  - Capability selection covers 8.0 through 11.x, with correct `capsDataLength` per version (ZeroVDI always used 4).
  - Multiple surfaces, and surface↔output mapping for multimon, including scaled output.
  - Surface caches with `CacheImportOffer`/`Reply` and eviction.
  - `ResetGraphics` on resize.
- **Flow control:** keep the design proven in ZeroVDI and macRDP, a frames-in-flight window plus `SUSPEND_FRAME_ACK`. Also use the client's `queueDepth` and QoE acks, which both earlier servers ignored. Congestion tiers (quality/bitrate ladder) driven by RTT and bandwidth, measured with MS-RDPBCGR auto-detect.
- **Progressive:** port the encoder from macRDP/ZeroVDI (the FreeRDP pipeline: DWT, quantization, RLGR1). Add the missing pieces: real `TILE_UPGRADE` passes, RLGR3, reduce-extrapolate DWT, SIMD, and a way to send more than 16 KB per frame without breaking mstsc's cap.
- **AVC:**
  - Real region rectangles with per-region QP and quality, instead of one full-frame rectangle at qp 26.
  - True **AVC444 v1/v2**, i.e. a second chroma stream; both earlier servers only wrapped the 4:2:0 stream as LC=1.
  - Check the colour matrix and range (BT.601 vs BT.709, limited vs full) against MS-RDPEGFX and FreeRDP's primitives; ZeroVDI used BT.601 limited without checking.
- **ClearCodec encoder:** there is no open-source reference encoder. We build it against the spec and use the decoders in ZeroVDI (`clear.js`) and FreeRDP (`clear.c`) as test oracles. It covers residual, bands with V-bar caches, the RLEX subcodec, and the glyph cache.

### 3.5 Authentication model (server)

NTLM-based NLA means the server must know the account's **NT hash**, so it cannot verify system (PAM) passwords directly. farland therefore offers these modes:

| Mode | How | Use case |
|---|---|---|
| **Per-server credential** (default) | NT hash stored at `0600` in the user file (`user:domain:hash`); NTLMv2 and MIC verified | Screen sharing / single user, like GRD's user mode |
| **Self-enrolled store** | Each user enrols once with `farlandctl passwd`: over D-Bus and polkit (`auth_self`), farlandd checks the account password through PAM and stores the NT hash with the local account (`user:domain:hash:account`) | Multi-user headless sessions without a directory |
| **Kerberos** | GSSAPI acceptor with a keytab (`TERMSRV/host`) via SPNEGO; the principal is mapped to a local user | AD-, FreeIPA- or SSSD-joined hosts; multi-user without shared secrets |

A "delegated login" that checks the client's plaintext `TSCredentials` through PAM cannot replace the store: NTLM needs the NT hash before any credentials are delegated. Delegated credentials can still be handed to PAM afterwards, to unlock the keyring in the user's session.

For multi-session, the system daemon (farlandd) keeps the single port, and its sandboxed network process keeps TLS for the connection's lifetime. After NLA the plaintext connection goes to the user's session agent (farland-agent, running in the user's logind session) as a descriptor over a Unix socket (SCM_RIGHTS). This is one TCP connection that every client accepts, without RDSTLS, and the TLS key never leaves the daemon. GNOME sessions come from GDM's RemoteDisplayFactory (`CreateUserDisplay`: headless, logged in through gdm-autologin); Plasma, wlroots and cage sessions from farland's own PAM and logind launcher. Server Redirection with RDSTLS, as GRD uses it, remains an option for spreading sessions over several hosts.

---

## 4. Reference material and what to reuse

| Source | Component | Verdict |
|---|---|---|
| **macRDP** (`~/Git/macRDP`, Swift, ~7.4k lines) | Server connection sequence end to end: X.224, TLS server with serverAuth EKU, CredSSP acceptor v2–6, NTLMv2 server, MCS, licensing, Demand Active, reactivation | **Primary behavioural reference.** Rewrite with real parsers; it scans bytes, has no MIC check, ignores Confirm Active, and has no tests |
| macRDP | `RfxProgressiveEncoder`, `RdpGfxServer`, `DvcServer`, `DisplayControlServer`, `RdpSndServer` | **Port** to C++. These parts are clean and callback-driven |
| macRDP | Dropped-frame re-encode on ack, letterbox mapping, virtual-display + daemon concept | Keep the concepts |
| **ZeroVDI server bridge** (`89d992d^:KSol.ZeroVDI/RDP/Bridge`) | Frame clock, frames-in-flight gate, congestion tiers (CRF ladder), libx264 settings (`threads=1`, AUD framing, idle flush, VBV-capped CRF), dirty bounding-box progressive | **Keep the design, rewrite the code.** Its front end only works with the rdpweb client (no X.224, TLS or NLA), so drop it |
| **ZeroVDI client** (HEAD) | CredSSP/NTLM client (verifies pubKeyAuth, maps error codes), TOFU certificate pinning, ClearCodec/Progressive/ZGFX decoders (FreeRDP-faithful, ~95% complete), GFX command handling, server-redirection parser with tests | **Port for phase 2** (client), and use the decoders now as **server test oracles** |
| ZeroVDI client | `protocol.js` connection sequence (magic bytes copied from mstsc) | Only reference it for what mstsc actually sends |
| **FreeRDP 3** (Apache-2.0) | nego/NLA/RDSTLS/AAD flows, rdpgfx server/client, codecs (including the AVC444 split and the planar encoder), channel servers, shadow encoder selection | Behavioural reference. Its decoders are the **interop test oracle**. Keep Apache notices on any translated code |
| **FreeRDP 2** | How the settings API and channel APIs evolved into v3; CVE fixes (the ChangeLog lists 30) | Lessons only |
| Spec copies (`~/Git/ksol-rdpgw/Spec`) | Markdown versions of MS-RDPBCGR/-EGFX/-RFX/-EDYC/-ECLIP/-EVOR (~46k lines) | Cite by section number in code comments, following ZeroVDI's convention. Add MS-CSSP, MS-NLMP, MS-RDPEDISP, MS-RDPEA, MS-RDPEAI, MS-RDPEI, MS-RDPELE and MS-RDPEUDP |

### 4.1 mstsc / Windows App compatibility lessons (from macRDP history)

These cost real debugging time. Each needs a regression test:

- The TLS certificate needs the **serverAuth EKU**; otherwise the client fails with 0x907.
- Attach User Confirm and Channel Join Confirm must set the **optional-field-present bit** (0x2E/0x3E).
- SC_CORE must **echo the client's `requestedProtocols`**; otherwise 0x608.
- **Exact `totalLength`** in share headers; otherwise 0x500d.
- The Bitmap capability must advertise **32 bpp** for mstsc to accept GFX; otherwise 0x200d.
- The **Multifragment Update capability** is required; otherwise 0x1204.
- Legacy bitmaps: interleaved RLE was rejected with 0x510d. Revisit this with planar.
- Progressive: **at most 16 KB per stream**, and the WireToSurface2 `bitmapDataLength` must be correct; otherwise 0x8007006f.
- CredSSP binds to the **PKCS#1 RSAPublicKey**, not the whole SPKI; otherwise 0x204.
- ZeroVDI bug: a `Cookie: mstshash=` token in the X.224 CR must be skipped before `RDP_NEG_REQ`, or the requested protocols are read as 0.

---

## 5. Testing strategy

1. **Unit tests** (Catch2 or doctest) for every PDU round trip, with spec example bytes where the spec gives them.
2. **Transcript tests:** recorded, TLS-decrypted connection byte streams from mstsc, Windows App (macOS/iOS/Android), FreeRDP 2/3, Remmina, GNOME Connections, KRDC and the ZeroVDI web client, replayed through the sans-IO core.
3. **Codec conformance:**
   - Farland encodes a fixed corpus (text, UI, photos, video); FreeRDP's decoders and ZeroVDI's C#/JS decoders decode it.
   - Require bit-exact output for lossless codecs, and a PSNR/SSIM threshold for lossy ones.
4. **Live interop CI:** a containerised headless compositor + farland on one side; `sdl-freerdp`/`xfreerdp` (v2 and v3) connect and take screenshots, which are compared against the expected output. mstsc and Windows App run as a manual pre-release matrix on real devices.
5. **Fuzzing from day one:**
   - A libFuzzer target for every parser: X.224, GCC, capability sets, fast-path input, TSRequest, NTLM messages, drdynvc, rdpgfx client→server, cliprdr and every decoder. Structure-aware where it helps.
   - Apply to OSS-Fuzz once the project is public.
6. **Sanitizers in CI:** ASan+UBSan and TSan builds, and hardened release flags (§6).

---

## 6. Handling memory safety in C++

Most FreeRDP CVEs come from missing bounds checks in codec and PDU parsers. Since farland is written in C++, safety has to come from discipline and tooling:

- **No raw pointer arithmetic in `proto`, `auth`, `channels` or `codec`.** All reads go through the bounds-checked `Reader`, which returns `expected`; length fields are validated before any allocation. Build with Clang `-Wunsafe-buffer-usage` (C++ Safe Buffers) as an error in those libraries.
- **Hardened build:** `-D_GLIBCXX_ASSERTIONS` / `_LIBCPP_HARDENING_MODE=extensive`, GCC `-fhardened`, `-ftrivial-auto-var-init=zero`, CFI where the toolchain supports it.
- **Resource limits** on every size that comes from the network: ZGFX multipart totals, drdynvc reassembly, cliprdr formats, capability counts and GFX surface sizes.
- **Privilege separation:** the pre-auth process (X.224, TLS, CredSSP) runs as an unprivileged user under seccomp and landlock. It only hands the socket and TLS session to the session process after authentication succeeds, following OpenSSH's privsep model. Codec encoders never parse network data. On the server, only client→server channels and input do.
- **An external security review** before 1.0.

---

## 7. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| C++ memory-safety bugs in parsers | Remote pre-auth RCE (the FreeRDP pattern) | §6: Safe Buffers, fuzzing, privsep, review |
| Portal permission UX: RemoteDesktop needs an interactive grant | Unattended access is awkward | Restore tokens; compositor-specific backends (Mutter, KWin) for system/headless mode; document per desktop |
| wlroots has no standard RemoteDesktop portal | Weaker wlroots support | A native wlroots backend (ext-image-copy-capture + virtual input protocols) |
| **Unicode keyboard events** (mobile clients, IME) cannot be injected through EIS with a fixed keymap | Text input broken from iOS/Android clients | Headless sessions: the compositor keymap is ours to extend. Portal sessions: best effort, with temporary keymap remapping where the compositor allows it; track upstream EIS/text-input work |
| Client keyboard layout ≠ server keymap | Wrong characters | Use CS_CORE `keyboardLayout` to choose the XKB layout for headless sessions; document for shared sessions |
| NTLM needs an NT hash; OpenSSL 3 lacks MD4 by default | Credential storage and packaging friction | Own MD4 implementation (fine for NTLM); Kerberos mode for managed fleets |
| H.264 patents and licensing; x264 is GPL | Distribution | VA-API/NVENC (driver-side licensing) and runtime-loaded Cisco OpenH264 as defaults; x264 as an opt-in build |
| mstsc strictness, with undocumented behaviour | Connection failures with opaque codes | §4.1 regression suite; transcript tests; test against mstsc early and continuously (from M1 on) |
| Scope creep (FreeRDP has 92k lines of channels) | Never reaching 1.0 | Server 1.0 scope is fixed in the roadmap; rdpdr, USB, RAIL and gateway come later |
| GRD and KRdp already exist | Adoption | Differentiate: works on any compositor, multi-session broker, better codecs (Clear, true AVC444, mixed mode), UDP transport |
