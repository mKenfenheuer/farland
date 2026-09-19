# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0
#
# The RPM package: the same four binaries and the same system integration as
# the Debian one (packaging/make-deb.sh), for Fedora and openSUSE.
# packaging/make-rpm.sh builds it from a checkout; rpmbuild -ba with a
# tarball in SOURCES also works.

Name:           farland
Version:        0.0.1
Release:        1%{?dist}
Summary:        RDP server for Linux and Wayland, with headless multi-session

License:        Apache-2.0
URL:            https://github.com/mKenfenheuer/farland
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  meson >= 1.3.0
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig
BuildRequires:  systemd-rpm-macros
BuildRequires:  pkgconfig(openssl)
BuildRequires:  pkgconfig(libsystemd)
BuildRequires:  pkgconfig(libei-1.0)
BuildRequires:  pkgconfig(libpipewire-0.3)
BuildRequires:  pkgconfig(gbm)
BuildRequires:  pkgconfig(egl)
BuildRequires:  pkgconfig(libva)
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(xkbcommon)
BuildRequires:  wayland-protocols-devel
BuildRequires:  pam-devel
BuildRequires:  krb5-devel

Requires:       pam
Recommends:     xdg-desktop-portal
Recommends:     pipewire

%description
farland serves the Remote Desktop Protocol on Wayland desktops: NLA, the
Graphics Pipeline with RemoteFX Progressive, ClearCodec and H.264 (VA-API,
NVENC or OpenH264), clipboard, audio, multi-monitor, resizing, and touch.

The farlandd service gives each user their own headless desktop over one
port: GNOME through GDM, or Plasma, sway, labwc and cage through farland's
own PAM and logind sessions. Users reconnect into the session they left.
Configure it in /etc/farland/farland.toml and enrol users with farlandctl.

%prep
%autosetup

%build
# The package ships no tests, and a package build should not reach for a
# Catch2 wrap it may not be allowed to download: run `meson test` from the
# checkout instead (CONTRIBUTING.md). --werror is the build the packages
# are made from, as in packaging/make-deb.sh.
%meson -Dwerror=true -Dtests=disabled
%meson_build

%install
%meson_install
# The state directory farlandd keeps its certificate, its credential store
# and its session table in.
install -d -m 0700 %{buildroot}%{_sharedstatedir}/%{name}
install -d -m 0755 %{buildroot}%{_sysconfdir}/%{name}

%post
%systemd_post farlandd.service
if [ ! -e %{_sysconfdir}/%{name}/farland.toml ]; then
    cp %{_datadir}/%{name}/farland.toml %{_sysconfdir}/%{name}/farland.toml
    chmod 0644 %{_sysconfdir}/%{name}/farland.toml
fi
# The service is the point of the package: headless multi-user sessions on
# port 3389. Enrol users with farlandctl.
if [ $1 -eq 1 ] && [ -d /run/systemd/system ]; then
    systemctl enable --now farlandd.service >/dev/null 2>&1 || :
fi

%preun
%systemd_preun farlandd.service

%postun
# An upgrade leaves the old daemon running beside the new agents on disk,
# and the two speak the broker protocol to each other, so restart it. The
# sessions and their clients survive that (docs/ROADMAP.md M7): farlandd
# writes its session table, the agents keep their desktops and the network
# processes keep the clients' connections, and the new farlandd picks them
# up again.
%systemd_postun_with_restart farlandd.service
# /etc/farland/farland.toml and /var/lib/farland (the certificate, the
# enrolled users and the session table) stay behind on erase. rpm has no
# purge to ask for them to go, and an administrator's settings and a
# server's identity are not ours to delete on the way out.

%files
%license LICENSE
%doc README.md
# meson installs the reference configuration here, and %%post copies it into
# /etc the first time. The same reasoning as the Debian package: under
# %%config it would change with every release that documents a new key, and
# the administrator's settings and the documentation of the defaults are
# better kept apart. It is not documentation, though -- an install with
# --nodocs would leave it out and %%post would have nothing to copy.
%{_datadir}/%{name}/farland.toml
%dir %{_datadir}/%{name}
%{_bindir}/farland-server
%{_bindir}/farland-agent
%{_bindir}/farlandctl
%{_sbindir}/farlandd
%{_unitdir}/farlandd.service
%config(noreplace) %{_sysconfdir}/pam.d/farland
%{_datadir}/dbus-1/system.d/org.farland.Farland1.conf
%{_datadir}/polkit-1/actions/org.farland.Farland1.policy
# One per program that attaches to a running Plasma session, so KWin can
# grant it screen casting: server and agent.
%{_datadir}/applications/org.farland.*.desktop
%{_libdir}/security/pam_farland.so
%dir %{_sysconfdir}/%{name}
%attr(0700,root,root) %dir %{_sharedstatedir}/%{name}

%changelog
* Sat Sep 19 2026 Maximilian Kenfenheuer <maximilian.kenfenheuer@ksol.it> - 0.0.1-1
- First package.
