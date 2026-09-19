# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# OpenSSL headers are included as system headers: their macros (SSL_OP_*,
# SSL_CTX_set_min_proto_version, ...) expand to C casts that would trip
# -Wold-style-cast where OpenSSL is not installed under /usr/include.
farland_openssl_dep = openssl_dep.as_system('system')

# Kerberos as an NLA mechanism. MIT krb5's GSS-API; Heimdal's is not tried.
# System headers: gssapi.h defines its constants with C casts.
kerberos_opt = get_option('kerberos')
krb5_gssapi_dep = dependency('krb5-gssapi', required: kerberos_opt, include_type: 'system')
have_kerberos = krb5_gssapi_dep.found()
farland_auth_deps = [farland_base_dep, farland_openssl_dep]
farland_auth_args = ['-DFARLAND_HAVE_KERBEROS=@0@'.format(have_kerberos ? 1 : 0)]
if have_kerberos
  farland_auth_deps += krb5_gssapi_dep
endif

farland_auth_lib = static_library(
  'farland-auth',
  'auto_reconnect.cpp',
  'credential_store.cpp',
  'credssp.cpp',
  'kerberos.cpp',
  'legacy_crypto.cpp',
  'ntlm.cpp',
  'openssl.cpp',
  'spnego.cpp',
  'tls.cpp',
  'tls_identity.cpp',
  include_directories: farland_inc,
  dependencies: farland_auth_deps,
  cpp_args: farland_safe_buffer_args + farland_auth_args,
)

farland_auth_dep = declare_dependency(
  link_with: farland_auth_lib,
  include_directories: farland_inc,
  dependencies: farland_auth_deps,
  compile_args: farland_auth_args,
)
