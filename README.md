# farland

**A Remote Desktop Protocol server for Linux and Wayland.** Connect to a
Linux machine with the Remote Desktop client you already have — Microsoft's
mstsc or Windows App, FreeRDP, or any other RDP client — and get a Wayland
desktop: your own, or a headless one that exists only for the connection.

farland is written in C++ on its own sans-IO protocol core, and is meant to
succeed FreeRDP's server as the RDP stack on Linux.

```
mstsc / Windows App / FreeRDP  ──RDP──▶  farlandd  ──▶  a desktop per user
                                                        (GNOME, Plasma, sway,
                                                         labwc, cage)
```

## What it does

- **Connect the way you already do.** NLA (CredSSP with NTLMv2 or Kerberos),
  so clients ask for the password before the desktop appears, and a
  workstation with a Kerberos ticket signs in without one.
- **A desktop per user, behind one port.** `farlandd` gives each user their
  own headless session on port 3389: GNOME through GDM, or Plasma, sway,
  labwc and cage through farland's own logind sessions. Disconnect and the
  session keeps running; reconnect and you are back in it.
- **Or share the desktop that is running.** `farland-server --share` serves
  the Wayland session in front of you, through xdg-desktop-portal, on any
  compositor whose portal supports RemoteDesktop.
- **A picture that holds up.** The RDP Graphics Pipeline with RemoteFX
  Progressive, ClearCodec for text and UI, and H.264 (AVC420 and AVC444) for
  moving picture — encoded on the GPU through NVENC or VA-API where there is
  one. Text stays sharp, video stays smooth, and a desktop that stands still
  ends up pixel-exact.
- **The rest of a remote desktop.** Clipboard both ways (text, HTML, images,
  files), audio out and microphone in, the client's camera as a local camera,
  multi-monitor, resizing while connected, touch and pen.
- **Adapts to the link.** Round trip and bandwidth are measured
  continuously, and the frame rate, quantisation and bitrate follow.
- **Built to be run on a server.** Each client is served by a separate,
  sandboxed network process — `nobody`, Landlock, seccomp, no file system —
  that never sees a password hash or a keytab. Upgrading `farlandd` does not
  disconnect anybody.

## Status

Pre-release and unversioned: there is no tagged release yet, and the packages
below are built from a checkout. The server is what works today (milestone M6
of [docs/ROADMAP.md](docs/ROADMAP.md)); a native Wayland *client* is still to
come. It is tested against FreeRDP; mstsc and Windows App are only partly
tried, and the feature notes in the documentation say what each was tested
with.

## Try it without installing anything

The container image serves a synthetic test desktop — colour bars, a bouncing
square, a crosshair that follows your pointer — so you can point a client at
it without a compositor, a GPU or a login:

```sh
podman build -t farland -f packaging/container/Containerfile .
podman run --rm -p 3389:3389 -e FARLAND_USER=alice -e FARLAND_PASSWORD=secret farland
xfreerdp3 /v:localhost:3389 /u:alice /p:secret /cert:ignore /gfx:progressive
```

## Install

There is no apt or dnf repository yet, so the packages are built from a
checkout. Every push builds the Debian, RPM and Arch packages in CI and keeps
them as downloadable artifacts, if you would rather not build them yourself.

```sh
git clone https://github.com/mKenfenheuer/farland.git
cd farland
```

### Debian and Ubuntu

```sh
sudo sh ci/install-deps.sh                  # the build dependencies
sudo apt-get install -y dpkg-dev
sh packaging/make-deb.sh build-deb .        # farland_<version>_<arch>.deb
sudo apt-get install ./farland_*.deb
```

### Fedora and openSUSE

```sh
sudo dnf install -y rpm-build "dnf-command(builddep)"
sudo dnf builddep -y packaging/farland.spec
sh packaging/make-rpm.sh .                  # farland-<version>-<release>.rpm
sudo dnf install ./farland-*.rpm
```

### Arch Linux

The AUR recipe is `farland-git`, in [packaging/aur/](packaging/aur/):

```sh
cd packaging/aur && makepkg --syncdeps --install
```

### From source

Nothing has to be packaged to run farland. It needs GCC ≥ 13 or Clang ≥ 19
(on Ubuntu 24.04, `clang-19`), Meson ≥ 1.3 with Ninja, and OpenSSL ≥ 3.0
development files. Catch2 is fetched automatically if the system does not
provide it.

```sh
meson setup build --prefix=/usr/local
meson compile -C build
meson test -C build
sudo meson install -C build
```

With a prefix other than `/usr`, three files have to be copied to where the
system looks for them; the install notes in
[docs/MULTI-SESSION.md](docs/MULTI-SESSION.md) say which.

### What the packages do

The Debian and RPM packages install `farlandd`, `farland-agent`,
`farland-server` and `farlandctl`, the systemd service, and the PAM, D-Bus
and polkit files. They enable and start `farlandd.service` on port 3389, copy
the reference configuration to `/etc/farland/farland.toml` the first time,
and never touch that file again on an upgrade. An upgrade restarts the daemon
without disconnecting anyone.

The Arch package follows Arch's convention instead: it writes nothing into
`/etc` and enables nothing, and prints the three commands to run.

## First connection

After installing the deb or the RPM:

```sh
sudoedit /etc/farland/farland.toml   # pick [session] desktop, at least
farlandctl passwd                    # as each user who may log in
```

`farlandctl passwd` asks for your own account password, checks it with PAM
and stores the hash NLA needs, so you can then log in over RDP with your
normal account name and password. Then, from another machine:

```sh
xfreerdp3 /v:SERVER:3389 /u:alice /p:"…" /cert:tofu /dynamic-resolution /timeout:60000
```

Or in mstsc or Windows App: the host name, your user name and your password.
Starting a GNOME session through GDM takes 10–20 seconds on the first
connect, which is longer than FreeRDP waits by default — hence
`/timeout:60000`.

`farlandctl sessions` lists what is running, and `farlandctl terminate` ends
a session.

### Choosing a desktop

`[session] desktop` in `/etc/farland/farland.toml` picks what each user gets:
`gnome` (through GDM), `plasma`, `sway`, `labwc`, `cage`, or `test` for the
synthetic desktop. Which ones a build can start is listed under "Headless
desktops" in the `meson setup` summary.
[docs/MULTI-SESSION.md](docs/MULTI-SESSION.md) describes each, along with the
policies for timeouts, session takeover, and what happens when the user is
already logged in at the machine itself.

### Patched mutter and kwin packages

One setting needs more than the distributions ship. Handing a client the very
session a user has open at the machine (`[policy] on_local_session =
attach`) needs a Mutter that keeps drawing while the session is not active on
its seat, and on Plasma it needs a KWin that goes on configuring its virtual
outputs off-seat. Both fixes are waiting on upstream; until then these
scripts build Debian packages that differ from the distribution's only by
them. They need `deb-src` lines enabled, and Ubuntu 26.04 — the forks sit on
mutter 50.1 and KWin 6.6.6.

```sh
git submodule update --init packaging/mutter    # or packaging/kwin
sudo apt-get install -y dpkg-dev quilt
sudo apt-get build-dep -y mutter                # or kwin
sh packaging/make-mutter-deb.sh out             # or make-kwin-deb.sh
sudo apt-get install ./out/libmutter-*.deb ./out/mutter-common*.deb
sudo systemctl restart gdm                      # for a session to pick them up
```

Without them farland still works: it falls back to switching the seat's
monitors off while the client holds the session. The `Compositor packages`
workflow builds both on demand.

## Documentation

- **[docs/MULTI-SESSION.md](docs/MULTI-SESSION.md)** — `farlandd`: the
  desktops, configuration, policies, Kerberos, users, metrics and upgrades.
- **[docs/SERVER.md](docs/SERVER.md)** — `farland-server`: sharing a desktop,
  the headless backends, and every graphics, audio, clipboard and input
  option.
- **[docs/PLAN.md](docs/PLAN.md)** — scope, architecture, testing, security,
  risks.
- **[docs/ROADMAP.md](docs/ROADMAP.md)** — milestones M0–M8 (server 1.0),
  C1–C5 (client), phase 3.
- **[docs/SPECS.md](docs/SPECS.md)** — the specifications farland implements.
- **[CONTRIBUTING.md](CONTRIBUTING.md)** — sanitizer and fuzzing builds, the
  compositor tests, and the coding rules.

## License

Apache License 2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE).
