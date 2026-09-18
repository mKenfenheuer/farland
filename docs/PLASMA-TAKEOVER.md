<!--
SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
SPDX-License-Identifier: Apache-2.0
-->

# Taking over a local Plasma session

With `on_local_session = "attach"`, a client takes the session a person is
using at the machine: it gets virtual outputs of its own, the screen at the
machine shows the display manager's login screen while the client holds the
session, and logging in there takes the session back. This is what GNOME
does, and Plasma now does the same.

This was written after the Plasma side was built, from what was measured on
the test machine, not from what the code looks like it should do. The
measurements are in it because two of them went against the reading of the
source.

## Where things stand

| | GNOME | Plasma |
| --- | --- | --- |
| What the client sees | a virtual monitor per client monitor | a virtual output per client monitor |
| Screen at the machine while a client holds it | a login screen (GDM) | a login screen (SDDM, LightDM) |
| Client monitor size | the desktop resizes to it | the desktop resizes to it |
| Where the panel and new windows go | the client's first monitor | the client's first screen |
| Giving the session back on disconnect | it goes back whole | it goes back whole |
| Taking it back at the machine | logging in ends the connection | logging in ends the connection |
| Asking before a client takes it | yes (farlandd) | yes (farlandd) |
| Asking before the seat takes it back | yes (PAM module) | yes (PAM module) |
| Compositor change needed | a Mutter patch (`packaging/mutter`) | a KWin patch (`packaging/kwin`), for reconnecting only |

The last two rows of the consent pair are backend-agnostic, and the seat
watching now is too: `SeatWatch`, `switch_seat_to_greeter()` and
`activate_user_session()` live in `src/farland/platform/logind/`, which both
backends use. `switch_seat_to_greeter()` tries GDM's
`CreateTransientDisplay` and then the freedesktop display manager API that
SDDM and LightDM implement (`Seat.SwitchToGreeter`), so it works whichever
display manager runs.

## How it works

`PlasmaHeadlessDesktop` (`apps/farland-server/plasma_headless.cpp`) has two
ways of attaching, and takes the second whenever KWin offers it:

- **Mirroring**, as before: it streams the outputs KWin already has
  (`stream_output`). The client and the screen at the machine show the same
  picture. This is what a KWin older than `zkde_screencast_unstable_v1`
  version 2 gets, and what a launched (non-attached) session uses, where
  KWin's own outputs are the client's anyway.
- **Virtual outputs**, when attached: one virtual output per client monitor
  (`stream_virtual_output_with_description`), added and removed as the
  client's monitors change (`Desktop::screens_follow_monitors()`). The seat's
  own screen is never streamed, so a client never sees it and a person
  walking past the machine never sees the client's.

`src/farland/platform/kwin/screencast.cpp` binds the version KWin offers, up
to the 5 the vendored protocol describes (KWin 6.6 offers 6). KWin puts its
own `Virtual-` in front of the name it is given, so the output farland asks
for as `farland-0` arrives as `Virtual-farland-0`; that is the name the
libei region carries as well, so the desktop looks the wl_output up and uses
whatever KWin ended up calling it.

Sizes go through `kde_output_management_v2` as before: KWin gives a virtual
output custom modes, so `OutputManagement::request_size()` resizes it, and
the client's monitor size is what the screen ends up being. KWin refuses a
configuration while its set of outputs is changing — which is exactly when a
client adds or removes a monitor — so a refusal is now tried again a few
times before it counts (`OutputManagement`, `max_refusals`).

`OutputManagement::request_layout()` lays the session out around the
client's screens: they sit side by side beside the seat's, and the first of
them is the primary one, so that the panel, new windows and the overview are
where the client looks. It gives the whole set of enabled outputs a priority
— a configuration that gives only some of them one leaves KWin's own order
in place, and the panel stays where it was. `restore_layout()` puts back what
was there before, and the desktop's destructor calls it before its virtual
outputs go.

The seat's own screen is **not** switched off, unlike GNOME's. Two reasons,
both measured: KWin refuses every output configuration while the session is
not active on its seat, so a screen switched off on the way in could not be
switched on again on the way out; and KWin remembers the configurations it
applies. The login screen is what keeps the screen at the machine from
showing the session, and that is enough.

## What was measured, and what it changed

### KWin keeps drawing a virtual output off the seat

This is the question the whole GNOME job turned on, and on Plasma the answer
is yes, which is why there is no KWin patch for it.

`tests/platform/kwin/plasma_headless_probe.cpp` (`farland-plasma-headless-probe`,
the twin of the GNOME one) holds the desktop as a client would and prints
the frames each screen gives per second. Something has to be animating, or
an idle desktop gives 0 fps and proves nothing — a Konsole running
`while :; do printf "%s " $RANDOM; sleep 0.03; done` does it.

**It has to be animating _on the virtual output_.** This is the trap: with
the window on the seat's own screen, the frames go 33 → 0 the instant the
seat switches, which reads exactly like KWin pausing the virtual output.
It is not: a window on the seat's screen stops being drawn because that
output's render loop is inhibited, and it stops committing, so there is
nothing to cast. Move the window across first
(`qdbus6 org.kde.kglobalaccel /component/kwin org.kde.kglobalaccel.Component.invokeShortcut "Window to Next Screen"`),
wait until the counter shows frames (`--baseline SECONDS`), and only then
hand the seat over. Measured that way:

```
19 fps for 20 s with the session on its seat
a client takes the desktop (the seat gets a login screen)
25–27 fps for the next 30 s
```

It goes *up*, because KWin no longer draws the seat's screen. The KWin
source says the same, once you know where to look: losing the seat is
`Session::devicePaused` → `DrmGpu::setActive(false)`, which inhibits the
render loops of `m_drmOutputs` only, and a `DrmVirtualOutput` is owned by
`DrmBackend::m_virtualOutputs` and presents through a software vsync
monitor, with no KMS commit and no DRM master.

### KWin will not *make* one off the seat, and that needed the patch

A client that lets go leaves the login screen on the seat, so the session is
off its seat when the next client arrives. There, creating the virtual
output fails: `KWin cannot cast the virtual output farland-0: Could not
find output`.

`DrmBackend::applyOutputChanges()` tests the outputs on the seat before it
applies anything, that test needs DRM master, and off the seat there is
none: `Atomic modeset test failed! Permission denied`, the whole
configuration is refused, `Workspace::updateOutputs()` never runs, and the
virtual output never becomes one KWin shows — although nothing about it
needs master. Without the fix, a session can only be taken while the screen
at the machine still shows it.

Bringing the session back to its seat first is not a way out: logind's
`Activate` and `Seat.SwitchTo` both want polkit admin authentication from a
session that is not the active one, so an agent cannot do it
(`Failed to issue method call: Access denied`).

`packaging/kwin` is a fork of KWin, branch
`farland/virtual-outputs-off-seat-6.6.6`, with one commit: while the session
is inactive, apply the configuration to the virtual outputs and leave the
outputs on the seat exactly as they are. `packaging/make-kwin-deb.sh` builds
distribution packages from the distribution's own source plus that commit,
the way `packaging/make-mutter-deb.sh` does for Mutter.

It is a smaller thing than the Mutter patch and a different kind: no
protocol addition, nothing to opt into, and no behaviour change on the seat,
because it only stops a configuration that cannot touch the seat from being
thrown away with one that can. Upstream first.

### A refused login hands the seat back, and KWin resizes on the way

[policy] seat_takeover refuses a login at the machine, and the display
manager then gives up the login screen it was showing — which puts the
session back on its seat although nobody was let in. The agent puts a login
screen back (`keep_the_seat_after_a_refusal()`), and the client keeps the
session. What that costs on Plasma, measured on the test machine:

```
a client holds the session; the seat shows SDDM's login screen
a login at the machine is refused, and the seat falls back to the session
KWin resizes the virtual output to its own 1024x768 and lays the session
  out for the seat's screen
the login screen has the seat again
the client's screen comes back at 1280x800
```

The sizes have to go back afterwards, and only afterwards. A configuration
in the moment of the return is refused whole — `Atomic modeset test failed!
Permission denied`, because KWin is back on the seat without DRM master yet
— and one sent immediately after the login screen returns does nothing
either: KWin has not described what it did to the outputs yet, so asking for
the size it already believes we want changes nothing (`request_size()` has
nothing to do). It waits for KWin's word on the outputs and then asks again
(`catch_up_after_a_refusal()`), which is the only window that works, because
off the seat a configuration reaches the virtual outputs alone
(`packaging/kwin`).

GNOME needs none of this: Mutter keeps the session laid out for the client's
monitors across the return, and the layout check after it applies nothing.

## The test machine

`max@10.1.250.18` (`kubuntu-vm`), Kubuntu 26.04, KWin 6.6.6, SDDM, one seat
with a Plasma session on tty3. Root over ssh as well. Proxmox shows the
console, which is the only way to see what the screen at the machine shows —
nothing on the box can screenshot it.

Useful things:

- **Run the probe against the running session**: it needs the session's
  socket and bus, and a desktop file naming it for
  `X-KDE-Wayland-Interfaces` (KWin grants screen casting by executable):

  ```sh
  cat > ~/.local/share/applications/farland-plasma-probe.desktop <<'EOF'
  [Desktop Entry]
  Type=Application
  Name=farland plasma probe
  Exec=/home/max/farland-main/build-gcc/tests/platform/kwin/farland-plasma-headless-probe
  NoDisplay=true
  X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1
  EOF
  export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 \
         DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
  env -u XDG_SESSION_ID farland-plasma-headless-probe --attach --hold 30 --baseline 20 1280x800
  ```

  `env -u XDG_SESSION_ID` matters: over ssh that names the *ssh* session, and
  the seat watch would watch the wrong one and never see the seat change. An
  agent farlandd starts inherits the graphical session's id and needs none of
  this.
- **Put the seat back** after a test: `chvt 3` as root, which is what logging
  in at the greeter does as far as the session is concerned.
- **Drive a client without a person**: `xvfb-run -a xfreerdp3 /v:127.0.0.1
  /u:<user> /p:<password> /cert:ignore /size:1280x800` in the background,
  and `pkill -x xfreerdp3` to end it — killing `xvfb-run` leaves the client
  running and farland never sees the disconnect.
- **A login farland accepts**: the credential store
  (`/var/lib/farland/users`) takes `user:domain:NT hash:local account`, and
  an entry whose local account is the one logged in at the seat gets you an
  attached session without knowing anybody's password. The NT hash is MD4 of
  the UTF-16LE password; OpenSSL and Python have both dropped MD4, so
  compute it yourself. Take the entry out again afterwards.
- **Exercise the PAM stack without the greeter**: a few lines calling
  `pam_start("sddm", user, ...)` and `pam_acct_mgmt()` show what somebody
  logging in at the machine would get, including the messages. There is a
  copy at `/tmp/pamcheck` on both test machines.
- **See the outputs**: `kscreen-doctor -o`, from inside the session (it
  needs the session's Wayland socket, so it says nothing over plain ssh). It
  shows priorities, which is how to check the layout took.
- **See what KWin says**: `journalctl --user -u plasma-kwin_wayland.service`.
  `Failed to open /dev/dri/renderD128 device (No such device)` there is
  noise: KWin sees every drm uevent, tries `TakeDevice` on the render node,
  and logind refuses because it is not tagged to a seat. Nothing comes of it.

## Traps that cost time

- **An animating window has to be on the virtual output**, or the frame
  counter measures the seat's screen and says the opposite of the truth.
  See above; this one cost a wrong conclusion, not just time.
- **`rsync -a` keeps mtimes**, so ninja on the test machine decides the
  build is current and skips it. A header change then reaches the machine
  without being compiled in, and a virtual call quietly goes to the wrong
  place. Sync with `--no-times`, and when a change "has no effect", check
  that it was linked before debugging the logic.
- **The package puts `farlandd` in `/usr/sbin` and the agent in
  `/usr/bin`**, and a locally built `farlandd` looks for the agent under
  `/usr/local/bin`. Copying a fresh build over the wrong one wastes an
  afternoon: check `readlink -f /proc/$(pgrep -x farlandd)/exe`.
- **`common-account` ends with `account sufficient pam_localuser.so`**,
  which finishes the stack for a local user, so a PAM line after
  `@include common-account` never runs. It goes before the include.
- **An idle desktop produces no frames at all**, so "0 fps" proves nothing
  unless something is animating.
