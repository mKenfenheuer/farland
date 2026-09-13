# Connection transcripts

Recorded RDP byte streams, replayed through the sans-IO protocol core in tests. Each file captures one connection from one client or server build, after TLS decryption.

## Format

```
# Comment until end of line. Blank lines are ignored.
C> 03 00 00 13          # a record sent by the client
   0e e0 00 00 00 00 00 # an indented line continues the previous record
S> 03 00 00 13 ...      # a record sent by the server
```

- `C>` starts a client-to-server record, `S>` a server-to-client record.
- A record is one unit the way the transport delivered it, such as one TPKT or one fast-path PDU. Tests must not assume records line up with PDU boundaries.
- Hex is case-insensitive, and every line must contain whole bytes.

## Naming

`<client-or-server>-<version>-<scenario>.txt`, for example `mstsc-win11-24h2-nla-gfx.txt`. Put the capture setup (host, client settings, date) in comments at the top.

## Capturing

1. Capture with `SSLKEYLOGFILE` set. FreeRDP honours it; for mstsc, capture on the server side with farland's debug dump (M1) instead.
2. Decrypt in Wireshark and export the TCP payload per direction.
3. Convert it to this format and remove anything sensitive: passwords in TSCredentials, usernames, host names, certificates.
