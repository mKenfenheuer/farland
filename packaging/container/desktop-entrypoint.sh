#!/bin/sh
# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0
#
# Makes the one account from the environment and enrols it for NLA, then
# becomes systemd. systemd does not hand the container's environment to its
# services, so this is done before it starts.
#
# FARLAND_USER / FARLAND_PASSWORD  the account, and its RDP and login
#                                  password; it may use sudo
# FARLAND_LOCALE                   language and formats, e.g. de_DE.UTF-8;
#                                  the image needs the language's packs
#                                  (FARLAND_LANGUAGES at build time)
# FARLAND_TIMEZONE                 e.g. Europe/Berlin
# FARLAND_KEYBOARD                 XKB layout, e.g. de or de(nodeadkeys)
# Unset, each leaves what the last start set, or the image's default.
set -eu

user=${FARLAND_USER:-farland}
password=${FARLAND_PASSWORD:-}

if [ -z "$password" ]; then
    password=$(tr -dc 'A-Za-z0-9' </dev/urandom | head -c 16)
    echo "farland: no FARLAND_PASSWORD given; the password for $user is $password"
fi

# A home kept on a volume outlives the container: reuse the account's uid.
if ! id "$user" >/dev/null 2>&1; then
    if [ -d "/home/$user" ]; then
        useradd --no-create-home --home-dir "/home/$user" --shell /bin/bash --groups sudo \
            --uid "$(stat -c %u "/home/$user")" --user-group "$user" 2>/dev/null ||
            useradd --no-create-home --home-dir "/home/$user" --shell /bin/bash --groups sudo "$user"
    else
        useradd --create-home --shell /bin/bash --groups sudo "$user"
    fi
fi
printf '%s:%s\n' "$user" "$password" | chpasswd
uid=$(id -u "$user")
gid=$(id -g "$user")
home=$(getent passwd "$user" | cut -d: -f6)

# The host's GPU, which --privileged shows as the host has it: the nodes
# belong to groups of the host's, which the account joins. Without a seat,
# logind grants no device access. KWin needs this to cast its screen at all,
# Mutter to render and encode on the GPU. The primary nodes (card*) too:
# on a GPU that cannot render itself, such as vgem, Mesa renders in
# software into dumb buffers, which only a primary node hands out, and KWin
# opens vgem's primary node for that.
for node in /dev/dri/renderD* /dev/dri/card*; do
    [ -e "$node" ] || continue
    node_gid=$(stat -c %g "$node")
    [ "$node_gid" -ne 0 ] || continue
    group=$(getent group "$node_gid" | cut -d: -f1)
    if [ -z "$group" ]; then
        group=host-gpu-$node_gid
        groupadd --gid "$node_gid" "$group"
    fi
    usermod -aG "$group" "$user"
done

env="HOME=$home USER=$user LOGNAME=$user"
as_user() {
    setpriv --reuid "$uid" --regid "$gid" --init-groups env $env "$@"
}
# A KDE setting that cannot be written is no reason not to start.
kde_setting() {
    command -v kwriteconfig6 >/dev/null || return 0
    as_user kwriteconfig6 "$@" 2>/dev/null || echo "farland: cannot write the KDE setting $*" >&2
}

# Language and formats. The locale is generated at every start, as the
# image carries none; /etc/default/locale reaches farland's own sessions
# through pam_env, and GDM gives a GNOME session the language AccountsService
# has for the user. Plasma's own setting, where the user has one, would win
# over both, so it is set too.
if [ -n "${FARLAND_LOCALE:-}" ]; then
    locale=$FARLAND_LOCALE
    if grep -q "^$locale " /usr/share/i18n/SUPPORTED; then
        locale-gen "$locale" >/dev/null
        language=${locale%%.*}
        printf 'LANG=%s\nLANGUAGE=%s:%s\n' "$locale" "$language" "${language%%_*}" >/etc/default/locale
        accounts=/var/lib/AccountsService/users/$user
        mkdir -p "${accounts%/*}"
        [ -f "$accounts" ] && grep -q '^\[User\]' "$accounts" || printf '[User]\n' >>"$accounts"
        sed -i -e '/^Languages\{0,1\}=/d' -e "/^\[User\]/a Language=$locale\nLanguages=$locale;" "$accounts"
        kde_setting --file plasma-localerc --group Formats --key LANG "$locale"
        kde_setting --file plasma-localerc --group Translations --key LANGUAGE "$language:${language%%_*}"
    else
        echo "farland: FARLAND_LOCALE=$locale is no locale this system knows (see /usr/share/i18n/SUPPORTED)" >&2
    fi
fi

if [ -n "${FARLAND_TIMEZONE:-}" ]; then
    if [ -f "/usr/share/zoneinfo/$FARLAND_TIMEZONE" ]; then
        ln -sf "/usr/share/zoneinfo/$FARLAND_TIMEZONE" /etc/localtime
        echo "$FARLAND_TIMEZONE" >/etc/timezone
    else
        echo "farland: FARLAND_TIMEZONE=$FARLAND_TIMEZONE is no time zone this system knows" >&2
    fi
fi

# The keyboard: what the desktop maps the client's keys with (farland sends
# key positions). The system default, for new users and the login screen;
# Plasma's own setting, which KWin reads when it starts; and for GNOME, whose
# setting lives in dconf, a file the session applies at login
# (farland-keyboard).
if [ -n "${FARLAND_KEYBOARD:-}" ]; then
    layout=${FARLAND_KEYBOARD%%(*}
    variant=
    case $FARLAND_KEYBOARD in
    *"("*")") variant=${FARLAND_KEYBOARD#*(} variant=${variant%)} ;;
    esac
    printf 'XKBMODEL="pc105"\nXKBLAYOUT="%s"\nXKBVARIANT="%s"\nXKBOPTIONS=""\nBACKSPACE="guess"\n' \
        "$layout" "$variant" >/etc/default/keyboard
    kde_setting --file kxkbrc --group Layout --key Use true
    kde_setting --file kxkbrc --group Layout --key LayoutList "$layout"
    kde_setting --file kxkbrc --group Layout --key VariantList "$variant"
    # /run is new with every container, so an unset FARLAND_KEYBOARD
    # enforces nothing at the next login.
    mkdir -p /run/farland-container
    echo "$layout${variant:++$variant}" >/run/farland-container/keyboard
fi

# A user's tasks: systemd allows each user a third of what the container
# may have, which under a container limit (Podman's is 2048) is too few for
# a desktop. The container's own limit is the one that counts.
mkdir -p /etc/systemd/system/user-.slice.d
printf '[Slice]\nTasksMax=infinity\n' >/etc/systemd/system/user-.slice.d/50-farland-container.conf

# What `farlandctl passwd` would store after asking polkit and PAM, which
# are not running yet.
printf '%s\n' "$password" |
    farlandctl --file /var/lib/farland/users passwd "$user" --local-account "$user" --stdin

exec /sbin/init
