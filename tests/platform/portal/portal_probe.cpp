// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// farland-portal-probe: starts a real xdg-desktop-portal RemoteDesktop session
// and prints what the portal granted. For trying the portal client on a
// desktop (GNOME, KDE); it is not a test.

#include <farland/base/log.hpp>
#include <farland/platform/portal/portal_input.hpp>
#include <farland/platform/portal/portal_session.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <iostream>
#include <poll.h>
#include <string>
#include <string_view>
#include <thread>

namespace portal = farland::platform::portal;
using namespace std::chrono_literals;

namespace {

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables): signal handler state
portal::PortalSession* active_session = nullptr;
volatile std::sig_atomic_t interrupted = 0;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

extern "C" void on_signal(int /*signal*/)
{
    interrupted = 1;
    if (active_session != nullptr) {
        active_session->cancel();
    }
}

void usage()
{
    std::cerr << "usage: farland-portal-probe [options]\n"
                 "  --virtual            also ask for a virtual monitor\n"
                 "  --no-persist         do not ask for a restore token\n"
                 "  --token-file PATH    restore token file (default: $XDG_STATE_HOME/farland/portal-restore-token)\n"
                 "  --no-token           neither read nor write a restore token\n"
                 "  --embedded-cursor    do not ask for cursor metadata\n"
                 "  --timeout SECONDS    how long to wait for the dialog (default 300)\n"
                 "  --notify             test Notify* input (moves the pointer, scrolls) instead of ConnectToEIS\n"
                 "  --wait               keep the session until it is closed or Ctrl-C\n"
                 "  --verbose            debug logging\n";
}

void print_streams(const portal::PortalSession& session)
{
    const auto& caps = session.capabilities();
    std::cout << "RemoteDesktop v" << caps.remote_desktop_version << ", device types " << caps.device_types
              << "; ScreenCast v" << caps.screen_cast_version << ", source types " << caps.source_types
              << ", cursor modes " << caps.cursor_modes << '\n';
    std::cout << "session " << session.session_handle() << '\n';
    std::cout << "granted devices " << session.devices() << " (1 keyboard, 2 pointer, 4 touchscreen), cursor mode "
              << static_cast<unsigned>(session.cursor_mode()) << ", clipboard "
              << (session.clipboard_enabled() ? "yes" : "no") << '\n';
    for (const auto& stream : session.streams()) {
        std::cout << "stream: node " << stream.node_id << " id '" << stream.id << "' type " << stream.source_type;
        if (stream.position) {
            std::cout << " position " << stream.position->first << ',' << stream.position->second;
        }
        if (stream.size) {
            std::cout << " size " << stream.size->first << 'x' << stream.size->second;
        }
        std::cout << " mapping '" << stream.mapping_id << "'\n";
    }
}

void notify_demo(portal::PortalSession& session)
{
    portal::PortalNotifyInput input(session);
    const auto layout = portal::default_layout(session.streams());
    if (layout.empty()) {
        return;
    }
    const auto& region = layout.front().desktop;
    const double cx = region.x + (region.width / 2.0);
    const double cy = region.y + (region.height / 2.0);
    std::cout << "moving the pointer around " << cx << ',' << cy << " and scrolling\n";
    for (int i = 0; i < 4; ++i) {
        const double dx = (i == 1 || i == 2) ? 100 : -100;
        const double dy = (i >= 2) ? 100 : -100;
        input.pointer_motion_absolute(cx + dx, cy + dy);
        input.flush();
        std::this_thread::sleep_for(300ms);
    }
    input.pointer_motion_relative(20, 20);
    input.scroll_discrete(0, 120);
    input.scroll_discrete(0, -120);
    input.flush();
}

}  // namespace

int main(int argc, char** argv)
{
    portal::PortalOptions options;
    std::optional<std::filesystem::path> token_file = portal::default_restore_token_path();
    bool notify = false;
    bool wait = false;
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto arg = args[i];
        const bool has_value = i + 1 < args.size();
        if (arg == "--virtual") {
            options.virtual_monitor = true;
        } else if (arg == "--no-persist") {
            options.persist = false;
        } else if (arg == "--token-file" && has_value) {
            token_file = std::filesystem::path(args[++i]);
        } else if (arg == "--no-token") {
            token_file.reset();
        } else if (arg == "--embedded-cursor") {
            options.cursor_metadata = false;
        } else if (arg == "--timeout" && has_value) {
            options.timeout = std::chrono::seconds(std::stoi(std::string(args[++i])));
        } else if (arg == "--notify") {
            notify = true;
        } else if (arg == "--wait") {
            wait = true;
        } else if (arg == "--verbose") {
            farland::log::set_level(farland::log::Level::debug);
        } else {
            usage();
            return 2;
        }
    }

    if (token_file) {
        auto token = portal::load_restore_token(*token_file);
        if (!token) {
            std::cerr << "cannot load " << token_file->string() << ": " << token.error().message() << '\n';
            return 1;
        }
        options.restore_token = *token;
        std::cout << (options.restore_token ? "restoring with the token from " : "no token in ") << token_file->string()
                  << '\n';
    }

    portal::PortalSession session;
    active_session = &session;
    struct sigaction action{};
    action.sa_handler = on_signal;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    std::cout << "starting the portal session; answer the dialog...\n";
    if (auto started = session.start(options); !started) {
        std::cerr << "failed: " << portal::to_string(started.error().code) << ": " << started.error().message << '\n';
        return 1;
    }
    print_streams(session);

    if (token_file) {
        if (const auto& token = session.restore_token()) {
            auto saved = portal::save_restore_token(*token_file, *token);
            std::cout << (saved ? "saved the new restore token to " : "cannot save the restore token to ")
                      << token_file->string() << '\n';
        } else {
            std::cout << "no restore token granted\n";
            static_cast<void>(portal::remove_restore_token(*token_file));
        }
    }

    auto pipewire = session.open_pipewire_remote();
    if (pipewire) {
        std::cout << "PipeWire remote: fd " << pipewire->get() << '\n';
    } else {
        std::cerr << "OpenPipeWireRemote failed: " << pipewire.error().message << '\n';
    }
    if (notify) {
        notify_demo(session);
    } else {
        auto eis = session.connect_to_eis();
        if (!eis) {
            std::cerr << "ConnectToEIS failed: " << eis.error().message << '\n';
        } else if (!eis->has_value()) {
            std::cout << "ConnectToEIS: not available (RemoteDesktop < 2); use --notify\n";
        } else {
            std::cout << "EIS: fd " << (*eis)->get() << '\n';
        }
    }

    if (wait) {
        std::cout << "waiting until the session is closed (Ctrl-C ends it)\n";
        while (interrupted == 0 && !session.closed()) {
            pollfd pfd{.fd = session.fd(), .events = session.events(), .revents = 0};
            ::poll(&pfd, 1, 250);
            session.process();
        }
        std::cout << (session.closed() ? "the portal closed the session\n" : "interrupted\n");
    }
    active_session = nullptr;
    return 0;
}
