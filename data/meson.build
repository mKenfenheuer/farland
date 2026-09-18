# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# farlandd's system integration (docs/ROADMAP.md M7): the service, the PAM
# configuration, the D-Bus and polkit policies, and an example configuration.
if host_machine.system() == 'linux'
  systemd_dep = dependency('systemd', required: false)
  unit_dir = get_option('prefix') / 'lib' / 'systemd' / 'system'
  dbus_dir = get_option('prefix') / get_option('datadir') / 'dbus-1' / 'system.d'
  polkit_dir = get_option('prefix') / get_option('datadir') / 'polkit-1' / 'actions'
  if systemd_dep.found()
    unit_dir = systemd_dep.get_variable('systemdsystemunitdir', pkgconfig_define: ['prefix', get_option('prefix')])
  endif
  configure_file(
    input: 'farlandd.service.in',
    output: 'farlandd.service',
    configuration: {'sbindir': get_option('prefix') / get_option('sbindir')},
    install_dir: unit_dir,
  )
  install_data('pam/farland', install_dir: get_option('sysconfdir') / 'pam.d')
  install_data('dbus/org.farland.Farland1.conf', install_dir: dbus_dir)
  install_data('polkit/org.farland.Farland1.policy', install_dir: polkit_dir)
  install_data('farland.toml', install_dir: get_option('datadir') / 'doc' / 'farland' / 'examples')
endif
