# SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
# SPDX-License-Identifier: Apache-2.0

# OpenSSL headers are included as system headers: their macros (SSL_OP_*,
# SSL_CTX_set_min_proto_version, ...) expand to C casts that would trip
# -Wold-style-cast where OpenSSL is not installed under /usr/include.
farland_openssl_dep = openssl_dep.as_system('system')

farland_auth_lib = static_library(
  'farland-auth',
  'credential_store.cpp',
  'credssp.cpp',
  'legacy_crypto.cpp',
  'ntlm.cpp',
  'openssl.cpp',
  'spnego.cpp',
  'tls.cpp',
  'tls_identity.cpp',
  include_directories: farland_inc,
  dependencies: [farland_base_dep, farland_openssl_dep],
  cpp_args: farland_safe_buffer_args,
)

farland_auth_dep = declare_dependency(
  link_with: farland_auth_lib,
  include_directories: farland_inc,
  dependencies: [farland_base_dep, farland_openssl_dep],
)
