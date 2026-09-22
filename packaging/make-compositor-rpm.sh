#!/bin/sh
# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0
#
# Builds Fedora mutter or kwin packages carrying what farland's backends
# need: a compositor that goes on drawing a virtual output while the session
# is not active on its seat, so a client can hold the session while the
# screen at the machine shows a login screen.
#
#   sh packaging/make-compositor-rpm.sh mutter|kwin [OUTPUT_DIR] [WORK_DIR]
#
# The same commits the Debian build adds (packaging/make-mutter-deb.sh and
# make-kwin-deb.sh), put into Fedora's own spec instead: the packages differ
# from the distribution's only by them. Fedora is rarely on the same release
# as Debian, so the patches are applied to whatever Fedora packages and the
# build stops if they no longer fit -- which is the signal to rebase the fork.
#
# Needs rpm-build, rpmdevtools and the spec's build dependencies
# (dnf builddep NAME). Install what it writes with dnf install ./*.rpm and
# restart the display manager for a session to pick it up.
set -eu

name=${1:?usage: make-compositor-rpm.sh mutter|kwin [OUTPUT_DIR] [WORK_DIR]}
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
work=${3:-build-$name-rpm}
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fork=$source_dir/packaging/$name

mkdir -p "$work"
work=$(CDPATH= cd -- "$work" && pwd)
rm -rf "$work/rpmbuild"
mkdir -p "$work/rpmbuild"

# Fedora's own source package.
(cd "$work" && dnf download --source "$name" >/dev/null)
srpm=$(find "$work" -maxdepth 1 -name "$name-*.src.rpm" | head -n 1)
if [ -z "$srpm" ]; then
    echo "no $name source package: is a source repository enabled?" >&2
    exit 1
fi
version=$(rpm -q --qf '%{version}' -p "$srpm")
echo "Fedora packages $name $version"

rpm -i --define "_topdir $work/rpmbuild" "$srpm"
spec=$work/rpmbuild/SPECS/$name.spec
if [ ! -f "$spec" ]; then
    echo "the source package has no $name.spec" >&2
    exit 1
fi

# The fork's commits, against whatever release its branch sits on.
base=$(sh "$source_dir/packaging/fork-patches.sh" "$fork" "$work/patches" "$version")
echo "adding the farland patches, taken against $base"

for patch in "$work/patches"/*.patch; do
    cp "$patch" "$work/rpmbuild/SOURCES/farland-$(basename "$patch")"
done

# Fedora's spec numbers its patches; farland's go after the last of them, so
# the distribution's own still apply to the source they were made for.
python3 - "$spec" "$work/patches" <<'PY'
import glob, os, re, sys

spec_path, patch_dir = sys.argv[1], sys.argv[2]
with open(spec_path, encoding='utf-8', errors='surrogateescape') as f:
    lines = f.read().split('\n')

patches = sorted(os.path.basename(p) for p in glob.glob(os.path.join(patch_dir, '*.patch')))
numbers = [int(m.group(1)) for line in lines
           if (m := re.match(r'^Patch(\d+)\s*:', line))]
nxt = max(numbers) + 1 if numbers else 0

# After the last Patch:, else after the last Source:, else before %description.
anchor = None
for i, line in enumerate(lines):
    if re.match(r'^Patch\d*\s*:', line) or re.match(r'^Source\d*\s*:', line):
        anchor = i
if anchor is None:
    for i, line in enumerate(lines):
        if line.startswith('%description'):
            anchor = i - 1
            break
if anchor is None:
    sys.exit('cannot see where to add the patches in the spec')

added = []
for name in patches:
    added.append(f'Patch{nxt}:         farland-{name}')
    nxt += 1
lines[anchor + 1:anchor + 1] = added
with open(spec_path, 'w', encoding='utf-8', errors='surrogateescape') as f:
    f.write('\n'.join(lines))
print('\n'.join(added))
PY

# A release of its own, above Fedora's, so these are not quietly replaced by
# the distribution's own build of the same version and `rpm -q` says which
# compositor a machine runs.
suffix=${COMPOSITOR_RPM_SUFFIX:-.farland1}
sed -i "0,/^Release:/s/^\\(Release:[[:space:]]*\\)\\(.*\\)$/\\1\\2$suffix/" "$spec"
echo "building $(rpm -q --qf '%{version}-%{release}' --specfile "$spec" | head -n 1)"

rpmbuild --define "_topdir $work/rpmbuild" -bb --nocheck "$spec"

find "$work/rpmbuild/RPMS" -name '*.rpm' -exec cp {} "$out/" \;
echo "packages in $out:"
ls -1 "$out"/*.rpm
