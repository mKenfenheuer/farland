// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Stand-ins for the headless backends this build does not have (see
// headless.hpp). apps/meson.build compiles this file whenever at least one
// backend is missing and defines FARLAND_HAVE_*_HEADLESS for those present.

#include "headless.hpp"

namespace farland::app {

#ifndef FARLAND_HAVE_GNOME_HEADLESS
Result<std::unique_ptr<Desktop>> start_gnome_headless(const HeadlessOptions& /*options*/)
{
    return fail(Errc::unsupported, "this build has no GNOME headless backend (-Dheadless-gnome)");
}
#endif

#ifndef FARLAND_HAVE_PLASMA_HEADLESS
Result<std::unique_ptr<Desktop>> start_plasma_headless(const HeadlessOptions& /*options*/)
{
    return fail(Errc::unsupported, "this build has no Plasma headless backend (-Dheadless-plasma)");
}
#endif

#ifndef FARLAND_HAVE_WLROOTS_HEADLESS
Result<std::unique_ptr<Desktop>> start_wlroots_headless(const HeadlessOptions& /*options*/)
{
    return fail(Errc::unsupported, "this build has no wlroots headless backend for sway, labwc and cage "
                                   "(-Dheadless-wlroots)");
}
#endif

}  // namespace farland::app
