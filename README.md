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

## Quick start

### In a container

A whole GNOME desktop, with nothing to install but Docker or Podman:

```sh
docker run -d --name farland-gnome --privileged --tmpfs /run --tmpfs /run/lock --shm-size 2g \
    -p 3389:3389 -e FARLAND_USER=alice -e FARLAND_PASSWORD=secret \
    -v farland-home:/home -v farland-state:/var/lib/farland \
    -v farland-systemd:/var/lib/systemd -v farland-accounts:/var/lib/AccountsService \
    ghcr.io/mkenfenheuer/farland-gnome
```

Then connect to `localhost` as `alice` with the password `secret`: in mstsc
or Windows App, or with
`xfreerdp3 /v:localhost /u:alice /p:secret /cert:tofu /dynamic-resolution /timeout:60000`.
The first connection takes 10–20 seconds while GDM starts the session.

- **Podman:** the same command with `podman` (a rootful machine on macOS).
- **Plasma:** `ghcr.io/mkenfenheuer/farland-plasma` instead. It needs a GPU
  render node, which any Linux host with a GPU has; on a Mac, Podman's
  libkrun machine has one and Docker Desktop does not.
- **More:** a compose file that runs both, adding software, and a test
  pattern image that needs no privileges at all:
  [docs/CONTAINERS.md](docs/CONTAINERS.md).

### On your GNOME or Plasma machine

On Debian or Ubuntu (amd64), install the package from the latest build of
main, here with the [GitHub CLI](https://cli.github.com), or from the
[continuous release](https://github.com/mKenfenheuer/farland/releases/tag/continuous):

```sh
gh release download continuous -R mKenfenheuer/farland -p 'farland_*_amd64.deb'
sudo apt install ./farland_*_amd64.deb
```

The package starts `farlandd` on port 3389. Tell it which desktop to give
each user, then enrol yourself:

```sh
# GNOME
sudo sed -i 's/^# gdm_display = "seat"/gdm_display = "headless"/' /etc/farland/farland.toml
# Plasma
sudo sed -i 's/^desktop = "gnome"/desktop = "plasma"/' /etc/farland/farland.toml

sudo systemctl restart farlandd
farlandctl passwd        # as each user who may log in; asks for the account's password
```

Then connect from another machine with your account name and password, to
the machine's name or address. Every client gets a desktop of its own,
beside whatever is open at the machine itself, and finds it again when it
reconnects. Other distributions are under [Install](#install), and
[First connection](#first-connection) says what the two settings above mean.

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
below are built from a checkout. It is at milestone M6 of
[docs/ROADMAP.md](docs/ROADMAP.md). It is tested against FreeRDP; mstsc and
Windows App are only partly tried, and the feature notes in the
documentation say what each was tested with.

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

Two settings need more than the distributions ship, because a compositor
will not draw a session that is not active on its seat — and a session held
by a remote client is exactly that.

**No distribution carries these patches.** Both fixes are waiting on
upstream, so this applies to every stock install, whatever the
distribution's version number suggests.

What needs them:

- **`[session] gdm_display = "seat"`** (the default), which gives each GNOME
  session a virtual terminal so that logging in at the greeter comes back to
  it. Without a Mutter that keeps drawing off the seat, farland has to bring
  the session *to* the seat to get a picture at all: the desktop is on the
  machine's screen for as long as the client holds it, and a second user's
  session taking the seat ends the first client's connection. One client at a
  time, in public.
- **`[policy] on_local_session = "attach"`**, which hands a client the very
  session a user has open at the machine. The same applies, and on Plasma it
  also needs a KWin that goes on configuring its virtual outputs off-seat.

**On a stock distribution, set `[session] gdm_display = "headless"`.** A
headless session has no seat, so none of the above arises: sessions run
side by side and nothing of them reaches the machine's screen. What it costs
is the greeter — logging in at the machine starts a second, empty session
rather than coming back to the one that is open. That is the trade until the
patches land, and it is one line in `/etc/farland/farland.toml`.

The patched packages are built by the `Compositor packages` workflow for
Debian, Fedora and Arch, and published in the `continuous` prerelease beside
farland's own. Install them over the distribution's: each carries a version
above it, so `apt policy`, `rpm -q` or `pacman -Qi` says which compositor a
machine runs, and the distribution's own upgrade takes it back.

To build them yourself — the commits are applied to whatever release your
distribution packages, and the build stops if they no longer fit:

```sh
git submodule update --init packaging/mutter          # or packaging/kwin

# Debian and Ubuntu (needs deb-src lines)
sudo apt-get install -y dpkg-dev quilt
sudo apt-get build-dep -y mutter
sh packaging/make-mutter-deb.sh out                   # or make-kwin-deb.sh
sudo apt-get install ./out/libmutter-*.deb ./out/mutter-common*.deb

# Fedora
sudo dnf install -y rpm-build rpmdevtools 'dnf-command(builddep)'
sudo dnf builddep -y mutter
sh packaging/make-compositor-rpm.sh mutter out        # or kwin
sudo dnf install ./out/*.rpm

# Arch (makepkg refuses to run as root)
sudo pacman -S --needed base-devel devtools python
sh packaging/make-compositor-arch.sh mutter out       # or kwin
sudo pacman -U ./out/*.pkg.tar.zst

sudo systemctl restart gdm     # or sddm, for a session to pick them up
```

## Documentation

- **[docs/MULTI-SESSION.md](docs/MULTI-SESSION.md)** — `farlandd`: the
  desktops, configuration, policies, Kerberos, users, metrics and upgrades.
- **[docs/CONTAINERS.md](docs/CONTAINERS.md)** — the container images: the
  test pattern, and GNOME and Plasma desktops; persistence, the GPU, and
  Docker versus Podman.
- **[docs/SERVER.md](docs/SERVER.md)** — `farland-server`: sharing a desktop,
  the headless backends, and every graphics, audio, clipboard and input
  option.
- **[docs/PLAN.md](docs/PLAN.md)** — scope, architecture, testing, security,
  risks.
- **[docs/ROADMAP.md](docs/ROADMAP.md)** — milestones M0–M8 (1.0) and what
  comes after.
- **[docs/SPECS.md](docs/SPECS.md)** — the specifications farland implements.
- **[CONTRIBUTING.md](CONTRIBUTING.md)** — sanitizer and fuzzing builds, the
  compositor tests, and the coding rules.

## License

Apache License 2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE).
