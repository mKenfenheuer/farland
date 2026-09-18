#!/bin/sh
# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0
#
# Builds the Debian/Ubuntu package from a meson build directory: farlandd
# with its systemd service for headless multi-user sessions, farland-server,
# farland-agent and farlandctl.
#
#   sh packaging/make-deb.sh [BUILD_DIR] [OUTPUT_DIR]
#
# Needs dpkg-dev. The build directory is configured (or reconfigured) for
# /usr with /etc, so the package lands where Debian expects it.
set -eu

build=${1:-build-deb}
out=${2:-.}
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
maintainer=${DEB_MAINTAINER:-'Maximilian Kenfenheuer <maximilian.kenfenheuer@ksol.it>'}

if [ -f "$build/build.ninja" ]; then
    meson configure "$build" --prefix=/usr --sysconfdir=/etc --buildtype=release >/dev/null
else
    meson setup "$build" "$source_dir" --prefix=/usr --sysconfdir=/etc --buildtype=release -Dwerror=true >/dev/null
fi
meson compile -C "$build"

version=$(meson introspect --projectinfo "$build" |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])')
revision=${DEB_REVISION:-1}
arch=$(dpkg --print-architecture)
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT INT TERM

DESTDIR="$stage" meson install -C "$build" --quiet
# The daemon reads /etc/farland/farland.toml; meson installs it as an
# example, because a plain `meson install` must not overwrite a live one.
install -D -m 0644 "$source_dir/data/farland.toml" "$stage/etc/farland/farland.toml"

# Only farland's own files: a build with tests also installs Catch2's
# headers, library and pkg-config files from the subproject.
keep="usr/bin/farland-agent usr/bin/farland-server usr/bin/farlandctl usr/sbin/farlandd"
find "$stage/usr" -type f | while read -r file; do
    path=${file#"$stage/"}
    case " $keep " in
    *" $path "*) continue ;;
    esac
    case $path in
    usr/lib/systemd/* | usr/share/dbus-1/* | usr/share/polkit-1/* | usr/share/doc/farland/*) continue ;;
    # The PAM module that asks a client before somebody at the machine
    # takes the session back; it does nothing until a PAM service names it
    # (packaging/pam/).
    */security/pam_farland.so) continue ;;
    # KWin reads these to grant screen casting when attaching to Plasma.
    usr/share/applications/org.farland.*) continue ;;
    esac
    rm -f "$file"
done
find "$stage/usr" -type d -empty -delete

mkdir -p "$stage/DEBIAN"
# Dependencies from the libraries the binaries really link.
mkdir -p "$stage/debian"
printf 'Source: farland\nMaintainer: %s\n' "$maintainer" > "$stage/debian/control"
binaries=$(find "$stage/usr" -type f -perm -u+x -exec sh -c 'file -b "$1" | grep -q ELF' _ {} \; -print)
depends=$(cd "$stage" && dpkg-shlibdeps -O --ignore-missing-info $binaries 2>/dev/null |
    sed -n 's/^shlibs:Depends=//p')
rm -rf "$stage/debian"
[ -n "$depends" ] || depends='libc6'

find "$stage/etc" -type f | sed "s|^$stage||" > "$stage/DEBIAN/conffiles"
size=$(du -ks "$stage" | cut -f1)

cat > "$stage/DEBIAN/control" <<EOF
Package: farland
Version: $version-$revision
Architecture: $arch
Maintainer: $maintainer
Section: net
Priority: optional
Homepage: https://github.com/kenfenheuer/farland
Installed-Size: $size
Depends: $depends
Recommends: xdg-desktop-portal, pipewire
Description: RDP server for Linux and Wayland, with headless multi-session
 farland serves the Remote Desktop Protocol on Wayland desktops: NLA, the
 Graphics Pipeline with RemoteFX Progressive, ClearCodec and H.264 (VA-API,
 NVENC or OpenH264), clipboard, audio, multi-monitor, resizing, and touch.
 .
 The farlandd service gives each user their own headless desktop over one
 port: GNOME through GDM, or Plasma, sway, labwc and cage through farland's
 own PAM and logind sessions. Users reconnect into the session they left.
 Configure it in /etc/farland/farland.toml and enrol users with farlandctl.
EOF

cat > "$stage/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ]; then
    mkdir -p /var/lib/farland
    chmod 0700 /var/lib/farland
    if [ -d /run/systemd/system ]; then
        systemctl daemon-reload || true
        # The service is the point of the package: headless multi-user
        # sessions on port 3389. Enrol users with farlandctl.
        systemctl enable --now farlandd.service || true
    fi
fi
EOF

cat > "$stage/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = remove ] && [ -d /run/systemd/system ]; then
    systemctl stop farlandd.service || true
    systemctl disable farlandd.service || true
fi
EOF

cat > "$stage/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
[ ! -d /run/systemd/system ] || systemctl daemon-reload || true
# The certificate, the credential store and the sessions' state.
[ "$1" != purge ] || rm -rf /var/lib/farland
EOF

chmod 0755 "$stage/DEBIAN/postinst" "$stage/DEBIAN/prerm" "$stage/DEBIAN/postrm"
mkdir -p "$out"
package="$out/farland_${version}-${revision}_${arch}.deb"
dpkg-deb --root-owner-group --build "$stage" "$package" >/dev/null
echo "$package"
