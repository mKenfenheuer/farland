#!/bin/sh
# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0
#
# Enrols one user from the environment and starts farland-server on the
# test desktop. Anything after the image name is passed to farland-server,
# so the codec and the log level can be chosen:
#
#   podman run ... farland --gfx-codec avc420 --log-level debug
#
# FARLAND_USER / FARLAND_PASSWORD  the one account the image enrols
# FARLAND_DOMAIN                   pin a domain (empty matches any)
# FARLAND_PORT                     default 3389
# FARLAND_ALLOW_TLS_ONLY           any value: also admit clients without
#                                  NLA, which then reach the desktop
#                                  without logging in
set -eu

port=${FARLAND_PORT:-3389}
user=${FARLAND_USER:-farland}
password=${FARLAND_PASSWORD:-}

if [ -z "$password" ]; then
    # A random one rather than a default everybody knows: the log says what
    # it is, and a client can use it.
    password=$(tr -dc 'A-Za-z0-9' </dev/urandom | head -c 16)
    echo "farland: no FARLAND_PASSWORD given; the password for $user is $password"
fi

# No prompting in a container: farlandctl reads the first line of stdin.
printf '%s\n' "$password" |
    farlandctl passwd "$user" ${FARLAND_DOMAIN:+--domain "$FARLAND_DOMAIN"} --stdin

set -- --port "$port" ${FARLAND_ALLOW_TLS_ONLY:+--allow-tls-only} "$@"
# A container has one process and it must be the server, so that a stop
# signal reaches it.
exec farland-server "$@"
