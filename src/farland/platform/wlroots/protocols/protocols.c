// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The interface tables of every Wayland protocol the wlroots backend speaks,
// as wayland-scanner generated them (see meson.build). They come in through
// the system include path, so compiler warnings and clang-tidy skip them.

#include <ext-data-control-v1-protocol-code.h>
#include <ext-foreign-toplevel-list-v1-protocol-code.h>
#include <ext-image-capture-source-v1-protocol-code.h>
#include <ext-image-copy-capture-v1-protocol-code.h>
#include <virtual-keyboard-unstable-v1-protocol-code.h>
#include <wlr-data-control-unstable-v1-protocol-code.h>
#include <wlr-output-management-unstable-v1-protocol-code.h>
#include <wlr-screencopy-unstable-v1-protocol-code.h>
#include <wlr-virtual-pointer-unstable-v1-protocol-code.h>
#ifdef FARLAND_HAVE_LINUX_DMABUF
#include <linux-dmabuf-v1-protocol-code.h>
#endif
