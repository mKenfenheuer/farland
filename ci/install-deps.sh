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
    apt-get install -y --no-install-recommends ca-certificates git meson ninja-build pkg-config g++ clang libssl-dev \
        libsystemd-dev libei-dev libeis-dev libpipewire-0.3-dev libspa-0.2-dev libgbm-dev libegl-dev libva-dev libwayland-dev libpam0g-dev \
        dbus python3-dbus python3-gi pipewire libwayland-dev libxkbcommon-dev wayland-protocols
    # Ubuntu 24.04's default clang is 18, which cannot use libstdc++'s
    # std::expected; its clang-19 package can.
    if [ "$ID" = ubuntu ] && [ "${VERSION_ID%%.*}" -lt 25 ]; then
        apt-get install -y --no-install-recommends clang-19
        ln -sf /usr/bin/clang-19 /usr/local/bin/clang
        ln -sf /usr/bin/clang++-19 /usr/local/bin/clang++
    fi
    ;;
fedora)
    dnf install -y git meson ninja-build pkgconf-pkg-config gcc-c++ clang openssl-devel \
        systemd-devel libei-devel pipewire-devel mesa-libgbm-devel libglvnd-devel libva-devel wayland-devel pam-devel \
        dbus-daemon python3-dbus python3-gobject pipewire wayland-devel libxkbcommon-devel wayland-protocols-devel
    ;;
arch)
    pacman -Syu --noconfirm --needed git meson ninja pkgconf gcc clang openssl \
        systemd libei pipewire mesa libglvnd libva wayland dbus python-dbus python-gobject pam libxkbcommon wayland-protocols
    ;;
*)
    echo "unsupported distribution: $ID" >&2
    exit 1
    ;;
esac
