#!/bin/sh
# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0
#
# Builds the RPM from this checkout, the way make-deb.sh builds the deb:
#
#   sh packaging/make-rpm.sh [OUTPUT_DIR]
#
# Needs rpm-build and the build dependencies in packaging/farland.spec
# (ci/install-deps.sh installs them on Fedora). It makes a tarball of the
# working tree -- not of HEAD, so an uncommitted change is packaged too --
# and hands it to rpmbuild in a private topdir.
set -eu

out=${1:-.}
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
spec="$source_dir/packaging/farland.spec"

version=$(sed -n 's/^Version: *//p' "$spec")
name=farland

top=$(mktemp -d)
trap 'rm -rf "$top"' EXIT INT TERM
mkdir -p "$top/SOURCES" "$top/SPECS"

# The tarball rpmbuild unpacks: one directory named name-version, without
# the build directories or the git history. subprojects/ goes in as it is:
# with the wraps already fetched the build needs no network, and without
# them meson fetches them, which is what a fresh checkout does too. What
# must not happen is a subproject directory that exists but is empty --
# meson then neither builds nor extracts it.
stage="$top/$name-$version"
mkdir -p "$stage"
tar -C "$source_dir" --exclude=.git --exclude='./build*' \
    --exclude='*.deb' --exclude='*.rpm' -cf - . | tar -C "$stage" -xf -
tar -C "$top" -czf "$top/SOURCES/$name-$version.tar.gz" "$name-$version"
cp "$spec" "$top/SPECS/"

rpmbuild --define "_topdir $top" -ba "$top/SPECS/$(basename "$spec")" >"$top/build.log" 2>&1 || {
    tail -40 "$top/build.log" >&2
    exit 1
}

mkdir -p "$out"
package=$(find "$top/RPMS" -name '*.rpm' | head -1)
cp "$package" "$out/"
echo "$out/$(basename "$package")"
