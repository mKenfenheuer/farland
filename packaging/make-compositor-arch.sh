#!/bin/sh
# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0
#
# Builds Arch mutter or kwin packages carrying what farland's backends need:
# a compositor that goes on drawing a virtual output while the session is not
# active on its seat, so a client can hold the session while the screen at
# the machine shows a login screen.
#
#   sh packaging/make-compositor-arch.sh mutter|kwin [OUTPUT_DIR] [WORK_DIR]
#
# The same commits the Debian and Fedora builds add, put into Arch's own
# PKGBUILD instead: the packages differ from the distribution's only by them.
# Arch is rarely on the same release as either, so the patches are applied to
# whatever Arch packages and the build stops if they no longer fit -- which
# is the signal to rebase the fork.
#
# Needs base-devel, devtools and python, and runs makepkg, which refuses to
# run as root: give it a user that may sudo, as the CI job does. Install what
# it writes with pacman -U ./*.pkg.tar.zst and restart the display manager
# for a session to pick it up.
set -eu

name=${1:?usage: make-compositor-arch.sh mutter|kwin [OUTPUT_DIR] [WORK_DIR]}
case $name in
    mutter | kwin) ;;
    *)
        echo "unknown compositor $name: mutter or kwin" >&2
        exit 1
        ;;
esac
out=${2:-.}
mkdir -p "$out"
out=$(CDPATH= cd -- "$out" && pwd)
work=${3:-build-$name-arch}
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fork=$source_dir/packaging/$name

mkdir -p "$work"
work=$(CDPATH= cd -- "$work" && pwd)
rm -rf "$work/pkg"
mkdir -p "$work/pkg"

# Arch's own packaging, from the package's git repository.
(cd "$work/pkg" && pkgctl repo clone --protocol=https "$name" >/dev/null)
tree=$work/pkg/$name
if [ ! -f "$tree/PKGBUILD" ]; then
    echo "no PKGBUILD for $name: is devtools installed?" >&2
    exit 1
fi

version=$(cd "$tree" && . ./PKGBUILD && echo "$pkgver")
echo "Arch packages $name $version"

# The fork's commits, against whatever release its branch sits on.
base=$(sh "$source_dir/packaging/fork-patches.sh" "$fork" "$work/patches" "$version")
echo "adding the farland patches, taken against $base"

names=
for patch in "$work/patches"/*.patch; do
    file=farland-$(basename "$patch")
    cp "$patch" "$tree/$file"
    names="$names $file"
done

# A pkgrel of its own, above Arch's, so these are not quietly replaced by the
# distribution's own build of the same version. It has to read as
# 'integer[.integer]' -- makepkg refuses anything else -- so the mark is the
# extra component rather than a word: `pacman -Qi` shows 50.5-1.1 where the
# distribution's own says 50.5-1.
suffix=${COMPOSITOR_ARCH_SUFFIX:-.1}

# shellcheck disable=SC2086 # one argument per patch
python3 "$source_dir/packaging/patch-pkgbuild.py" "$tree/PKGBUILD" "$suffix" $names

echo "building $version-$( (cd "$tree" && . ./PKGBUILD && echo "$pkgrel") )"
(cd "$tree" && makepkg --syncdeps --noconfirm --needed --nocheck --skipinteg)

find "$tree" -maxdepth 1 -name '*.pkg.tar.*' -exec cp {} "$out/" \;
echo "packages in $out:"
ls -1 "$out"/*.pkg.tar.*
