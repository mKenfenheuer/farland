// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// farland-server: an RDP server with NLA that shares the running Wayland
// desktop (--share), runs a headless one (--headless), or shows the
// synthetic test backend.

#include <farland/auth/credential_store.hpp>
#include <farland/auth/kerberos.hpp>
#include <farland/auth/ntlm.hpp>
#include <farland/auth/tls_identity.hpp>
#include <farland/base/log.hpp>
#ifdef FARLAND_HAVE_VAAPI
#include <farland/video/vaapi_encoder.hpp>
#endif

#include "nla.hpp"
#ifdef FARLAND_HAVE_PORTAL
#include "portal_desktop.hpp"
#endif
#include "headless.hpp"
#include "privsep_process.hpp"
#include "sandbox.hpp"
#include "session.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

namespace log = farland::log;
namespace app = farland::app;
using farland::app::SessionOptions;
constexpr std::string_view log_component = "app";

std::atomic<bool> stop_requested{
    false};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): signal handler state
std::atomic<int> active_sessions{0};  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

extern "C" void on_signal(int /*signal*/)
{
    stop_requested.store(true);
}

struct Options {
    std::string bind = "0.0.0.0";
    std::string port = "3389";
    std::filesystem::path cert;
    std::filesystem::path key;
    std::filesystem::path users;
    std::string hostname;
    std::string log_level = "info";
    bool print_fingerprint = false;
    bool privsep = true;
    /// Share the running desktop through xdg-desktop-portal.
    bool share = false;
    bool virtual_monitor = false;
    /// Start (or attach to) a headless compositor instead: sway, labwc or cage.
    std::optional<app::HeadlessOptions> headless;
    bool allow_tls_only = false;
    /// Accept Kerberos as well as NTLM: the keytab to accept with (empty
    /// when --keytab was not given), and the principal within it.
    std::optional<std::string> keytab;
    std::string service_principal;
    /// Internal: the monitor accepts Kerberos, so the network process may
    /// offer it. The network process never sees the keytab itself.
    bool kerberos = false;
    /// Refuse NTLM: only a client with a ticket gets in.
    bool kerberos_only = false;
    /// Internal: this process is the network process for one client.
    bool privsep_child = false;
    std::string peer;
    int max_sessions = 4;
    SessionOptions session;
};

std::filesystem::path config_dir()
{
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path(xdg) / "farland";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".config" / "farland";
    }
    return std::filesystem::current_path() / ".farland";
}

std::string local_hostname()
{
    std::string name(256, '\0');
    if (::gethostname(name.data(), name.size()) != 0) {
        return "localhost";
    }
    name.resize(name.find('\0'));
    return name.empty() ? "localhost" : name;
}

void usage()
{
    std::cout << "usage: farland-server [options]\n"
                 "  --bind ADDRESS        listen address (default 0.0.0.0)\n"
                 "  --port PORT           TCP port (default 3389)\n"
                 "  --cert FILE --key FILE  TLS certificate and key (PEM); created if both are missing\n"
                 "                        (default: $XDG_CONFIG_HOME/farland/tls/{cert,key}.pem)\n"
                 "  --users FILE          NLA users, managed with farlandctl passwd\n"
                 "                        (default: $XDG_CONFIG_HOME/farland/users)\n"
                 "  --hostname NAME       certificate host name (default: this host's name)\n"
                 "  --fps N               frame rate of the test pattern (default 30)\n"
                 "  --codec planar|raw    bitmap codec for 32 bpp sessions (default planar)\n"
                 "  --gfx-codec CODEC     progressive, planar, avc420 or avc444 for GFX clients (default progressive)\n"
                 "  --openh264 FILE       OpenH264 library for avc420/avc444 (default: libopenh264.so.8 and older)\n"
                 "  --h264-encoder NAME   auto, nvenc, vaapi, openh264 or x264 for avc420/avc444 (default auto:\n"
                 "                        NVENC on NVIDIA, VA-API where another GPU encodes H.264, else OpenH264)\n"
                 "  --render-node PATH    DRM render node for NVENC and VA-API (default: the first that works)\n"
                 "  --no-zero-copy        with --share: read every frame into CPU memory, even where the H.264\n"
                 "                        encoder takes the captured dmabufs directly (for comparisons and\n"
                 "                        driver problems)\n"
                 "  --no-clearcodec       Progressive surfaces: no ClearCodec for text and UI tiles\n"
                 "  --no-refine           Progressive surfaces: every tile at full quality at once, no\n"
                 "                        refinement passes\n"
                 "  --no-video-regions    Progressive surfaces: no H.264 for the tiles that keep changing\n"
                 "  --no-lossless-still   Progressive surfaces: do not resend a still picture losslessly\n"
                 "                        once it stands still\n"
                 "  --no-audio            do not play the desktop's audio on the client (rdpsnd)\n"
                 "  --no-microphone       do not offer the client's microphone as a local audio source (audin)\n"
                 "  --no-camera           do not offer the client's camera as a local camera (rdpecam)\n"
                 "  --autodetect MODE     network auto-detect for clients that support it: full (default;\n"
                 "                        also measures before licensing), continuous (only once connected) or off\n"
                 "  --max-sessions N      concurrent connections (default 4)\n"
                 "  --share               share the running Wayland desktop (xdg-desktop-portal, PipeWire, libei)\n"
                 "                        instead of the test pattern: every monitor picked in the portal dialog,\n"
                 "                        each on one of the client's monitors; one session at a time\n"
                 "  --virtual-monitor     with --share: share a new virtual monitor where the portal offers it,\n"
                 "                        sized to the client's monitor and resized with its window (GNOME)\n"
                 "  --headless KIND       run a headless desktop for this user instead of the test pattern:\n"
                 "                        gnome, plasma, sway, labwc or cage (cage runs the command after --,\n"
                 "                        e.g. --headless cage -- foot); stopped when farland-server exits\n"
                 "  --headless-size WxH   its first output's size until the client resizes it (default 1920x1080)\n"
                 "  --headless-layout L   XKB layout for its keymap, e.g. de or fr(azerty) (default us)\n"
                 "  --headless-attach     with --headless: use the compositor of this session ($WAYLAND_DISPLAY)\n"
                 "                        instead of starting one\n"
                 "  --no-clipboard        do not share the clipboard (text, HTML, images, files); without --share\n"
                 "                        the clipboard is a loopback that offers back what the client copies\n"
                 "  --allow-tls-only      also accept clients without NLA (anyone reaches the login screen)\n"
                 "  --keytab [FILE]       also accept Kerberos, with FILE (default: the system keytab)\n"
                 "  --service-principal P the principal in the keytab to accept as (default: any)\n"
                 "  --kerberos-only       with --keytab: refuse clients that have no ticket\n"
                 "  --no-privsep          handle clients in this process instead of a sandboxed one\n"
                 "  --log-level LEVEL     trace, debug, info, warn, error (default info)\n"
                 "  --fingerprint         print the certificate's SHA-256 fingerprint and exit\n";
}

bool parse_options(std::span<char*> args, Options& options)
{
    bool headless_given = false;  // --headless-* options apply only with it
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= args.size()) {
                throw std::runtime_error(std::string(arg) + " needs a value");
            }
            return args[++i];
        };
        if (arg == "--bind") {
            options.bind = value();
        } else if (arg == "--port") {
            options.port = value();
        } else if (arg == "--cert") {
            options.cert = value();
        } else if (arg == "--key") {
            options.key = value();
        } else if (arg == "--users") {
            options.users = value();
        } else if (arg == "--hostname") {
            options.hostname = value();
        } else if (arg == "--fps") {
            options.session.frames_per_second = static_cast<unsigned>(std::stoul(value()));
        } else if (arg == "--codec") {
            const auto codec = value();
            if (codec != "planar" && codec != "raw") {
                throw std::runtime_error("--codec must be planar or raw");
            }
            options.session.codec =
                codec == "planar" ? farland::server::BitmapCodec::planar : farland::server::BitmapCodec::uncompressed;
        } else if (arg == "--gfx-codec") {
            const auto codec = value();
            if (codec == "progressive") {
                options.session.gfx_codec = farland::server::TileCodec::progressive;
            } else if (codec == "planar") {
                options.session.gfx_codec = farland::server::TileCodec::planar;
            } else if (codec == "avc420" || codec == "h264") {
                options.session.gfx_codec = farland::server::TileCodec::avc420;
            } else if (codec == "avc444") {
                options.session.gfx_codec = farland::server::TileCodec::avc444;
            } else {
                throw std::runtime_error("--gfx-codec must be progressive, planar, avc420 or avc444");
            }
        } else if (arg == "--openh264") {
            options.session.openh264_library = value();
        } else if (arg == "--h264-encoder") {
            const auto name = value();
            if (name == "auto") {
                options.session.h264_backend.reset();
            } else if (const auto backend = farland::video::parse_backend(name)) {
                options.session.h264_backend = *backend;
            } else {
                throw std::runtime_error("--h264-encoder must be auto, nvenc, vaapi, openh264 or x264");
            }
        } else if (arg == "--render-node") {
            options.session.render_node = value();
        } else if (arg == "--no-zero-copy") {
            options.session.zero_copy = false;
        } else if (arg == "--no-clearcodec") {
            options.session.clearcodec = false;
        } else if (arg == "--no-refine") {
            options.session.refine = false;
        } else if (arg == "--no-video-regions") {
            options.session.video_regions = false;
        } else if (arg == "--no-lossless-still") {
            options.session.lossless_still = false;
        } else if (arg == "--no-audio") {
            options.session.audio = false;
        } else if (arg == "--no-microphone") {
            options.session.microphone = false;
        } else if (arg == "--no-camera") {
            options.session.camera = false;
        } else if (arg == "--autodetect") {
            const auto mode = value();
            if (mode == "full") {
                options.session.autodetect = farland::server::AutoDetectMode::full;
            } else if (mode == "continuous") {
                options.session.autodetect = farland::server::AutoDetectMode::continuous;
            } else if (mode == "off") {
                options.session.autodetect = farland::server::AutoDetectMode::off;
            } else {
                throw std::runtime_error("--autodetect must be full, continuous or off");
            }
        } else if (arg == "--max-sessions") {
            options.max_sessions = std::stoi(value());
        } else if (arg == "--keytab") {
            // The path is optional: bare --keytab means the system default,
            // which is what KRB5_KTNAME or /etc/krb5.keytab names.
            if (i + 1 < args.size() && !std::string_view(args[i + 1]).starts_with("--")) {
                options.keytab = args[++i];
            } else {
                options.keytab = std::string{};
            }
        } else if (arg == "--service-principal") {
            options.service_principal = value();
        } else if (arg == "--kerberos") {
            options.kerberos = true;
        } else if (arg == "--kerberos-only") {
            options.kerberos_only = true;
        } else if (arg == "--allow-tls-only") {
            options.allow_tls_only = true;
            options.session.preauth.require_nla = false;
        } else if (arg == "--share") {
            options.share = true;
        } else if (arg == "--virtual-monitor") {
            options.share = true;
            options.virtual_monitor = true;
        } else if (arg == "--headless") {
            const auto kind = app::parse_headless_kind(value());
            if (!kind) {
                throw std::runtime_error("--headless must be gnome, plasma, sway, labwc or cage");
            }
            auto& headless = options.headless ? *options.headless : options.headless.emplace();
            headless.kind = *kind;
            headless_given = true;
        } else if (arg == "--headless-size") {
            const auto size = value();
            auto& headless = options.headless ? *options.headless : options.headless.emplace();
            if (std::sscanf(size.c_str(), "%ux%u", &headless.width, &headless.height) != 2 || headless.width < 200 ||
                headless.height < 200 || headless.width > 8192 || headless.height > 8192) {
                throw std::runtime_error("--headless-size must be WIDTHxHEIGHT, 200 to 8192 each");
            }
        } else if (arg == "--headless-layout") {
            auto& headless = options.headless ? *options.headless : options.headless.emplace();
            headless.keymap_layout = value();
        } else if (arg == "--headless-attach") {
            auto& headless = options.headless ? *options.headless : options.headless.emplace();
            headless.attach = true;
        } else if (arg == "--") {
            auto& headless = options.headless ? *options.headless : options.headless.emplace();
            headless.cage_command.assign(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
            break;
        } else if (arg == "--no-clipboard") {
            options.session.clipboard = false;
        } else if (arg == "--no-privsep") {
            options.privsep = false;
        } else if (arg == "--privsep-child") {
            options.privsep_child = true;
        } else if (arg == "--peer") {
            options.peer = value();
        } else if (arg == "--log-level") {
            options.log_level = value();
            static constexpr std::array levels{"trace", "debug", "info", "warn", "error"};
            const auto* found = std::ranges::find(levels, options.log_level);
            if (found == levels.end()) {
                throw std::runtime_error("unknown log level " + options.log_level);
            }
            log::set_level(static_cast<log::Level>(found - levels.begin()));
        } else if (arg == "--fingerprint") {
            options.print_fingerprint = true;
        } else if (arg == "--help" || arg == "-h") {
            usage();
            return false;
        } else {
            throw std::runtime_error("unknown option " + std::string(arg));
        }
    }
    if (options.headless && !headless_given) {
        throw std::runtime_error("--headless-size, --headless-layout, --headless-attach and -- need --headless");
    }
    if (options.headless && options.share) {
        throw std::runtime_error("--headless and --share exclude each other");
    }
    if (options.headless && options.headless->kind == app::HeadlessKind::cage &&
        options.headless->cage_command.empty() && !options.headless->attach) {
        throw std::runtime_error("--headless cage needs the application to run after --");
    }
    if (options.cert.empty() != options.key.empty()) {
        throw std::runtime_error("--cert and --key go together");
    }
    if (options.cert.empty()) {
        options.cert = config_dir() / "tls" / "cert.pem";
        options.key = config_dir() / "tls" / "key.pem";
    }
    if (options.users.empty()) {
        options.users = config_dir() / "users";
    }
    if (options.hostname.empty()) {
        options.hostname = local_hostname();
    }
    return true;
}

/// The arguments that give the network process the monitor's configuration.
app::ChildLaunch child_launch(const Options& options, const char* argv0)
{
    app::ChildLaunch launch{app::current_executable(argv0), {}};
    launch.arguments = {"--cert",     options.cert.string(), "--key",       options.key.string(),
                        "--hostname", options.hostname,      "--log-level", options.log_level};
    if (options.allow_tls_only) {
        launch.arguments.emplace_back("--allow-tls-only");
    }
    // Only that Kerberos is on offer, never the keytab: the network process
    // is sandboxed out of the file system and must not hold the host key.
    if (options.keytab) {
        launch.arguments.emplace_back("--kerberos");
        if (options.kerberos_only) {
            launch.arguments.emplace_back("--kerberos-only");
        }
    }
    return launch;
}

int listen_on(const Options& options)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* result = nullptr;
    if (const int rc = ::getaddrinfo(options.bind.c_str(), options.port.c_str(), &hints, &result); rc != 0) {
        throw std::runtime_error("cannot resolve " + options.bind + ": " + ::gai_strerror(rc));
    }
    const int fd = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    const int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    const bool ok = fd >= 0 && ::bind(fd, result->ai_addr, result->ai_addrlen) == 0 && ::listen(fd, 16) == 0;
    ::freeaddrinfo(result);
    if (!ok) {
        throw std::runtime_error("cannot listen on " + options.bind + ":" + options.port + ": " + std::strerror(errno));
    }
    return fd;
}

std::string peer_name(const sockaddr_storage& address)
{
    std::string host(NI_MAXHOST, '\0');
    std::string port(NI_MAXSERV, '\0');
    if (::getnameinfo(reinterpret_cast<const sockaddr*>(&address), sizeof(address), host.data(),
                      static_cast<socklen_t>(host.size()), port.data(), static_cast<socklen_t>(port.size()),
                      NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return "unknown peer";
    }
    host.resize(host.find('\0'));
    port.resize(port.find('\0'));
    return host + ":" + port;
}

/// Checks NTLM responses against the credential store, read afresh for every
/// connection so that farlandctl changes apply without a restart.
class StoreVerifier final : public farland::auth::NtlmVerifier {
public:
    explicit StoreVerifier(farland::auth::CredentialStore store)
        : store_(std::move(store)),
          verifier_([this](std::string_view user, std::string_view domain) { return store_.lookup(user, domain); })
    {
    }

    std::optional<std::array<std::byte, 16>> session_base_key(std::string_view user, std::string_view domain,
                                                              std::span<const std::byte, 8> challenge,
                                                              std::span<const std::byte> response) override
    {
        return verifier_.session_base_key(user, domain, challenge, response);
    }
    bool verify_password(std::string_view user, std::string_view domain, std::string_view password) override
    {
        return verifier_.verify_password(user, domain, password);
    }

private:
    farland::auth::CredentialStore store_;
    farland::auth::ntlm::LocalNtlmVerifier verifier_;
};

/// The network process: one client, sandboxed.
int run_child(const Options& options)
{
    std::signal(SIGPIPE, SIG_IGN);
    auto identity = farland::auth::TlsIdentity::load_or_create(options.cert, options.key, options.hostname);
    if (!identity) {
        log::error(log_component, "cannot load the TLS identity: {}", identity.error().message());
        return 1;
    }
    if (!app::enter_network_sandbox()) {
        return 1;
    }
    const auto& tls = *identity;
    return app::run_network_child(
        options.peer, tls, options.session,
        [&](const app::NlaBackends& backends) { return app::make_nla_factory(tls, backends, options.hostname); },
        options.kerberos, options.kerberos_only);
}

}  // namespace

int main(int argc, char** argv)
{
    Options options;
    try {
        if (!parse_options(std::span(argv, static_cast<std::size_t>(argc)), options)) {
            return 0;
        }
    } catch (const std::exception& e) {
        std::cerr << "farland-server: " << e.what() << "\n";
        usage();
        return 2;
    }

    // This build can serve the Graphics Pipeline (planar tiles over RDPGFX).
    options.session.preauth.advertise_gfx = true;

    if (options.privsep_child) {
        return run_child(options);
    }

    auto identity = farland::auth::TlsIdentity::load_or_create(options.cert, options.key, options.hostname);
    if (!identity) {
        std::cerr << "farland-server: cannot load or create the TLS identity: " << identity.error().message() << "\n";
        return 1;
    }
    if (options.print_fingerprint) {
        std::cout << identity->sha256_fingerprint() << "\n";
        return 0;
    }
    const auto launch = child_launch(options, argv[0]);
#ifdef FARLAND_HAVE_PORTAL
    // Declared before the listener and the session threads, destroyed after them.
    std::unique_ptr<app::Desktop> desktop;
    // The capture offers the dmabuf layouts of the encoder's GPU, so it
    // needs the encoder's render node.
    std::string render_node = options.session.render_node;
#ifdef FARLAND_HAVE_VAAPI
    const auto backend = options.session.h264_backend;
    if ((options.share || options.headless) && render_node.empty() &&
        options.session.gfx_codec == farland::server::TileCodec::avc420 &&
        (!backend || *backend == farland::video::Backend::vaapi)) {
        if (const auto device = farland::video::vaapi::probe()) {
            render_node = device->render_node;
        }
    }
#endif
    if (options.share) {
        auto shared = app::start_portal_desktop(app::PortalDesktopOptions{
            .virtual_monitor = options.virtual_monitor,
            .restore_token_file = std::nullopt,
            .timeout = std::chrono::seconds(300),
            .render_node = render_node,
            .clipboard = options.session.clipboard,
        });
        if (!shared) {
            std::cerr << "farland-server: cannot share the desktop: " << shared.error().message() << "\n";
            return 1;
        }
        desktop = std::move(*shared);
        options.session.desktop = desktop.get();
        options.max_sessions = 1;  // one session drives the desktop at a time
    }
#else
    if (options.share) {
        std::cerr << "farland-server: --share needs the portal backend (Linux with sd-bus, PipeWire and libei), "
                     "which this build does not have\n";
        return 2;
    }
#endif
    std::unique_ptr<app::Desktop> headless_desktop;
    if (options.headless) {
        auto headless = *options.headless;
        headless.render_node = options.session.render_node;
        auto started = app::start_headless_desktop(headless);
        if (!started) {
            std::cerr << "farland-server: cannot start the headless desktop: " << started.error().message() << "\n";
            return 1;
        }
        headless_desktop = std::move(*started);
        options.session.desktop = headless_desktop.get();
        options.max_sessions = 1;  // one session drives the desktop at a time
    }
    if (const auto users = farland::auth::CredentialStore::load(options.users); !users) {
        std::cerr << "farland-server: cannot read the NLA user store " << options.users.string() << "\n";
        return 1;
    } else if (users->entries().empty()) {
        log::warn(log_component, "no NLA users in {}; add one with: farlandctl passwd USER", options.users.string());
    }

    std::optional<farland::auth::kerberos::Credential> kerberos;
    if (options.keytab) {
        auto credential = farland::auth::kerberos::Credential::acquire(
            {.keytab = *options.keytab, .service_principal = options.service_principal});
        if (!credential) {
            std::cerr << "farland-server: " << credential.error().message() << "\n";
            return 1;
        }
        kerberos = std::move(*credential);
        log::info(log_component, "Kerberos is accepted as {} (keytab {})", kerberos->principal(),
                  options.keytab->empty() ? "the system default" : *options.keytab);
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    int listener = -1;
    try {
        listener = listen_on(options);
    } catch (const std::exception& e) {
        std::cerr << "farland-server: " << e.what() << "\n";
        return 1;
    }
    log::info(log_component, "listening on {}:{}, certificate SHA-256 {}{}", options.bind, options.port,
              identity->sha256_fingerprint(), options.privsep ? "" : " (privilege separation off)");

    while (!stop_requested.load()) {
        pollfd pfd{listener, POLLIN, 0};
        if (::poll(&pfd, 1, 500) <= 0) {
            continue;
        }
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        const int fd = ::accept(listener, reinterpret_cast<sockaddr*>(&address), &length);
        if (fd < 0) {
            continue;
        }
        app::prepare_socket(fd);
        const std::string peer = peer_name(address);
        if (active_sessions.load() >= options.max_sessions) {
            log::warn(log_component, "{}: refused, {} sessions already running", peer, options.max_sessions);
            ::close(fd);
            continue;
        }
        log::info(log_component, "{}: connected", peer);
        ++active_sessions;
        std::thread([fd, peer, &identity, &options, &launch, &kerberos] {
            if (options.privsep) {
                auto store = farland::auth::CredentialStore::load(options.users);
                if (!store) {
                    log::error(log_component, "{}: refused, the NLA user store cannot be read", peer);
                    ::close(fd);
                } else {
                    StoreVerifier verifier(std::move(*store));
                    app::run_monitored_session(fd, peer, launch, verifier, options.session, stop_requested,
                                               kerberos ? &*kerberos : nullptr);
                }
            } else if (auto store = farland::auth::CredentialStore::load(options.users); !store) {
                log::error(log_component, "{}: refused, the NLA user store cannot be read", peer);
                ::close(fd);
            } else {
                StoreVerifier verifier(std::move(*store));
                auto session = options.session;
                // No privilege separation, so no monitor to ask: the
                // Kerberos context runs right here.
                const app::NlaBackends backends{.verifier = verifier,
                                                .monitor = {},
                                                .kerberos = false,
                                                .credential = kerberos ? &*kerberos : nullptr,
                                                .kerberos_only = options.kerberos_only};
                session.make_nla = app::make_nla_factory(*identity, backends, options.hostname);
                app::run_session(fd, peer, *identity, session, stop_requested);
            }
            --active_sessions;
        }).detach();
    }

    log::info(log_component, "shutting down");
    ::close(listener);
    while (active_sessions.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return 0;
}
