# Containers: farland without a host to install it on

farland comes in three container images. One serves a synthetic test
desktop and needs nothing. The other two are a whole Ubuntu desktop,
GNOME or Plasma, with farlandd in front of it, the way a host with the
package installed would run it.

| Image | Built from | What a client gets | Needs |
|---|---|---|---|
| `farland` | `packaging/container/Containerfile` | the test pattern: colour bars, a bouncing square, the pointer, the keys pressed | nothing |
| `farland-gnome` | `packaging/container/Containerfile.desktop`, target `gnome` | Ubuntu's GNOME session, through GDM | `--privileged` |
| `farland-plasma` | `packaging/container/Containerfile.desktop`, target `plasma` | Kubuntu's Plasma session, started by farlandd | `--privileged` and a GPU render node |

Tried on macOS (Apple silicon) with Windows App: GNOME under Docker Desktop,
and Plasma under Podman. The desktop images have not been run on a Linux
host yet.

## The test pattern

`farland-server` in an image, with no compositor, systemd or login behind
it: something to point a client at to see the protocol work.

```sh
podman build -t farland -f packaging/container/Containerfile .   # or docker build
podman run --rm -p 3389:3389 -e FARLAND_USER=alice -e FARLAND_PASSWORD=secret farland
xfreerdp3 /v:localhost:3389 /u:alice /p:secret /cert:ignore /gfx:progressive
```

Anything after the image name goes to `farland-server`, for example
`--gfx-codec avc420 --log-level debug`. `packaging/container/entrypoint.sh`
lists the variables it reads.

## The desktop images

Each desktop image is systemd, logind, D-Bus, polkit, PipeWire and the
farland Debian package, with `farlandd.service` on port 3389, as on an
installed host. A client logs in with NLA and gets a headless session of its
own, which keeps running when it disconnects. [MULTI-SESSION.md](MULTI-SESSION.md)
describes farlandd itself; here it runs as it would anywhere else.

- **GNOME:** Ubuntu's session (Yaru, the dock, the settings daemons), with
  Settings, GNOME Software, Ptyxis, Files, Text Editor, System Monitor,
  Calculator and Loupe. GDM starts each session on its remote display
  factory (`gdm_display = "headless"`), because the container has no seat.
- **Plasma:** Kubuntu's session, with System Settings, Discover, Konsole,
  Dolphin, Kate, Ark, Gwenview, Okular, Spectacle and System Monitor. There
  is no display manager: farlandd opens the session and starts KWin on its
  virtual backend itself.
- **Both:** Firefox from Mozilla's repository. Ubuntu's `firefox` package
  only installs the snap, and snapd does not run in a container, so the
  App Center is missing as well. GNOME Software and Discover install from
  Flathub instead. Sound goes to an output called "Remote Audio", which
  stands in for the sound card a container does not have; farland sends
  what plays there to the client.

### Running them

`packaging/container/compose.yaml` runs both, GNOME on port 3389 and Plasma
on 3390, with the options they need and their state on volumes:

```sh
FARLAND_USER=alice FARLAND_PASSWORD=secret \
    docker compose -f packaging/container/compose.yaml up -d            # both
FARLAND_USER=alice FARLAND_PASSWORD=secret \
    docker compose -f packaging/container/compose.yaml up -d gnome      # or one
xfreerdp3 /v:localhost:3389 /u:alice /p:secret /cert:tofu /dynamic-resolution /timeout:60000
```

With Podman, `podman compose` runs the same file (see
[Docker and Podman](#docker-and-podman)). Without `--build`, compose runs
the images CI publishes; `up -d --build` builds them from the checkout.

A new session takes 10–20 seconds to start on the first connection, which is
longer than FreeRDP waits by default, hence `/timeout:60000`. Windows App
waits long enough by itself.

### Settings

| Variable | Used | Meaning |
|---|---|---|
| `FARLAND_USER` | at every start | the account; default `farland` |
| `FARLAND_PASSWORD` | at every start | its password, for RDP and for the desktop; compose refuses to start without it |
| `FARLAND_PACKAGES` | when building | more packages for the image, from Ubuntu's, Mozilla's or Microsoft's repository |
| `FARLAND_LANGUAGES` | when building | languages the image carries translations for, as codes such as `de fr`: Ubuntu's language packs for the desktop, and Firefox's |
| `FARLAND_LOCALE` | at every start | language and formats, such as `de_DE.UTF-8`; its language has to be in `FARLAND_LANGUAGES` |
| `FARLAND_TIMEZONE` | at every start | the time zone, such as `Europe/Berlin` |
| `FARLAND_KEYBOARD` | at every start | the desktop's keyboard layout, as XKB names it: `de`, or with a variant, `de(nodeadkeys)` |

The settings applied at every start win over what was chosen inside the
desktop since: they set the user's own GNOME or KDE setting, not only the
system's default. Left empty, they change nothing. The keyboard layout is
the desktop's, not the client's: farland sends the keys by position, and
the desktop's layout decides what they type, so it has to match the
keyboard at the client.

Compose also reads them from `packaging/container/.env`, which git and the
image build both ignore, because it holds the password. With
`COMPOSE_FILE` in it as well, compose needs no `-f` options when run from
that directory:

```sh
# packaging/container/.env
FARLAND_USER=alice
FARLAND_PASSWORD=secret
COMPOSE_FILE=compose.yaml:compose.home.yaml

cd packaging/container && docker compose up -d
```

The account is created again at every start, with the password from the
environment and the user ID its home directory already has. It is in the
`sudo` group. Logging in over RDP with that password unlocks the session's
keyring as well (GNOME Keyring, or KWallet on Plasma;
[MULTI-SESSION.md](MULTI-SESSION.md) says how), so applications do not ask
for it again. A password changed inside the desktop lasts until the next
start. A polkit rule
(`packaging/container/49-farland-software.rules`) lets the `sudo` group
install, update and remove Flatpak and PackageKit software without being
asked for the password at every step; everything else still asks.

### The host's folders

`compose.home.yaml` puts the host's Desktop, Documents, Downloads,
Pictures, Music and Movies (as Videos) into the account's home, for both
desktops; what the desktop saves there lands on the host. Add it with a
second `-f`, or in `COMPOSE_FILE`. Folders of your own go in
`compose.local.yaml` beside it, in the same form, which git ignores. On
macOS, Docker Desktop and Podman share the Mac's files with whoever reads
them inside, so the account writes them as the Mac's own user; macOS asks
once whether Docker or Podman may open those folders. On a Linux host the
files keep their owners, so the account needs the host user's uid to write
there.

### More software

Packages installed with `apt` inside a container are gone when the container
is recreated. Software that should stay goes into the image, with
`FARLAND_PACKAGES`:

```sh
FARLAND_PACKAGES="code dotnet-sdk-10.0 git build-essential" \
FARLAND_USER=alice FARLAND_PASSWORD=secret \
    docker compose -f packaging/container/compose.yaml up -d --build
```

The package list is the last step of each image, so changing it rebuilds
only that step. Microsoft's repository is set up in the images for VS Code
(`code`); the .NET SDK comes from Ubuntu's archive. The images CI publishes
carry no extra packages. The alternative is Flathub, whose applications
live on a volume (below) and survive a new image.

### What survives a new container

| Volume (GNOME / Plasma) | Mounted at | Holds |
|---|---|---|
| `home` / `plasma-home` | `/home` | the users' files, settings, Firefox profile, and Flatpak apps installed per user |
| `state` / `plasma-state` | `/var/lib/farland` | the TLS certificate, so clients do not ask again, and the NLA credential store |
| `config` / `plasma-config` | `/etc/farland` | `farland.toml`; a new image never overwrites it, as the packages never do |
| `flatpak` / `plasma-flatpak` | `/var/lib/flatpak` | Flatpak apps installed for all users, and the Flathub catalogue, which a service refreshes at every start |
| `accounts` / `plasma-accounts` | `/var/lib/AccountsService` | the users' pictures and languages |
| `systemd` / `plasma-systemd` | `/var/lib/systemd` | systemd's own state; on a volume for Podman's sake (below) |

`docker compose down` keeps the volumes and `down -v` deletes them. The
two desktops have separate volumes, because they keep different settings
in the same places.

### The GPU

`--privileged` shows the container the host's `/dev/dri` as it is. The render
nodes belong to a group of the host's, which the container does not know, so
the entrypoint adds the account to whatever group owns them. Without a seat,
logind grants no device access of its own.

- **Plasma needs a render node.** KWin shares its screen only when it
  composites with OpenGL, and its virtual backend gets OpenGL only through a
  DRM render node. Without one, the client connects, logs in and is
  disconnected at once, and farlandd's journal says
  `KWin refused the screen cast (it needs OpenGL compositing)`. The render
  node does not have to be fast: Mesa's llvmpipe rendering on it is enough.
- **GNOME needs none.** Mutter renders with llvmpipe without a render node,
  and uses the GPU where there is one.
- **H.264 on the GPU:** the images carry no VA-API or NVENC drivers, so
  farland encodes H.264 with OpenH264 on the CPU, or uses Progressive, the
  default. `FARLAND_PACKAGES="mesa-va-drivers"` (AMD) or
  `"intel-media-va-driver"` (Intel) should give VA-API encoding on a Linux
  host; that is untried.

## Docker and Podman

Both run the same images and the same compose file. What differs is the
virtual machine they run in on macOS, and one detail of Podman's storage.

| | Docker Desktop (macOS) | Podman with libkrun (macOS) | Docker or Podman on Linux |
|---|---|---|---|
| GNOME | works | works | should work |
| Plasma | does not work: no render node | works | needs a GPU on the host |
| Render node in the container | none | `renderD128`, a virtio-gpu; Mesa renders with llvmpipe on it | the host's |
| Compose | `docker compose` | `podman compose` | either |

- **The render node.** Docker Desktop's Linux VM has no GPU device, and its
  kernel has no DRM at all, not even vkms or vgem, so no render node can be
  made there; that is why Plasma does not run. Podman's libkrun provider
  (krunkit) gives its VM a virtio-gpu, whose render node is all KWin needs.
  Its Vulkan path to the Mac's GPU (Venus, MoltenVK, Metal) fails with
  `VK_ERROR_OUT_OF_HOST_MEMORY`, so rendering stays on the CPU either way.
- **Podman's overlay file system.** Under Podman, systemd cannot set up a
  service's state directory on the container's own file system
  (`Failed to set up special execution directory in /var/lib: No such
  device`), which leaves logind and accounts-daemon down and every session
  with them. `/var/lib/systemd` and `/var/lib/AccountsService` are on
  volumes in `compose.yaml` for that reason. Docker does not need them.
- **Rootful.** The Podman machine has to be rootful for a privileged systemd
  container.
- **`podman compose`** runs an external compose provider: Docker's
  `docker-compose` where Docker Desktop is installed, `podman-compose`
  otherwise. It starts the containers in the Podman machine either way.
- **Separate engines.** Docker and Podman keep separate images and volumes.
  An image built in one reaches the other with
  `docker save IMAGE | podman load`, or by building or pulling it there.
  Both forward published ports to the Mac's `localhost`, so running GNOME in
  Docker and Plasma in Podman at the same time works: their ports differ.

### Setting up Podman on macOS

At the time of writing, the Homebrew tap `slp/krunkit` has krunkit 1.1.1,
which Podman 6 cannot start: it passes `--timesync`, which krunkit only has
from 1.2 on, and `podman machine start` fails with
`krunkit exited unexpectedly with exit code 2`. krunkit's own release works:

```sh
brew install podman

# krunkit and its libraries, from the release made for Podman
gh release download v1.3.2 -R containers/krunkit -p 'krunkit-podman-unsigned-*.tgz'
mkdir -p ~/.local/opt/krunkit && tar xzf krunkit-podman-unsigned-*.tgz -C ~/.local/opt/krunkit
cd ~/.local/opt/krunkit && chmod -R u+w .
# Its own libraries, not Homebrew's, and a signature with the hypervisor entitlement
install_name_tool -delete_rpath /opt/homebrew/lib -delete_rpath /opt/podman/lib \
    -add_rpath @executable_path/../lib bin/krunkit
cat > /tmp/krunkit.plist <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.hypervisor</key><true/></dict></plist>
EOF
for lib in lib/*.dylib; do codesign --force -s - "$lib"; done
codesign --force -s - --entitlements /tmp/krunkit.plist bin/krunkit

# Where Podman looks for it
mkdir -p ~/.config/containers
cat >> ~/.config/containers/containers.conf <<EOF
[engine]
helper_binaries_dir = ["$HOME/.local/opt/krunkit/bin", "/opt/homebrew/bin", "/opt/homebrew/opt/podman/libexec/podman"]
EOF

podman machine init --provider libkrun --rootful --cpus 6 --memory 10240 farland-gpu
podman machine start farland-gpu
podman machine ssh farland-gpu ls -l /dev/dri      # renderD128
```

Then `podman compose -f packaging/container/compose.yaml up -d plasma`, with
the same variables as above.

## Building the images

```sh
docker build --target gnome -t farland-gnome -f packaging/container/Containerfile.desktop .
docker build --target plasma -t farland-plasma -f packaging/container/Containerfile.desktop .
```

The first stage builds farland's Debian package from the checkout, the way
the README does, so the images carry whatever the working tree holds,
including changes not yet committed. `.dockerignore` (Docker) and
`.containerignore` (Podman) keep build directories and packages out of the
build context. The two targets share everything up to the desktop, so
building both costs one farland build.

## Published images

The `Container images` workflow (`.github/workflows/container-images.yml`)
builds both desktop images for amd64 and arm64, each on a runner of its own
architecture, and publishes them to the GitHub Container Registry:

| Tag | From |
|---|---|
| `ghcr.io/mkenfenheuer/farland-gnome:latest`, `ghcr.io/mkenfenheuer/farland-plasma:latest` | every push to main |
| `…:sha-<commit>` | the same pushes, for a fixed version |
| `…:<tag>` | every tag |

Each tag names both architectures, so the same tag works on an Apple silicon
Mac and on an x86 host. The build cache lives in the registry too, as
`…:buildcache-amd64` and `…:buildcache-arm64`. A package GitHub creates is
private at first; it has to be made public in its settings before anyone
else can pull it.

## When it does not work

- **Logs.** `docker exec farland-gnome-1 journalctl -u farlandd -b` (or
  `farland-plasma-1`, or `podman exec`) shows the connection, the login and
  the session starting. `systemctl --failed` inside the container lists what
  did not start.
- **The client is disconnected right after logging in (Plasma):** no render
  node; see [The GPU](#the-gpu).
- **Every session fails, and `systemctl --failed` lists logind (Podman):**
  a compose file without the `/var/lib/systemd` volume; see
  [Docker and Podman](#docker-and-podman).
- **FreeRDP gives up while the desktop starts:** `/timeout:60000`.
- **The client asks about the certificate after every new container:** the
  `state` volume is missing, so every container makes a new certificate.
