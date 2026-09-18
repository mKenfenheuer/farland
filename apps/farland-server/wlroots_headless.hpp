// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include "desktop.hpp"
#include "headless_options.hpp"

#include <memory>

namespace farland::app {

/// A wlroots compositor as a desktop without a portal (docs/PLAN.md §3.3,
/// ROADMAP M7): options.kind is sway, labwc or cage.
///
/// - Launch (attach false): the compositor starts on the headless backend
///   (WLR_BACKENDS=headless, WLR_LIBINPUT_NO_DEVICES=1) with one output of
///   options.width x options.height, rendering with pixman, or on
///   options.render_node. cage runs options.cage_command. The compositor
///   stops when the desktop is destroyed, and the desktop closes when the
///   compositor exits (the user logged out, cage's application ended).
/// - Attach: the compositor of this session ($WAYLAND_DISPLAY), every
///   output a screen; it keeps running and its outputs keep their sizes.
///
/// Frames and the cursor come from ext-image-copy-capture-v1 (sway, labwc)
/// or wlr-screencopy (cage, with the cursor in the frames); input from the
/// virtual keyboard (a keymap for options.keymap_layout) and virtual
/// pointer; the clipboard from ext- or wlr-data-control (none on cage).
/// Launched screens are resizable through wlr-output-management; their
/// count stays what it was at the start.
///
/// Blocks until every screen delivered its first frame, at most
/// options.timeout. Linux only; built with the wlroots backend.
[[nodiscard]] Result<std::unique_ptr<Desktop>> start_wlroots_headless(const HeadlessOptions& options);

}  // namespace farland::app
