// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// farland-agent: runs one user's multi-session desktop for farlandd
// (docs/ROADMAP.md M7). farlandd starts it; it is not meant to be run by hand
// except for debugging.

#include <farland/base/log.hpp>
#include <farland/base/text.hpp>

#include "agent.hpp"
#include "agent_token.hpp"
#include "headless.hpp"
#include "test_desktop.hpp"
#include "transport.hpp"

#include <algorithm>
#include <array>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

namespace log = farland::log;
namespace app = farland::app;
constexpr std::string_view log_component = "agent";

std::atomic<bool> stop_requested{false};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): signal state

extern "C" void on_signal(int /*signal*/)
{
    stop_requested.store(true);
}

struct Options {
    std::string socket;
    int token_fd = -1;
    std::uint32_t session_id = 0;
    std::string desktop = "test";
    bool attach = false;
    std::vector<std::string> cage_command;
    app::SessionOptions session;
};

void usage()
{
    std::cout << "usage: farland-agent --socket PATH --session-id N --desktop KIND [options] [-- COMMAND...]\n"
                 "Runs one user's desktop session for farlandd; farlandd starts it.\n"
                 "  --socket PATH         farlandd's agent socket\n"
                 "  --token-fd N          read the session token from descriptor N (default: the environment\n"
                 "                        variable FARLAND_AGENT_TOKEN)\n"
                 "  --session-id N        the session's number at farlandd\n"
                 "  --desktop KIND        test, gnome, plasma, sway, labwc or cage\n"
                 "  --attach              use the compositor already running in this login session\n"
                 "The options below are for running the agent by hand: farlandd sends the session's\n"
                 "codecs, channels and timeouts from /etc/farland/farland.toml and they win.\n"
                 "  --fps N               frame rate (default 30)\n"
                 "  --gfx-codec CODEC     progressive, planar, avc420 or avc444 (default progressive)\n"
                 "  --render-node PATH    DRM render node for the compositor and H.264\n"
                 "  --no-audio, --no-microphone, --no-clipboard\n"
                 "  --log-level LEVEL     trace, debug, info, warn, error (default info)\n"
                 "  -- COMMAND...         cage: the application to run\n";
}

bool parse(std::span<char*> args, Options& options)
{
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= args.size()) {
                throw std::runtime_error(std::string(arg) + " needs a value");
            }
            return args[++i];
        };
        if (arg == "--socket") {
            options.socket = value();
        } else if (arg == "--token-fd") {
            options.token_fd = std::stoi(value());
        } else if (arg == "--session-id") {
            options.session_id = static_cast<std::uint32_t>(std::stoul(value()));
        } else if (arg == "--desktop") {
            options.desktop = value();
        } else if (arg == "--attach") {
            options.attach = true;
        } else if (arg == "--fps") {
            options.session.frames_per_second = static_cast<unsigned>(std::stoul(value()));
        } else if (arg == "--gfx-codec") {
            const auto codec = value();
            if (codec == "progressive") {
                options.session.gfx_codec = farland::server::TileCodec::progressive;
            } else if (codec == "planar") {
                options.session.gfx_codec = farland::server::TileCodec::planar;
            } else if (codec == "avc420") {
                options.session.gfx_codec = farland::server::TileCodec::avc420;
            } else if (codec == "avc444") {
                options.session.gfx_codec = farland::server::TileCodec::avc444;
            } else {
                throw std::runtime_error("--gfx-codec must be progressive, planar, avc420 or avc444");
            }
        } else if (arg == "--render-node") {
            options.session.render_node = value();
        } else if (arg == "--no-audio") {
            options.session.audio = false;
        } else if (arg == "--no-microphone") {
            options.session.microphone = false;
        } else if (arg == "--no-clipboard") {
            options.session.clipboard = false;
        } else if (arg == "--log-level") {
            const auto level = value();
            static constexpr std::array levels{"trace", "debug", "info", "warn", "error"};
            const auto* found = std::ranges::find(levels, level);
            if (found == levels.end()) {
                throw std::runtime_error("unknown log level " + level);
            }
            log::set_level(static_cast<log::Level>(found - levels.begin()));
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return false;
        } else if (arg == "--") {
            for (++i; i < args.size(); ++i) {
                options.cage_command.emplace_back(args[i]);
            }
        } else {
            throw std::runtime_error("unknown option " + std::string(arg));
        }
    }
    if (options.socket.empty() || options.session_id == 0) {
        throw std::runtime_error("--socket and --session-id are required");
    }
    if (options.desktop != "test" && !app::parse_headless_kind(options.desktop)) {
        throw std::runtime_error("unknown desktop " + options.desktop);
    }
    return true;
}

/// The token from the inherited descriptor, or from the environment, which
/// is cleared at once so the compositor and its clients never see it.
std::optional<farland::server::broker::Token> read_token(int fd)
{
    std::string text;
    if (fd >= 0) {
        std::array<char, 128> buffer{};
        ssize_t n = 0;
        while ((n = ::read(fd, buffer.data(), buffer.size())) > 0 && text.size() < 256) {
            text.append(buffer.data(), static_cast<std::size_t>(n));
        }
        ::close(fd);
        farland::secure_zero(std::as_writable_bytes(std::span(buffer)));
    } else if (const char* value = std::getenv(app::agent_token_variable.data()); value != nullptr) {
        text = value;
    }
    ::unsetenv(app::agent_token_variable.data());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    auto token = app::token_from_hex(text);
    farland::secure_zero(std::as_writable_bytes(std::span(text)));
    return token;
}

farland::UniqueFd connect_to(const std::string& path)
{
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        throw std::runtime_error("socket path too long: " + path);
    }
    std::ranges::copy(path, std::begin(address.sun_path));
    farland::UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API takes a generic sockaddr
    if (!fd.valid() || ::connect(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        throw std::runtime_error("cannot connect to " + path + ": " + std::strerror(errno));
    }
    app::prepare_socket(fd.get());
    return fd;
}

}  // namespace

int main(int argc, char** argv)
{
    Options options;
    try {
        if (!parse(std::span(argv, static_cast<std::size_t>(argc)), options)) {
            return 0;
        }
    } catch (const std::exception& e) {
        std::cerr << "farland-agent: " << e.what() << "\n";
        usage();
        return 2;
    }
    const auto token = read_token(options.token_fd);
    if (!token) {
        std::cerr << "farland-agent: no valid session token (--token-fd or FARLAND_AGENT_TOKEN)\n";
        return 2;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    farland::agent::AgentConfig config;
    try {
        config.daemon = connect_to(options.socket);
    } catch (const std::exception& e) {
        std::cerr << "farland-agent: " << e.what() << "\n";
        return 1;
    }
    config.token = *token;
    config.logon_id = options.session_id;
    config.session = options.session;
    config.session.preauth.advertise_gfx = true;
    if (options.desktop == "test") {
        config.make_desktop =
            [](const farland::agent::DesktopRequest& request) -> farland::Result<std::unique_ptr<app::Desktop>> {
            return std::make_unique<app::TestDesktop>(request.width, request.height, request.frames_per_second);
        };
    } else {
        app::HeadlessOptions headless;
        headless.kind = *app::parse_headless_kind(options.desktop);
        headless.attach = options.attach;
        headless.cage_command = options.cage_command;
        config.make_desktop = [headless](const farland::agent::DesktopRequest& request) mutable {
            headless.width = request.width;
            headless.height = request.height;
            headless.render_node = request.render_node;
            return app::start_headless_desktop(headless);
        };
    }
    log::info(log_component, "session {}: {} desktop{}", options.session_id, options.desktop,
              options.attach ? " (attached)" : "");
    farland::agent::Agent agent(std::move(config));
    const auto reason = agent.run(stop_requested);
    const bool failed = reason == farland::server::broker::EndReason::desktop_failed ||
                        reason == farland::server::broker::EndReason::error;
    return failed ? 1 : 0;
}
