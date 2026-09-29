#!/bin/sh
# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0
#
# Runs at every GNOME login (autostart): the keyboard layout FARLAND_KEYBOARD
# set at the container's start, which the entrypoint left here as
# layout[+variant]. GNOME keeps its layouts in dconf, which only the session
# can write, so the entrypoint cannot set it itself.
f=/run/farland-container/keyboard
[ -r "$f" ] || exit 0
source=$(cat "$f")
exec gsettings set org.gnome.desktop.input-sources sources "[('xkb', '$source')]"
