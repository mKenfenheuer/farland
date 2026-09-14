# Specification index

farland implements RDP from the published specifications. FreeRDP, IronRDP, ZeroVDI and macRDP are behavioural references only (docs/PLAN.md §4).

## Citing specifications in code

Every parser and encoder cites the section that defines its structure, next to the code:

```cpp
// [MS-RDPBCGR] 2.2.1.3.2 Client Core Data (TS_UD_CS_CORE)
```

- State-machine transitions cite the processing-rules section (usually 3.x).
- When farland deviates from the text for interoperability, say which peer needs it and point to the regression test, for example `// mstsc rejects ... without this (error 0x608); see tests/proto/test_gcc.cpp`.
- Test names cite the section they check.

## Documents

Microsoft Open Specifications: `https://learn.microsoft.com/openspecs/windows_protocols/<name>/`, where `<name>` is the lowercase document name (for example `ms-rdpbcgr`).

Markdown copies of MS-RDPBCGR, -RDPEGFX, -RDPRFX, -RDPEDYC, -RDPECLIP and -RDPEVOR are in `~/Git/ksol-rdpgw/Spec/`. Do not add Microsoft's documents to this repository.

| Document | Covers | Needed from |
|---|---|---|
| **MS-RDPBCGR** | Connection sequence, security headers, capability sets, fast-path, redirection, auto-detect | M1 |
| MS-RDPELE | Licensing | M1 |
| ITU-T X.224 / RFC 1006 (TPKT) | Transport framing | M1 |
| ITU-T T.125 (MCS), T.124 (GCC) | Domain and conference PDUs | M1 |
| ITU-T X.690 (BER/DER), X.691 (PER) | ASN.1 encodings in `farland-base` | M0 |
| **MS-CSSP** | CredSSP / NLA | M2 |
| MS-NLMP | NTLM | M2 |
| MS-SPNG, RFC 4178 | SPNEGO | M2 |
| RFC 5929 | TLS channel bindings (`tls-server-end-point`) | M2 |
| MS-KILE | Kerberos extensions | M2 / M7 |
| **MS-RDPEGFX** | Graphics pipeline, ZGFX, AVC420/444, caches | M3 |
| MS-RDPEDYC | Dynamic virtual channels | M3 |
| MS-RDPRFX | RemoteFX, including the progressive codec | M3 |
| MS-RDPEDISP | Display control (resize, multi-monitor) | M6 |
| MS-RDPECLIP | Clipboard | M6 |
| MS-WMF 2.2.2 (DeviceIndependentBitmap, BitmapInfoHeader, BitmapV5Header) | Clipboard images (CF_DIB, CF_DIBV5) | M6 |
| PNG (W3C, 3rd edition), RFC 1950 (zlib) | Clipboard images as image/png | M6 |
| HTML Clipboard Format (CF_HTML), RFC 2483 (text/uri-list), RFC 8089 (file URIs) | Clipboard HTML and files | M6 |
| MS-RDPEA, MS-RDPEAI | Audio output and input | M6 |
| MS-RDPEI | Touch and pen input | M6 |
| MS-RDPECAM | Camera redirection | M6 (optional) |
| MS-RDPEFS | Device redirection (rdpdr) | phase 2 |
| MS-RDPERP | RemoteApp (RAIL) | phase 2 |
| MS-TSGU | RD Gateway | phase 2 |
| MS-RDPEMT, MS-RDPEUDP, MS-RDPEUDP2 | Multitransport over UDP | phase 3 |

## Linux platform interfaces

| Interface | Used for |
|---|---|
| xdg-desktop-portal `org.freedesktop.portal.ScreenCast`, `RemoteDesktop`, `Clipboard` | Portal backend (M4, M6) |
| libei / EIS | Input injection (M4) |
| PipeWire, SPA video metadata (`SPA_META_VideoDamage`, `SPA_META_Cursor`) | Capture, damage and cursor (M4) |
| `org.gnome.Mutter.ScreenCast` / `RemoteDesktop` | Mutter backend (M4, M7) |
| `zkde_screencast_unstable_v1` | KWin backend (M4, M7) |
| Wayland `ext-image-copy-capture-v1`, `ext-image-capture-source-v1` | wlroots backend (M4) |
