// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// farland-touch-probe: starts a real xdg-desktop-portal RemoteDesktop session,
// connects to the compositor's EIS server and touches the shared monitor: a
// tap in the middle, a one-finger swipe and a two-finger spread. For trying
// EiInput's touch path on a desktop without a touch client; it is not a test.

#include <farland/base/log.hpp>
#include <farland/platform/portal/ei_input.hpp>
#include <farland/platform/portal/portal_session.hpp>

#include <chrono>
#include <iostream>
#include <poll.h>
#include <string>
#include <string_view>
#include <vector>

namespace portal = farland::platform::portal;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

/// Runs EIS and the portal for `duration`.
void pump(portal::EiInput& input, portal::PortalSession& session, Clock::duration duration)
{
    const auto until = Clock::now() + duration;
    do {
        std::vector<pollfd> fds{{.fd = input.fd(), .events = POLLIN, .revents = 0},
                                {.fd = session.fd(), .events = session.events(), .revents = 0}};
        ::poll(fds.data(), fds.size(), 10);
        input.dispatch();
        session.process();
    } while (Clock::now() < until && !input.closed());
}

}  // namespace

int main(int argc, char** argv)
{
    std::optional<std::filesystem::path> token_file = portal::default_restore_token_path();
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--token-file" && i + 1 < args.size()) {
            token_file = std::filesystem::path(args[++i]);
        } else if (args[i] == "--verbose") {
            farland::log::set_level(farland::log::Level::debug);
        } else {
            std::cerr << "usage: farland-touch-probe [--token-file PATH] [--verbose]\n"
                         "  The token file defaults to farland-server's; the new token is written back.\n";
            return 2;
        }
    }

    portal::PortalOptions options;
    if (token_file) {
        if (auto token = portal::load_restore_token(*token_file); token && *token) {
            options.restore_token = **token;
        }
    }
    portal::PortalSession session;
    std::cout << "starting the portal session"
              << (options.restore_token ? " with the stored token" : "; answer the dialog") << "...\n";
    if (auto started = session.start(options); !started) {
        std::cerr << "failed: " << portal::to_string(started.error().code) << ": " << started.error().message << '\n';
        return 1;
    }
    if (token_file) {
        const auto& token = session.restore_token();
        static_cast<void>(token ? portal::save_restore_token(*token_file, *token)
                                : portal::remove_restore_token(*token_file));
    }
    std::cout << "granted devices " << session.devices() << " (4 is the touchscreen)\n";
    const auto& stream = session.streams().front();
    if (!stream.size) {
        std::cerr << "the stream has no size\n";
        return 1;
    }
    auto eis = session.connect_to_eis();
    if (!eis || !eis->has_value()) {
        std::cerr << "no EIS connection\n";
        return 1;
    }
    auto connected = portal::EiInput::connect_fd((*eis)->release());
    if (!connected) {
        std::cerr << "libei: " << connected.error().message() << '\n';
        return 1;
    }
    auto& input = **connected;
    const auto [width, height] = *stream.size;
    input.set_outputs({{farland::platform::Rect{0, 0, width, height}, stream.mapping_id}});

    const auto deadline = Clock::now() + 5s;
    while (!input.accepts_touch() && !input.closed() && Clock::now() < deadline) {
        pump(input, session, 50ms);
    }
    std::cout << "EIS devices: keyboard " << input.can_send(portal::EiInput::Capability::keyboard) << ", absolute "
              << input.can_send(portal::EiInput::Capability::pointer_absolute) << ", touch " << input.accepts_touch()
              << '\n';
    if (!input.accepts_touch()) {
        std::cerr << "the compositor offers no touch device\n";
        return 1;
    }

    const double cx = width / 2.0;
    const double cy = height / 2.0;
    std::cout << "tap at " << cx << ',' << cy << '\n';
    input.touch_down(0, cx, cy);
    input.flush();
    pump(input, session, 80ms);
    input.touch_up(0);
    input.flush();
    pump(input, session, 500ms);

    std::cout << "swipe right from " << cx - 200 << ',' << cy << '\n';
    input.touch_down(1, cx - 200, cy);
    input.flush();
    for (int step = 1; step <= 20; ++step) {
        pump(input, session, 16ms);
        input.touch_motion(1, cx - 200 + (step * 20), cy);
        input.flush();
    }
    input.touch_up(1);
    input.flush();
    pump(input, session, 500ms);

    std::cout << "two fingers spreading, the second one canceled\n";
    input.touch_down(2, cx - 50, cy + 100);
    input.touch_down(3, cx + 50, cy + 100);
    input.flush();
    for (int step = 1; step <= 10; ++step) {
        pump(input, session, 16ms);
        input.touch_motion(2, cx - 50 - (step * 10), cy + 100);
        input.touch_motion(3, cx + 50 + (step * 10), cy + 100);
        input.flush();
    }
    input.touch_up(2);
    input.touch_cancel(3);
    input.flush();
    pump(input, session, 1s);

    std::cout << (input.closed() ? "the EIS server disconnected\n" : "done; still connected\n");
    return input.closed() ? 1 : 0;
}
