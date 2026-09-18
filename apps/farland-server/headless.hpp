// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include "desktop.hpp"
#include "headless_options.hpp"

#include <memory>
#include <optional>
#include <string_view>

/// Headless desktops for multi-session (docs/ROADMAP.md M7): one factory per
/// compositor family, and the dispatcher farland-agent calls.
///
/// Each factory lives in its backend's file pair (gnome_headless.cpp,
/// plasma_headless.cpp, wlroots_headless.cpp), whose header declares it the
/// same way. A build without a backend (its option off, or its dependencies
/// missing) compiles headless_unavailable.cpp instead, where the factory
/// fails with Errc::unsupported, so the dispatcher and farland-agent always
/// link.
namespace farland::app {

[[nodiscard]] Result<std::unique_ptr<Desktop>> start_gnome_headless(const HeadlessOptions& options);
[[nodiscard]] Result<std::unique_ptr<Desktop>> start_plasma_headless(const HeadlessOptions& options);
[[nodiscard]] Result<std::unique_ptr<Desktop>> start_wlroots_headless(const HeadlessOptions& options);

/// Starts the headless desktop `options.kind` names for the user this
/// process runs as (or attaches to the running one with `options.attach`):
/// GNOME through Mutter, Plasma through KWin, sway, labwc and cage through
/// the wlroots backend.
[[nodiscard]] Result<std::unique_ptr<Desktop>> start_headless_desktop(const HeadlessOptions& options);

/// Whether this build has the backend for `kind`.
[[nodiscard]] bool headless_backend_built(HeadlessKind kind) noexcept;

[[nodiscard]] std::string_view to_string(HeadlessKind kind) noexcept;
/// "gnome", "plasma", "sway", "labwc" or "cage".
[[nodiscard]] std::optional<HeadlessKind> parse_headless_kind(std::string_view name) noexcept;

}  // namespace farland::app
