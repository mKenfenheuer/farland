#!/bin/sh
# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0
#
# Writes the commits a compositor fork carries as numbered patch files, for
# whichever distribution's packaging is about to apply them.
#
#   sh packaging/fork-patches.sh FORK_DIR OUT_DIR [BASE_HINT]
#
# The fork is a clone of the compositor with farland's commits on top of an
# upstream release tag. The patches are everything between that tag and the
# branch's HEAD.
#
# BASE_HINT is the release the distribution packages ("50.5", say). It is
# only a hint: where the fork carries that tag the patches come out against
# it exactly, and otherwise they come out against whatever release the
# branch does sit on and the caller applies them to the source it has. That
# is deliberate -- one fork branch serves every distribution for as long as
# the patches still apply, and the distributions are never on the same
# version. A patch that does not apply is the caller's error to report,
# because only the caller knows what it was applying to.
set -eu

fork=${1:?usage: fork-patches.sh FORK_DIR OUT_DIR [BASE_HINT]}
out=${2:?usage: fork-patches.sh FORK_DIR OUT_DIR [BASE_HINT]}
hint=${3:-}

# Not `-d "$fork/.git"`: a submodule's .git is a file holding "gitdir: ...".
if ! git -C "$fork" rev-parse HEAD >/dev/null 2>&1; then
    echo "$fork is not there: git submodule update --init $fork" >&2
    exit 1
fi

# A clone of one branch carries no tags, and the base is one.
if ! git -C "$fork" describe --tags --abbrev=0 HEAD >/dev/null 2>&1; then
    git -C "$fork" fetch --quiet --tags origin 2>/dev/null || true
fi

base=
if [ -n "$hint" ] &&
   git -C "$fork" rev-parse --verify --quiet "refs/tags/$hint" >/dev/null &&
   git -C "$fork" merge-base --is-ancestor "refs/tags/$hint" HEAD; then
    base=$hint
else
    base=$(git -C "$fork" describe --tags --abbrev=0 HEAD 2>/dev/null || true)
fi
if [ -z "$base" ]; then
    echo "$fork has no release tag to take the patches against: fetch its tags" >&2
    exit 1
fi
if [ -n "$hint" ] && [ "$base" != "$hint" ]; then
    echo "note: the fork sits on $base and this distribution packages $hint;" >&2
    echo "      the patches are taken against $base and have to apply anyway" >&2
fi

rm -rf "$out"
mkdir -p "$out"
git -C "$fork" format-patch --no-signature -o "$out" "$base..HEAD" >/dev/null
if [ -z "$(ls -A "$out")" ]; then
    echo "$fork has no commits on top of $base: nothing to add" >&2
    exit 1
fi
echo "$base"
