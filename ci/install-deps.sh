#!/bin/sh
# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0
#
# Installs the build dependencies inside a CI container (run as root).
# Catch2 is not installed on purpose: the build falls back to the wrap, so every
# distribution tests against the same Catch2 version.
set -eu

. /etc/os-release
case "$ID" in
debian | ubuntu)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends ca-certificates git meson ninja-build pkg-config g++ clang libssl-dev
    # Ubuntu 24.04's default clang is 18, which cannot use libstdc++'s
    # std::expected; its clang-19 package can.
    if [ "$ID" = ubuntu ] && [ "${VERSION_ID%%.*}" -lt 25 ]; then
        apt-get install -y --no-install-recommends clang-19
        ln -sf /usr/bin/clang-19 /usr/local/bin/clang
        ln -sf /usr/bin/clang++-19 /usr/local/bin/clang++
    fi
    ;;
fedora)
    dnf install -y git meson ninja-build pkgconf-pkg-config gcc-c++ clang openssl-devel
    ;;
arch)
    pacman -Syu --noconfirm --needed git meson ninja pkgconf gcc clang openssl
    ;;
*)
    echo "unsupported distribution: $ID" >&2
    exit 1
    ;;
esac
