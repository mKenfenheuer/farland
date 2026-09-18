// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "headless.hpp"

#include <farland/base/log.hpp>

#include <array>
#include <utility>

namespace farland::app {

namespace {

constexpr std::string_view log_component = "app.headless";

constexpr std::array<std::pair<HeadlessKind, std::string_view>, 5> kind_names{{
    {HeadlessKind::gnome, "gnome"},
    {HeadlessKind::plasma, "plasma"},
    {HeadlessKind::sway, "sway"},
    {HeadlessKind::labwc, "labwc"},
    {HeadlessKind::cage, "cage"},
}};

}  // namespace

std::string_view to_string(HeadlessKind kind) noexcept
{
    for (const auto& [value, name] : kind_names) {
        if (value == kind) {
            return name;
        }
    }
    return "unknown";
}

std::optional<HeadlessKind> parse_headless_kind(std::string_view name) noexcept
{
    for (const auto& [value, known] : kind_names) {
        if (known == name) {
            return value;
        }
    }
    return std::nullopt;
}

bool headless_backend_built(HeadlessKind kind) noexcept
{
    switch (kind) {
    case HeadlessKind::gnome:
#ifdef FARLAND_HAVE_GNOME_HEADLESS
        return true;
#else
        return false;
#endif
    case HeadlessKind::plasma:
#ifdef FARLAND_HAVE_PLASMA_HEADLESS
        return true;
#else
        return false;
#endif
    case HeadlessKind::sway:
    case HeadlessKind::labwc:
    case HeadlessKind::cage:
#ifdef FARLAND_HAVE_WLROOTS_HEADLESS
        return true;
#else
        return false;
#endif
    }
    return false;
}

Result<std::unique_ptr<Desktop>> start_headless_desktop(const HeadlessOptions& options)
{
    log::info(log_component, "{} {} desktop at {}x{}{}", options.attach ? "attaching to the" : "starting a headless",
              to_string(options.kind), options.width, options.height,
              options.keymap_layout.empty() ? "" : ", keymap " + options.keymap_layout);
    switch (options.kind) {
    case HeadlessKind::gnome:
        return start_gnome_headless(options);
    case HeadlessKind::plasma:
        return start_plasma_headless(options);
    case HeadlessKind::sway:
    case HeadlessKind::labwc:
    case HeadlessKind::cage:
        if (options.kind == HeadlessKind::cage && options.cage_command.empty() && !options.attach) {
            return fail(Errc::invalid_value, "cage needs the application to run");
        }
        return start_wlroots_headless(options);
    }
    return fail(Errc::invalid_value, "unknown headless desktop");
}

}  // namespace farland::app
