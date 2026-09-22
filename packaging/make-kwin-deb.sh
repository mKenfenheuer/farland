#!/bin/sh
# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0
#
# Builds Debian/Ubuntu kwin packages carrying the fix farland's Plasma
# backend needs: KWin goes on configuring its virtual outputs while the
# session is not active on its seat, so a client can take a session whose
# seat shows a login screen — and, having let go, take it again.
#
#   sh packaging/make-kwin-deb.sh [OUTPUT_DIR] [WORK_DIR]
#
# The fix lives in the fork in packaging/kwin, as commits on top of the
# upstream release tag. This takes the distribution's own kwin source package
# and adds those commits to its patch series, so the packages differ from the
# distribution's only by this fix. Once it is in upstream KWin and the
# distribution ships it, this script and the submodule go.
#
# Needs dpkg-dev, quilt and a deb-src line for the distribution's kwin, and
# the build dependencies (apt-get build-dep kwin). Install the packages it
# writes with apt-get install ./kwin-wayland*.deb ./kwin-common*.deb ..., and
# restart the display manager for a session to pick them up.
#
# What the fix is, and why it is not the same shape as the mutter one:
# KWin already keeps *drawing* a virtual output while the session is off its
# seat (measured on 6.6.6: a screen cast of one goes on at its full frame
# rate, and only the seat's own outputs stop), so nothing has to be opted
# into and nothing changes on the seat. What it refuses off the seat is every
# output *configuration*, because the outputs on the seat cannot be committed
# without DRM master — and a virtual output that is never configured never
# becomes one the compositor shows. The fix applies such a configuration to
# the virtual outputs and leaves the seat's outputs alone, which is why it is
# a single commit with no protocol addition and no switch to turn on.
set -eu

out=${1:-.}
mkdir -p "$out"
out=$(CDPATH= cd -- "$out" && pwd)
work=${2:-build-kwin-deb}
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fork=$source_dir/packaging/kwin

# Not `-d "$fork/.git"`: a submodule's .git is a *file* holding "gitdir:
# ...", so that test fails on every correctly checked-out submodule. Ask git
# instead, which answers for either layout and proves the fork is usable.
if ! git -C "$fork" rev-parse HEAD >/dev/null 2>&1; then
    echo "packaging/kwin is not there: git submodule update --init packaging/kwin" >&2
    exit 1
fi

mkdir -p "$work"
work=$(CDPATH= cd -- "$work" && pwd)
rm -rf "$work/source"
mkdir -p "$work/source"

# The distribution's source package, with its own patches applied.
(cd "$work/source" && apt-get source kwin)
tree=$(find "$work/source" -maxdepth 1 -name 'kwin-*' -type d | head -n 1)
if [ -z "$tree" ]; then
    echo "no kwin source package: is there a deb-src line for it?" >&2
    exit 1
fi

# The release the distribution packages, as a hint for which tag the
# fork's commits should come against. Where the fork sits on another
# release the patches still have to apply to this source, and quilt
# below says so if they do not.
version=$(dpkg-parsechangelog -l"$tree/debian/changelog" -SVersion)
upstream=${version%%-*}
upstream=${upstream#*:}
base=$(sh "$source_dir/packaging/fork-patches.sh" "$fork" "$work/patches" "v$upstream")
echo "adding the farland patches, taken against $base"

for patch in "$work/patches"/*.patch; do
    name=farland-$(basename "$patch")
    cp "$patch" "$tree/debian/patches/$name"
    echo "$name" >> "$tree/debian/patches/series"
    echo "added $name"
done

(cd "$tree" && QUILT_PATCHES=debian/patches quilt push -a)

# A version of its own, above the distribution's, so that these packages are
# not quietly replaced by the distribution's own build of the same version,
# and so that `apt policy` shows which kwin a machine runs.
suffix=${KWIN_DEB_SUFFIX:-+farland1}
distribution=$(dpkg-parsechangelog -l"$tree/debian/changelog" -SDistribution)
cat > "$tree/debian/changelog.farland" <<EOF
kwin ($version$suffix) $distribution; urgency=medium

  * Add the farland patches from packaging/kwin: KWin goes on configuring
    its virtual outputs while the session is not active on its seat, so a
    screen cast client can be given one while the seat shows a login screen.

 -- $(git -C "$fork" log -1 --format='%an <%ae>')  $(date -R)

EOF
cat "$tree/debian/changelog" >> "$tree/debian/changelog.farland"
mv "$tree/debian/changelog.farland" "$tree/debian/changelog"
echo "building $version$suffix"

(cd "$tree" && DEB_BUILD_OPTIONS="nocheck ${DEB_BUILD_OPTIONS:-}" dpkg-buildpackage -b -uc -us)

for deb in "$work/source"/*.deb; do
    cp "$deb" "$out/"
done
echo "packages in $out:"
ls -1 "$out"/*.deb
