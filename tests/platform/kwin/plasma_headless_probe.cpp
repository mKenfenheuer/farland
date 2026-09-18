// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Manual check of the headless Plasma desktop against a real KWin (not run as
// a test), the twin of farland-gnome-headless-probe: starts a session (or
// attaches to the running one with --attach), then asks for each layout in
// turn and reports the screens the desktop ends up with.
//
//   farland-plasma-headless-probe [--attach] [--keymap LAYOUT] [--hold SECONDS]
//       1600x900 1280x720,1024x768 800x600
//
// Each argument is one layout: the client monitors' sizes, comma-separated.
// --debug turns on the debug log; --keep-seat counts frames without handing
// the seat a login screen, and --baseline SECONDS is how long the frames are
// counted before the seat is handed over, which is the measurement's
// baseline: it has to show frames, or the measurement after it says nothing.
// --hold keeps the last layout afterwards and prints the frames each screen
// gives per second, which is how to see whether KWin keeps drawing a virtual
// output while the session is not active on its seat; --poke moves the
// pointer each second while it holds.

#include <farland/base/log.hpp>

#include "plasma_headless.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <poll.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Size = std::pair<std::uint32_t, std::uint32_t>;

std::vector<Size> parse_layout(std::string_view text)
{
    std::vector<Size> sizes;
    while (!text.empty()) {
        const auto comma = text.find(',');
        const std::string entry(text.substr(0, comma));
        unsigned width = 0;
        unsigned height = 0;
        if (std::sscanf(entry.c_str(), "%ux%u", &width, &height) == 2) {  // NOLINT(cert-err34-c)
            sizes.emplace_back(width, height);
        }
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    return sizes;
}

/// Counts the frames each screen gave in the second that just passed, as
/// --hold reports them.
class FrameRate {
public:
    void count(std::size_t screen)
    {
        if (frames_.size() <= screen) {
            frames_.resize(screen + 1);
        }
        ++frames_[screen];
    }

    /// Prints a line once a second has passed since the last one.
    void report()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - since_ < std::chrono::seconds(1)) {
            return;
        }
        std::string line;
        for (const auto frames : frames_) {
            line += std::format(" {}", frames);
        }
        std::printf("%s frames/s:%s\n",
                    std::format("{:%T}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()))
                        .c_str(),
                    line.empty() ? " -" : line.c_str());
        std::fflush(stdout);
        std::ranges::fill(frames_, 0);
        since_ = now;
    }

private:
    std::vector<unsigned> frames_;
    std::chrono::steady_clock::time_point since_ = std::chrono::steady_clock::now();
};

/// Dispatches and takes frames, as a session would, for `seconds` or until
/// `done()`. With `rate`, reports the frames each screen gives per second.
template <class Done>
bool run(farland::app::Desktop& desktop, int seconds, Done done, FrameRate* rate = nullptr, bool poke = false)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<pollfd> fds;
        for (const int fd : desktop.dispatch_fds()) {
            fds.push_back(pollfd{fd, POLLIN, 0});
        }
        for (std::size_t i = 0; i < desktop.screen_count(); ++i) {
            fds.push_back(pollfd{desktop.screen_frames(i).wake_fd(), POLLIN, 0});
        }
        ::poll(fds.data(), fds.size(), 100);
        desktop.dispatch();
        if (desktop.closed()) {
            std::printf("the desktop closed\n");
            return false;
        }
        for (std::size_t i = 0; i < desktop.screen_count(); ++i) {
            if (desktop.screen_frames(i).take_frame() && rate != nullptr) {
                rate->count(i);
            }
        }
        if (rate != nullptr) {
            rate->report();
            if (poke) {
                // Moves the pointer, so that what reads the desktop's idle
                // time sees input arriving even while it is not on its seat.
                static double x = 10.0;
                x = x > 200.0 ? 10.0 : x + 7.0;
                desktop.input().pointer_motion_absolute(x, x);
            }
        }
        if (done()) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv)
{
    farland::app::HeadlessOptions options;
    options.kind = farland::app::HeadlessKind::plasma;
    std::vector<std::vector<Size>> layouts;
    int hold = 0;
    bool poke = false;
    bool keep_seat = false;
    int baseline = 5;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--attach") {
            options.attach = true;
        } else if (arg == "--keymap" && i + 1 < argc) {
            options.keymap_layout = argv[++i];
        } else if (arg == "--debug") {
            farland::log::set_level(farland::log::Level::debug);
        } else if (arg == "--baseline" && i + 1 < argc) {
            baseline = std::atoi(argv[++i]);  // NOLINT(cert-err34-c)
        } else if (arg == "--keep-seat") {
            keep_seat = true;
        } else if (arg == "--poke") {
            poke = true;
        } else if (arg == "--hold" && i + 1 < argc) {
            hold = std::atoi(argv[++i]);  // NOLINT(cert-err34-c)
        } else {
            layouts.push_back(parse_layout(arg));
        }
    }
    auto started = farland::app::start_plasma_headless(options);
    if (!started) {
        std::printf("cannot start: %s\n", std::string(started.error().message()).c_str());
        return 1;
    }
    auto& desktop = **started;
    std::printf("started: %zu screen(s), resizable: %s, screens follow monitors: %s\n", desktop.screen_count(),
                desktop.resizable() ? "yes" : "no", desktop.screens_follow_monitors() ? "yes" : "no");
    int failures = 0;
    for (const auto& layout : layouts) {
        std::printf("asking for %zu monitor(s)\n", layout.size());
        desktop.request_screen_sizes(layout);
        const bool reached = run(desktop, 20, [&] {
            if (desktop.screen_count() != layout.size()) {
                return false;
            }
            for (std::size_t i = 0; i < layout.size(); ++i) {
                if (desktop.screen_frames(i).size() != layout[i]) {
                    return false;
                }
            }
            return true;
        });
        std::printf("%s: %zu screen(s):", reached ? "reached" : "NOT reached", desktop.screen_count());
        for (std::size_t i = 0; i < desktop.screen_count(); ++i) {
            const auto [width, height] = desktop.screen_frames(i).size();
            std::printf(" %ux%u", width, height);
        }
        std::printf("\n");
        failures += reached ? 0 : 1;
        // Frames keep coming on every screen afterwards.
        static_cast<void>(run(desktop, 2, [] { return false; }));
    }
    if (hold > 0) {
        // Holds the last layout as a client would and reports the frames per
        // second, to watch what the desktop does while something else happens
        // on its seat; then lets go, as a client disconnecting does.
        // The frames the screens give while the session still has its seat,
        // as the measurement's baseline: 0 here means nothing is animating,
        // and says nothing about what happens off the seat.
        FrameRate rate;
        std::printf("before the takeover, with the session on its seat:\n");
        static_cast<void>(run(desktop, baseline, [] { return false; }, &rate, poke));
        if (keep_seat) {
            std::printf("--keep-seat: the seat keeps the session; only counting frames\n");
        } else {
            std::printf("a client takes the desktop (the seat gets a login screen)\n");
            desktop.set_held(true);
        }
        static_cast<void>(run(desktop, hold, [] { return false; }, &rate, poke));
        std::printf("the client lets go (closed: %s)\n", desktop.closed() ? "yes" : "no");
        desktop.set_held(false);
        static_cast<void>(run(desktop, 5, [] { return false; }, &rate, false));
        std::printf("after letting go, closed: %s\n", desktop.closed() ? "yes" : "no");
    }
    return failures == 0 ? 0 : 1;
}
