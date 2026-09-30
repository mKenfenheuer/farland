#!/bin/sh
# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0
#
# The snapshot part of a build's version, r<commits>.g<commit>, as the AUR
# recipe's pkgver() has it: every build of main gets a version of its own,
# and a later build a higher one, so package managers take it as an upgrade.
# Prints nothing for a tag, which is a release and keeps meson.build's
# version as it is.
#
#   FARLAND_SNAPSHOT=$(sh ci/snapshot-version.sh) sh packaging/make-deb.sh ...
#
# Needs the whole history (actions/checkout with fetch-depth: 0): a shallow
# clone counts one commit.
set -eu

[ "${GITHUB_REF_TYPE:-branch}" = tag ] && exit 0
# The checkout can belong to another user than a job's container runs as.
git() { command git -c safe.directory='*' "$@"; }
printf 'r%s.g%s\n' "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
