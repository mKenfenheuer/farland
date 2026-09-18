#!/bin/sh
# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0
#
# Builds Debian/Ubuntu mutter packages carrying the feature farland's GNOME
# backend needs: a virtual monitor that Mutter keeps drawing while the
# session is not active on its seat, so a client holds the session while the
# screen at the machine shows a login screen
# (RecordVirtual's "keep-rendering-when-inactive", ScreenCast version 5).
#
#   sh packaging/make-mutter-deb.sh [OUTPUT_DIR] [WORK_DIR]
#
# The feature lives in the fork in packaging/mutter, as commits on top of the
# upstream release tag. This takes the distribution's own mutter source
# package and adds those commits to its patch series, so the packages differ
# from the distribution's only by this feature. Once it is in upstream Mutter
# and the distribution ships it, this script and the submodule go.
#
# Needs dpkg-dev, quilt and a deb-src line for the distribution's mutter, and
# the build dependencies (apt-get build-dep mutter). Install the packages it
# writes with apt-get install ./libmutter-*.deb ./mutter-common*.deb, and
# restart the display manager for a session to pick them up.
set -eu

out=${1:-.}
mkdir -p "$out"
out=$(CDPATH= cd -- "$out" && pwd)
work=${2:-build-mutter-deb}
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fork=$source_dir/packaging/mutter

if [ ! -d "$fork/.git" ]; then
    echo "packaging/mutter is not there: git submodule update --init packaging/mutter" >&2
    exit 1
fi

mkdir -p "$work"
work=$(CDPATH= cd -- "$work" && pwd)
rm -rf "$work/source"
mkdir -p "$work/source"

# The distribution's source package, with its own patches applied.
(cd "$work/source" && apt-get source mutter)
tree=$(find "$work/source" -maxdepth 1 -name 'mutter-*' -type d | head -n 1)
if [ -z "$tree" ]; then
    echo "no mutter source package: is there a deb-src line for it?" >&2
    exit 1
fi

# The upstream release the distribution packages, which is the release the
# fork's commits sit on: anything else would apply to the wrong Mutter.
version=$(dpkg-parsechangelog -l"$tree/debian/changelog" -SVersion)
upstream=${version%%-*}
upstream=${upstream#*:}
if ! git -C "$fork" rev-parse --verify --quiet "refs/tags/$upstream" >/dev/null; then
    # A clone of one branch has no tags; the fork carries the release its
    # commits sit on.
    git -C "$fork" fetch --quiet origin "refs/tags/$upstream:refs/tags/$upstream" 2>/dev/null || true
fi
if ! git -C "$fork" rev-parse --verify --quiet "refs/tags/$upstream" >/dev/null; then
    echo "the fork has no tag $upstream: it is not the release this distribution packages" >&2
    exit 1
fi
if ! git -C "$fork" merge-base --is-ancestor "refs/tags/$upstream" HEAD; then
    echo "the fork's branch is not based on $upstream, which this distribution packages" >&2
    exit 1
fi

rm -rf "$work/patches"
mkdir -p "$work/patches"
git -C "$fork" format-patch --no-signature -o "$work/patches" "$upstream..HEAD" >/dev/null
if [ -z "$(ls -A "$work/patches")" ]; then
    echo "the fork has no commits on top of $upstream: nothing to add" >&2
    exit 1
fi

for patch in "$work/patches"/*.patch; do
    name=farland-$(basename "$patch")
    cp "$patch" "$tree/debian/patches/$name"
    echo "$name" >> "$tree/debian/patches/series"
    echo "added $name"
done

(cd "$tree" && QUILT_PATCHES=debian/patches quilt push -a)

# A version of its own, above the distribution's, so that these packages are
# not quietly replaced by the distribution's own build of the same version,
# and so that `apt policy` shows which mutter a machine runs.
suffix=${MUTTER_DEB_SUFFIX:-+farland1}
distribution=$(dpkg-parsechangelog -l"$tree/debian/changelog" -SDistribution)
cat > "$tree/debian/changelog.farland" <<EOF
mutter ($version$suffix) $distribution; urgency=medium

  * Add the farland patches from packaging/mutter: a virtual monitor that
    Mutter keeps drawing while the session is not active on its seat
    (RecordVirtual's "keep-rendering-when-inactive", ScreenCast version 5).

 -- $(git -C "$fork" log -1 --format='%an <%ae>') $(date -R)

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
