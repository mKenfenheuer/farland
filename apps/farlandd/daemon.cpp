// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "daemon.hpp"

#include <farland/auth/credential_store.hpp>
#include <farland/auth/kerberos.hpp>
#include <farland/auth/ntlm.hpp>
#include <farland/auth/tls_identity.hpp>
#include <farland/base/log.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/text.hpp>
#include <farland/proto/framing.hpp>
#include <farland/proto/gcc.hpp>
#include <farland/proto/mcs.hpp>
#include <farland/server/broker.hpp>
#include <farland/server/connection.hpp>

#include "agent_token.hpp"
#include "consent.hpp"
#include "enrol.hpp"
#include "launcher.hpp"
#include "logind.hpp"
#include "metrics.hpp"
#include "privsep_process.hpp"
#include "registry.hpp"
#include "sandbox.hpp"
#include "seat_takeover.hpp"
#include "session_store.hpp"
#include "session_view.hpp"
#include "transport.hpp"
#include "unix_socket.hpp"

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <format>
#include <functional>
#include <mutex>
#include <netdb.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <variant>

namespace farland::daemon {

namespace {

namespace broker = server::broker;
using Clock = std::chrono::steady_clock;
constexpr std::string_view log_component = "daemon";

/// The Set Error Info code a refusal carried, as a name a metrics label can
/// hold: a handful of values rather than a number a dashboard has to look up.
[[nodiscard]] std::string errinfo_name(std::uint32_t error_info)
{
    switch (error_info) {
    case proto::errinfo::server_denied_connection:
        return "denied";
    case proto::errinfo::server_insufficient_privileges:
        return "insufficient_privileges";
    case proto::errinfo::out_of_memory:
        return "out_of_memory";
    case proto::errinfo::logon_timeout:
        return "logon_timeout";
    case proto::errinfo::idle_timeout:
        return "idle_timeout";
    case proto::errinfo::disconnected_by_other_connection:
        return "disconnected_by_other_connection";
    default:
        return std::format("{:#010x}", error_info);
    }
}
/// Clients in pre-authentication at once; more are turned away.
constexpr int max_preauth_clients = 64;
/// How long a new session's agent may take to say Hello.
constexpr auto start_timeout = std::chrono::seconds(90);
/// How long an agent may take to end after Terminate.
constexpr auto terminate_grace = std::chrono::seconds(15);
constexpr auto hello_timeout = std::chrono::seconds(10);
/// A question about somebody logging in at the machine runs under a
/// connection id of its own, above every id a client will ever have
/// ([policy] seat_takeover).
constexpr std::uint64_t seat_connection_bit = std::uint64_t{1} << 63;
/// How long a session that outlived a restart waits for its agent to come
/// back before it is given up. The agent tries for as long (agent.cpp), so
/// whichever notices first ends it cleanly.
constexpr auto reattach_window = std::chrono::seconds(120);

/// How long an agent may keep farlandd waiting to take a message.
constexpr int agent_send_timeout_ms = 5000;
constexpr std::size_t max_identity_name = 1024;  ///< broker's limit for user and domain

/// Checks NTLM responses against the credential store, read afresh for
/// every connection so that farlandctl changes apply at once.
class StoreVerifier final : public auth::NtlmVerifier {
public:
    explicit StoreVerifier(auth::CredentialStore store)
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
    [[nodiscard]] const auth::CredentialStore& store() const noexcept { return store_; }

private:
    auth::CredentialStore store_;
    auth::ntlm::LocalNtlmVerifier verifier_;
};

/// A client through NLA, on its way to a session.
struct Authenticated {
    std::uint64_t id = 0;
    std::string peer;
    std::string account;
    server::Negotiation negotiation;
    UniqueFd plain;
    Clock::time_point accepted;
};

/// A session launch that ran on its own thread (GDM, the user's manager).
struct Launched {
    std::uint32_t session = 0;
    bool ok = false;
    std::string login_session;
    std::string account;
    bool gdm = false;
};

using Posted = std::variant<Authenticated, Launched>;

/// Where client and launch threads leave their results for the main loop.
/// Shared with the threads, so it outlives the daemon if one lingers.
class Mailbox {
public:
    Mailbox()
    {
        std::array<int, 2> fds{-1, -1};
        if (::pipe(fds.data()) == 0) {
            read_.reset(fds[0]);
            write_.reset(fds[1]);
            for (const int fd : fds) {
                ::fcntl(fd, F_SETFD, FD_CLOEXEC);
                ::fcntl(fd, F_SETFL, O_NONBLOCK);
            }
        }
    }

    void post(Posted item)
    {
        const std::scoped_lock lock(mutex_);
        if (closed_) {
            return;  // a late thread after shutdown; the item and its descriptor go
        }
        items_.push_back(std::move(item));
        const char byte = 1;
        [[maybe_unused]] const ssize_t written = ::write(write_.get(), &byte, 1);  // full pipe: already woken
    }

    std::deque<Posted> take()
    {
        std::array<char, 64> drain{};
        while (::read(read_.get(), drain.data(), drain.size()) > 0) {
        }
        const std::scoped_lock lock(mutex_);
        return std::exchange(items_, {});
    }

    void close()
    {
        const std::scoped_lock lock(mutex_);
        closed_ = true;
        items_.clear();
    }

    [[nodiscard]] int fd() const noexcept { return read_.get(); }

    std::atomic<int> client_threads{0};
    std::atomic<bool> stopping{false};

private:
    std::mutex mutex_;
    std::deque<Posted> items_;
    bool closed_ = false;
    UniqueFd read_;
    UniqueFd write_;
};

/// A local process that connected to the agent socket and has not said Hello yet.
struct AgentPeer {
    UniqueFd fd;
    uid_t uid = 0;
    std::optional<pid_t> pid;
    std::vector<std::byte> inbox;
    Clock::time_point since;
};

/// One session: its agent, the process farlandd started for it, and a
/// connection waiting for the agent.
struct Live {
    std::uint32_t id = 0;
    std::string account;
    std::optional<Account> user;
    broker::Token token{};
    std::unique_ptr<broker::AgentLink> link;
    UniqueFd agent;
    std::vector<std::byte> inbox;
    pid_t process = -1;         ///< the agent (no PAM) or session helper; -1: none of ours
    std::string login_session;  ///< GDM's or the attached local session
    bool gdm = false;           ///< GDM made the login session; end it with the agent
    std::string unit;           ///< the agent's unit in the user's manager
    std::optional<Authenticated> waiting;
    /// Held while whoever has the session is asked about the takeover
    /// ([policy] takeover).
    std::optional<Authenticated> asking;
    Clock::time_point created;
    /// The address of the client connected now, for `farlandctl sessions`.
    std::string peer;
    /// The agent's last Stats, for the metrics.
    broker::Stats stats;
    /// The token the agent greets with again after a farlandd restart. The
    /// greeting copy is wiped once used; this one lives as long as the
    /// session, and is what the written-down table holds.
    broker::Token reattach_token{};
    /// It outlived a farlandd restart and its agent has not come back yet;
    /// after this it is given up for lost.
    std::optional<Clock::time_point> reattach_deadline;
    std::optional<Clock::time_point> terminate_sent;
    /// Why this session was asked to end, as a metrics label. It is set where
    /// the Terminate is sent, because by the time the agent goes the only
    /// thing left to see is that it went.
    std::string end_tag;

    Live() = default;
    Live(const Live&) = delete;
    Live& operator=(const Live&) = delete;
    Live(Live&&) = delete;
    Live& operator=(Live&&) = delete;
    ~Live() { secure_zero(token); }
};

std::string peer_name(const sockaddr_storage& address)
{
    std::string host(NI_MAXHOST, '\0');
    std::string port(NI_MAXSERV, '\0');
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API takes a generic sockaddr
    if (::getnameinfo(reinterpret_cast<const sockaddr*>(&address), sizeof(address), host.data(),
                      static_cast<socklen_t>(host.size()), port.data(), static_cast<socklen_t>(port.size()),
                      NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return "unknown peer";
    }
    host.resize(host.find('\0'));
    port.resize(port.find('\0'));
    return host.find(':') != std::string::npos ? "[" + host + "]:" + port : host + ":" + port;
}

Result<UniqueFd> listen_tcp(const std::string& bind, std::uint16_t port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    if (::getaddrinfo(bind.c_str(), service.c_str(), &hints, &result) != 0 || result == nullptr) {
        return fail(Errc::io, "cannot resolve the bind address");
    }
    UniqueFd fd(::socket(result->ai_family, result->ai_socktype, result->ai_protocol));
    const int yes = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    const bool ok =
        fd.valid() && ::bind(fd.get(), result->ai_addr, result->ai_addrlen) == 0 && ::listen(fd.get(), 32) == 0;
    ::freeaddrinfo(result);
    if (!ok) {
        log::error(log_component, "cannot listen on {}:{}: {}", bind, port, std::strerror(errno));
        return fail(Errc::io, "cannot listen");
    }
    ::fcntl(fd.get(), F_SETFD, FD_CLOEXEC);
    return fd;
}

Result<UniqueFd> listen_unix(const std::filesystem::path& path)
{
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string text = path.string();
    if (text.size() >= sizeof(address.sun_path)) {
        return fail(Errc::invalid_value, "agent socket path too long");
    }
    std::ranges::copy(text, std::begin(address.sun_path));
    ::unlink(text.c_str());
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API takes a generic sockaddr
    if (!fd.valid() || ::bind(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(fd.get(), 16) != 0) {
        log::error(log_component, "cannot listen on {}: {}", text, std::strerror(errno));
        return fail(Errc::io, "cannot listen on the agent socket");
    }
    // Every user's agent connects; the token and the peer's uid decide.
    ::chmod(text.c_str(), 0666);
    ::fcntl(fd.get(), F_SETFD, FD_CLOEXEC);
    return fd;
}

void set_nonblocking(int fd)
{
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
}

/// Appends what is readable; false at end of file or on an error.
bool read_available(int fd, std::vector<std::byte>& inbox)
{
    std::array<std::byte, 16384> chunk{};
    while (true) {
        const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
        if (n > 0) {
            inbox.insert(inbox.end(), chunk.begin(), chunk.begin() + n);
            if (inbox.size() > broker::max_message_size * 4) {
                return false;  // an agent that floods is dropped
            }
            continue;
        }
        if (n < 0 && (errno == EINTR)) {
            continue;
        }
        return n < 0 && app::would_block(errno);
    }
}

/// The next complete frame out of `inbox`; nullopt until one is there,
/// an error for a bad length.
Result<std::optional<std::vector<std::byte>>> next_frame(std::vector<std::byte>& inbox)
{
    FARLAND_TRY(const auto length, broker::message_length(inbox));
    if (!length) {
        return std::nullopt;
    }
    std::vector<std::byte> frame(inbox.begin(), inbox.begin() + static_cast<std::ptrdiff_t>(*length));
    inbox.erase(inbox.begin(), inbox.begin() + static_cast<std::ptrdiff_t>(*length));
    return frame;
}

bool tokens_equal(const broker::Token& a, const broker::Token& b)
{
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

/// The user as NLA named them, for the log and the takeover prompt.
std::string user_name(const auth::Identity& identity)
{
    return identity.domain.empty() ? identity.user : identity.domain + "\\" + identity.user;
}

std::string account_of(const StoreVerifier& verifier, const auth::Identity& identity)
{
    if (const auto* entry = verifier.store().find(identity.user, identity.domain)) {
        return std::string(entry->account());
    }
    return identity.user;
}

server::TileCodec tile_codec_of(GfxCodec codec)
{
    switch (codec) {
    case GfxCodec::progressive:
        return server::TileCodec::progressive;
    case GfxCodec::planar:
        return server::TileCodec::planar;
    case GfxCodec::avc420:
        return server::TileCodec::avc420;
    case GfxCodec::avc444:
        return server::TileCodec::avc444;
    }
    return server::TileCodec::progressive;
}

std::optional<video::Backend> h264_backend_of(H264Encoder encoder)
{
    switch (encoder) {
    case H264Encoder::automatic:
        return std::nullopt;
    case H264Encoder::nvenc:
        return video::Backend::nvenc;
    case H264Encoder::vaapi:
        return video::Backend::vaapi;
    case H264Encoder::openh264:
        return video::Backend::openh264;
    case H264Encoder::x264:
        return video::Backend::x264;
    }
    return std::nullopt;
}

server::AutoDetectMode autodetect_of(AutoDetect autodetect)
{
    switch (autodetect) {
    case AutoDetect::off:
        return server::AutoDetectMode::off;
    case AutoDetect::continuous:
        return server::AutoDetectMode::continuous;
    case AutoDetect::full:
        return server::AutoDetectMode::full;
    }
    return server::AutoDetectMode::full;
}

/// The client's own name, out of the Connect Initial the client has
/// already sent ([MS-RDPBCGR] 2.2.1.3.2). MSG_PEEK leaves the bytes for the
/// agent, and nothing waits: an empty name only leaves the prompt a line
/// shorter.
std::string peek_client_name(int fd)
{
    std::array<std::byte, 4096> buffer{};
    const ssize_t received = ::recv(fd, buffer.data(), buffer.size(), MSG_PEEK | MSG_DONTWAIT);
    if (received <= 0) {
        return {};
    }
    const std::span pdu(buffer.data(), static_cast<std::size_t>(received));
    const auto frame = proto::peek_frame(pdu);
    if (!frame || !*frame || (*frame)->length > pdu.size()) {
        return {};
    }
    Reader r(pdu.first((*frame)->length));
    auto tpdu = proto::read_tpkt(r);
    if (!tpdu) {
        return {};
    }
    auto data = proto::decode_data_tpdu(*tpdu);
    if (!data) {
        return {};
    }
    const auto initial = proto::mcs::decode_connect_initial(*data);
    if (!initial) {
        return {};
    }
    Reader user_data(initial->user_data);
    const auto blocks = proto::gcc::decode_conference_create_request(user_data);
    if (!blocks) {
        return {};
    }
    Reader block_reader(*blocks);
    const auto client = proto::gcc::decode_client_data(block_reader);
    if (!client) {
        return {};
    }
    return client->core.client_name.substr(0, 64);
}

/// What the agent needs from the configuration (broker::Settings).
broker::Settings settings_of(const Config& config)
{
    broker::Settings settings;
    settings.frames_per_second = static_cast<std::uint16_t>(config.graphics.frames_per_second);
    settings.bitmap_codec = config.graphics.bitmap_codec == BitmapCodec::planar ? server::BitmapCodec::planar
                                                                                : server::BitmapCodec::uncompressed;
    settings.gfx_codec = tile_codec_of(config.graphics.gfx_codec);
    settings.h264_backend = h264_backend_of(config.graphics.h264_encoder);
    settings.openh264_library = config.graphics.openh264.value_or("");
    settings.render_node = config.graphics.render_node ? config.graphics.render_node->string() : std::string();
    settings.zero_copy = config.graphics.zero_copy;
    settings.clearcodec = config.graphics.clearcodec;
    settings.refine = config.graphics.refine;
    settings.video_regions = config.graphics.video_regions;
    settings.lossless_still = config.graphics.lossless_still;
    settings.audio = config.audio.playback;
    settings.microphone = config.audio.microphone;
    settings.camera = config.camera.enabled;
    settings.clipboard = config.clipboard.enabled;
    settings.autodetect = autodetect_of(config.network.autodetect);
    settings.activation_seconds = static_cast<std::uint32_t>(config.policy.activation_timeout.count());
    return settings;
}

}  // namespace

struct Daemon::Impl {
    explicit Impl(DaemonOptions o)
        : options(std::move(o)), registry(options.config.policy), consent(options.config.policy)
    {
    }

    int run(const std::atomic<bool>& stop);

    bool setup();
    void accept_client();
    void accept_agent();
    void read_unclaimed(AgentPeer& peer, bool& drop);
    void read_agent(Live& session);
    void handle(Authenticated client);
    void handle(Launched launched);
    /// `replace_local`: end the account's local session first, and give
    /// the client a headless one of its own.
    void start_session(Authenticated client, const std::string& account, const std::optional<Account>& user,
                       bool attach, bool replace_local = false, bool own_shell = false);
    /// Applies [policy] takeover, then hands the connection over or holds
    /// it while the session's user is asked.
    void deliver(Live& session, Authenticated client);
    /// Sends the connection and its socket to the session's agent.
    void hand_over(Live& session, Authenticated client);
    void ask_consent(Live& session, Authenticated client);
    /// [policy] seat_takeover: the logins at the machine waiting to be
    /// asked about, and what the accounts holding a session are now.
    void ask_the_seat();
    /// Acts on one answered, timed-out or withdrawn question.
    void resolve(const ConsentBroker::Resolved& resolved);
    /// Every question that has an answer by now.
    void settle();
    void refuse(Authenticated client, std::uint32_t error_info, const std::string& reason);
    /// `wait`: take the login session down before returning (at shutdown,
    /// when no detached thread would outlive the process).
    void end_session(Live& session, const std::string& why, std::string_view tag, bool wait = false);
    void tick();
    /// Writes the sessions down so that they outlive this process.
    void remember_sessions();
    /// Picks up the sessions of a farlandd that went away, whose agents are
    /// still running; they reconnect and greet with the token they kept.
    void recover_sessions();
    [[nodiscard]] std::vector<SessionView::Entry> describe_sessions(Clock::time_point now) const;
    void shutdown();
    [[nodiscard]] bool send_to(Live& session, const broker::Message& message, int fd = -1);
    Live* find(std::uint32_t id);

    DaemonOptions options;
    SessionRegistry registry;
    ConsentBroker consent;
    /// Questions that are over, waiting for the loop to act on them.
    std::vector<ConsentBroker::Resolved> resolutions;
    /// [policy] seat_takeover: the logins at the machine being asked about,
    /// by the connection id their question runs under.
    SeatTakeoverGate seat_gate;
    /// What `farlandctl sessions` and `farlandctl terminate` see and ask for.
    SessionView session_view;
    /// Where the sessions are written down, so a restart can pick them up.
    std::filesystem::path session_table;
    /// The Kerberos acceptor credential, where a keytab is configured. It is
    /// shared rather than owned because a connection's thread may outlive
    /// the daemon, and the monitor side of that thread needs it.
    std::shared_ptr<const auth::kerberos::Credential> kerberos;
    Metrics metrics;
    std::unique_ptr<MetricsServer> metrics_server;
    std::map<std::uint64_t, std::uint64_t> seat_questions;  ///< connection -> cookie
    std::uint64_t next_seat_connection = seat_connection_bit;
    app::ChildLaunch network_launch;
    app::SessionOptions preauth;
    /// [graphics], [network], [audio] and [clipboard] for every agent.
    broker::Settings settings;
    std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
    UniqueFd listener;
    UniqueFd agent_listener;
    std::filesystem::path socket_path;
    std::vector<AgentPeer> unclaimed;
    std::vector<std::unique_ptr<Live>> live;
    std::vector<pid_t> orphans;  ///< ended processes of ours still to reap
    std::uint64_t next_connection = 1;
    bool shutting_down = false;
    /// org.farland.Farland1 for self-enrolment (not in development mode).
    std::unique_ptr<ControlService> control;
};

Daemon::Daemon(DaemonOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

Daemon::~Daemon() = default;

int Daemon::run(const std::atomic<bool>& stop)
{
    return impl_->run(stop);
}

bool Daemon::Impl::setup()
{
    const auto& config = options.config;
    std::filesystem::path cert = options.state_dir / "tls" / "cert.pem";
    std::filesystem::path key = options.state_dir / "tls" / "key.pem";
    if (config.server.certificate && config.server.private_key) {
        cert = *config.server.certificate;
        key = *config.server.private_key;
    }
    std::error_code ec;
    std::filesystem::create_directories(options.state_dir, ec);
    auto identity = auth::TlsIdentity::load_or_create(cert, key, options.hostname);
    if (!identity) {
        log::error(log_component, "cannot load or create the TLS identity {}: {}", cert.string(),
                   identity.error().message());
        return false;
    }
    if (auto store = auth::CredentialStore::load(config.auth.credential_store); !store) {
        log::error(log_component, "cannot read the credential store {}: {}", config.auth.credential_store.string(),
                   store.error().message());
        return false;
    } else if (store->entries().empty()) {
        log::warn(log_component, "no users in {}; add one with farlandctl passwd",
                  config.auth.credential_store.string());
    }
    settings = settings_of(config);
    log::info(log_component, "session settings: {}", broker::describe(settings));
    if (config.auth.keytab) {
        auto credential = auth::kerberos::Credential::acquire(
            {.keytab = config.auth.keytab->string(), .service_principal = config.auth.service_principal});
        if (!credential) {
            log::error(log_component, "cannot accept Kerberos: {}", credential.error().message());
            return false;
        }
        kerberos = std::make_shared<const auth::kerberos::Credential>(std::move(*credential));
        log::info(log_component, "Kerberos is accepted as {} (keytab {})", kerberos->principal(),
                  config.auth.keytab->empty() ? "the system default" : config.auth.keytab->string());
    }
    network_launch =
        app::ChildLaunch{options.self,
                         {"--cert", cert.string(), "--key", key.string(), "--hostname", options.hostname, "--log-level",
                          options.log_level, "--activation-timeout", std::to_string(settings.activation_seconds)}};
    // Only that it is on offer: the keytab stays here, out of the sandbox.
    if (kerberos) {
        network_launch.arguments.emplace_back("--kerberos");
        if (config.auth.mode == AuthMode::kerberos) {
            network_launch.arguments.emplace_back("--kerberos-only");
        }
    }
    preauth.preauth.require_nla = true;
    preauth.preauth.advertise_gfx = true;
    preauth.activation_timeout = settings.activation_seconds;

    if (!options.no_pam) {
        control = std::make_unique<ControlService>(config.auth.credential_store, &seat_gate, &session_view);
        if (auto started = control->start(); !started) {
            log::warn(log_component, "no self-enrolment over D-Bus: {}", started.error().message());
            control.reset();
        }
    }

    auto tcp = listen_tcp(config.server.bind, config.server.port);
    if (!tcp) {
        return false;
    }
    listener = std::move(*tcp);
    std::filesystem::create_directories(options.runtime_dir, ec);
    ::chmod(options.runtime_dir.c_str(), 0755);
    socket_path = options.runtime_dir / "agent.sock";
    auto local = listen_unix(socket_path);
    if (!local) {
        return false;
    }
    agent_listener = std::move(*local);
    session_table = options.state_dir / "sessions";
    recover_sessions();
    if (config.metrics.listen) {
        metrics_server = std::make_unique<MetricsServer>(metrics);
        if (auto started = metrics_server->start(*config.metrics.listen); !started) {
            log::warn(log_component, "no metrics on {}: {}", *config.metrics.listen, started.error().message());
            metrics_server.reset();
        }
    }
    log::info(log_component, "listening on {}:{} (certificate SHA-256 {}), agents on {}, desktop {}{}",
              config.server.bind, config.server.port, identity->sha256_fingerprint(), socket_path.string(),
              to_string(config.session.desktop), options.no_pam ? ", development mode without PAM" : "");
    return true;
}

int Daemon::Impl::run(const std::atomic<bool>& stop)
{
    if (!setup()) {
        return 1;
    }
    while (!stop.load()) {
        std::vector<pollfd> fds{pollfd{listener.get(), POLLIN, 0}, pollfd{agent_listener.get(), POLLIN, 0},
                                pollfd{mailbox->fd(), POLLIN, 0}};
        for (const auto& peer : unclaimed) {
            fds.push_back(pollfd{peer.fd.get(), POLLIN, 0});
        }
        for (const auto& session : live) {
            fds.push_back(pollfd{session->agent.get(), static_cast<short>(session->agent.valid() ? POLLIN : 0), 0});
        }
        ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 250);
        const auto readable = [](const pollfd& p) { return (p.revents & (POLLIN | POLLHUP | POLLERR)) != 0; };

        // Agents first: their Disconnects precede whatever the mailbox brings.
        const std::size_t unclaimed_count = unclaimed.size();
        std::vector<std::uint32_t> ready;
        for (std::size_t i = 0; i < live.size(); ++i) {
            if (readable(fds[3 + unclaimed_count + i])) {
                ready.push_back(live[i]->id);
            }
        }
        for (const std::uint32_t id : ready) {
            if (Live* session = find(id)) {
                read_agent(*session);
            }
        }
        std::vector<bool> drop(unclaimed_count, false);
        for (std::size_t i = 0; i < unclaimed_count; ++i) {
            bool dropped = Clock::now() - unclaimed[i].since > hello_timeout;
            if (!dropped && readable(fds[3 + i])) {
                read_unclaimed(unclaimed[i], dropped);
            }
            drop[i] = dropped;
        }
        for (std::size_t i = unclaimed_count; i-- > 0;) {
            if (drop[i]) {
                unclaimed.erase(unclaimed.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
        if (readable(fds[2])) {
            for (auto& item : mailbox->take()) {
                std::visit([this](auto& posted) { handle(std::move(posted)); }, item);
            }
        }
        if (readable(fds[0])) {
            accept_client();
        }
        if (readable(fds[1])) {
            accept_agent();
        }
        tick();
    }
    shutdown();
    return 0;
}

void Daemon::Impl::accept_client()
{
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API takes a generic sockaddr
    UniqueFd fd(::accept(listener.get(), reinterpret_cast<sockaddr*>(&address), &length));
    if (!fd.valid()) {
        return;
    }
    app::prepare_socket(fd.get());
    const std::string peer = peer_name(address);
    if (mailbox->client_threads.load() >= max_preauth_clients) {
        log::warn(log_component, "{}: refused, {} clients are already logging in", peer, max_preauth_clients);
        return;
    }
    const std::uint64_t id = next_connection++;
    log::info(log_component, "{}: connected (connection {})", peer, id);
    ++mailbox->client_threads;
    // The thread owns copies of what it needs, so it may outlive the daemon.
    std::thread([box = mailbox, launch = network_launch, options = preauth, kerberos = kerberos,
                 store = options.config.auth.credential_store, fd = fd.release(), peer, id] {
        const auto accepted = Clock::now();
        auto loaded = auth::CredentialStore::load(store);
        if (!loaded) {
            log::error(log_component, "{}: refused, the credential store cannot be read", peer);
            ::close(fd);
            --box->client_threads;
            return;
        }
        StoreVerifier verifier(std::move(*loaded));
        auto client =
            app::authenticate_monitored(fd, peer, launch, verifier, options, box->stopping, accepted, kerberos.get());
        if (client && client->negotiation.identity) {
            const pid_t network = client->network_process;
            box->post(Authenticated{id, peer, account_of(verifier, *client->negotiation.identity),
                                    std::move(client->negotiation), std::move(client->plain), accepted});
            app::wait_network_process(network);
        } else if (client) {
            client->plain.reset();
            app::wait_network_process(client->network_process);
        }
        --box->client_threads;
    }).detach();
}

void Daemon::Impl::accept_agent()
{
    UniqueFd fd(::accept(agent_listener.get(), nullptr, nullptr));
    if (!fd.valid()) {
        return;
    }
    app::prepare_socket(fd.get());
    const auto credentials = app::peer_credentials(fd.get());
    if (!credentials) {
        return;
    }
    set_nonblocking(fd.get());
    unclaimed.push_back(AgentPeer{std::move(fd), credentials->uid, credentials->pid, {}, Clock::now()});
}

void Daemon::Impl::read_unclaimed(AgentPeer& peer, bool& drop)
{
    if (!read_available(peer.fd.get(), peer.inbox)) {
        drop = true;
        return;
    }
    auto frame = next_frame(peer.inbox);
    if (!frame) {
        drop = true;
        return;
    }
    if (!*frame) {
        return;  // not all there yet
    }
    const auto message = broker::decode_from(broker::Sender::agent, **frame, false);
    const auto* hello = message ? std::get_if<broker::Hello>(&*message) : nullptr;
    drop = true;
    if (hello == nullptr) {
        log::warn(log_component, "a process of uid {} spoke on the agent socket without a hello", peer.uid);
        return;
    }
    for (auto& s : live) {
        const uid_t expected = options.no_pam || !s->user ? ::getuid() : s->user->uid;
        if (s->link || s->terminate_sent || peer.uid != expected || !tokens_equal(hello->token, s->token)) {
            continue;
        }
        s->reattach_token = s->token;
        s->link = std::make_unique<broker::AgentLink>(s->token);
        if (auto accepted = s->link->receive(**frame, false); !accepted) {
            log::warn(log_component, "session {}: agent hello refused: {}", s->id, accepted.error().message());
            s->link.reset();
            return;
        }
        secure_zero(s->token);
        s->agent = std::move(peer.fd);
        s->inbox = std::move(peer.inbox);
        const bool returning = s->reattach_deadline.has_value();
        s->reattach_deadline.reset();
        registry.set_running(s->id);
        log::info(log_component, "session {}: agent {} of {} is {}", s->id,
                  peer.pid ? std::to_string(*peer.pid) : std::string("?"), s->account,
                  returning ? "back after the restart" : "ready");
        remember_sessions();
        // Before any connection: the agent applies it before it starts the
        // desktop, and it does not read /etc itself.
        if (!send_to(*s, settings)) {
            return;  // send_to ended the session
        }
        if (s->waiting) {
            auto client = std::move(*s->waiting);
            s->waiting.reset();
            const auto* state = registry.find(s->id);
            if (state != nullptr && state->attached) {
                // This connection did not start the session: somebody is
                // using it at the machine, and the agent asks them before
                // the session is taken from their screen ([policy] takeover).
                deliver(*s, std::move(client));
            } else {
                // Nobody is asked about the session this connection started.
                hand_over(*s, std::move(client));
            }
        }
        return;
    }
    // Usually an agent of a session that is gone: farlandd was away long
    // enough for it to be given up, or it was ended while farlandd was down.
    // Say so, or the agent keeps its desktop and tries again every half
    // second until its own timeout runs out.
    log::info(log_component, "agent hello from uid {} matches no session of ours; telling it to stop", peer.uid);
    static_cast<void>(app::send_message(peer.fd.get(), broker::encode(broker::Terminate{broker::EndReason::terminated}),
                                        -1, agent_send_timeout_ms));
}

void Daemon::Impl::read_agent(Live& session)
{
    const bool open = read_available(session.agent.get(), session.inbox);
    while (true) {
        auto frame = next_frame(session.inbox);
        if (!frame) {
            end_session(session, "its agent sent a bad frame", "bad_frame");
            return;
        }
        if (!*frame) {
            break;
        }
        auto message = session.link->receive(**frame, false);
        if (!message) {
            log::warn(log_component, "session {}: agent misbehaved: {}", session.id, message.error().message());
            end_session(session, "its agent misbehaved", "agent_misbehaved");
            return;
        }
        if (const auto* d = std::get_if<broker::Disconnect>(&*message)) {
            log::info(log_component, "session {}: connection {} ended{}", session.id, d->connection_id,
                      d->error_info != 0 ? std::format(" (error info {:#x})", d->error_info) : std::string());
            registry.disconnected(session.id, d->connection_id, Clock::now());
            const auto* state = registry.find(session.id);
            if (state != nullptr && state->connection == 0) {
                session.peer.clear();
            }
            if (state != nullptr && state->connection == 0 && !state->attached) {
                // Whoever was asked has left; the question falls away.
                for (auto& resolved : consent.released(session.id)) {
                    resolutions.push_back(std::move(resolved));
                }
            }
        } else if (const auto* reply = std::get_if<broker::ConsentReply>(&*message)) {
            if (auto resolved = consent.answered(reply->connection_id, reply->answer)) {
                resolutions.push_back(std::move(*resolved));
            }
        } else if (const auto* ended = std::get_if<broker::SessionEnded>(&*message)) {
            log::info(log_component, "session {} of {} ended: {}", session.id, session.account, ended->detail);
            registry.set_ending(session.id);
        } else if (const auto* stats = std::get_if<broker::Stats>(&*message)) {
            if (stats->connection_id != 0) {
                // After a restart the agent still has its client, which this
                // farlandd never handed over; take its word for it, and keep
                // later connections from being given the same number.
                registry.adopt_connection(session.id, stats->connection_id);
                next_connection = std::max(next_connection, stats->connection_id + 1);
            }
            registry.update_idle(session.id, stats->connection_id, stats->idle_seconds);
            session.stats = *stats;
        }
    }
    if (!open) {
        end_session(session, "its agent is gone", "agent_gone");
    }
}

void Daemon::Impl::handle(Authenticated client)
{
    const auto& identity = *client.negotiation.identity;
    if (identity.user.size() > max_identity_name || identity.domain.size() > max_identity_name ||
        client.account.empty()) {
        refuse(std::move(client), proto::errinfo::server_denied_connection, "the user name is too long");
        return;
    }
    const std::string who = user_name(identity);
    log::info(log_component, "{}: {} authenticated, local account {}", client.peer, who, client.account);
    const std::string account = client.account;
    std::optional<Account> user;
    bool local_session = false;
    if (!options.no_pam) {
        user = lookup_account(account);
        if (!user) {
            refuse(std::move(client), proto::errinfo::server_denied_connection,
                   std::format("{} has no local account {}", who, account));
            return;
        }
        if (user->uid == 0) {
            refuse(std::move(client), proto::errinfo::server_denied_connection, "root may not log in remotely");
            return;
        }
        local_session = !registry.find_account(account) && local_graphical_session(user->uid).has_value();
    }
    const auto decision = registry.admit(account, local_session);
    switch (decision.admission) {
    case SessionRegistry::Admission::existing:
        if (Live* session = find(decision.session)) {
            deliver(*session, std::move(client));
        }
        return;
    case SessionRegistry::Admission::create:
    case SessionRegistry::Admission::attach_local:
        start_session(std::move(client), account, user, decision.admission == SessionRegistry::Admission::attach_local);
        return;
    case SessionRegistry::Admission::replace_local:
        log::info(log_component, "{}: {}", client.peer, decision.reason);
        start_session(std::move(client), account, user, false, true);
        return;
    case SessionRegistry::Admission::separate_local:
        log::info(log_component, "{}: {}", client.peer, decision.reason);
        start_session(std::move(client), account, user, false, false, true);
        return;
    case SessionRegistry::Admission::refuse_local_session:
    case SessionRegistry::Admission::refuse_limit:
        refuse(std::move(client), decision.error_info, decision.reason);
        return;
    }
}

namespace {

/// Ends every graphical session `uid` has on a local seat and waits until
/// they are gone, so that GDM can start a headless one instead
/// (on_local_session = "replace"). The applications running in them are
/// lost, which is what that policy is for. Runs on the launcher's thread.
void end_local_sessions(const std::string& account, uid_t uid)
{
    const auto session = local_graphical_session(uid);
    if (!session) {
        return;
    }
    log::warn(log_component,
              "ending the local session {} of {} and everything running in it (on_local_session = "
              "\"replace\")",
              session->id, account);
    if (auto ended = terminate_login_session(session->id); !ended) {
        log::warn(log_component, "cannot end the local session {}: {}", session->id, ended.error().message());
        return;
    }
    // logind takes a moment to tear it down; GDM puts the greeter back on
    // the seat by itself once it is gone.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (local_graphical_session(uid) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (local_graphical_session(uid)) {
        log::warn(log_component, "the local session of {} is still there; starting a headless one anyway", account);
        return;
    }
    log::info(log_component, "the local session of {} ended; its seat shows the login screen again", account);
    // GNOME restarts the user's D-Bus when a session of theirs ends
    // (gnome-session-restart-dbus) and its services take a moment to go;
    // a session GDM starts in the middle of that dies again.
    std::this_thread::sleep_for(std::chrono::seconds(5));
}

}  // namespace

void Daemon::Impl::start_session(Authenticated client, const std::string& account, const std::optional<Account>& user,
                                 bool attach, bool replace_local, bool own_shell)
{
    const auto& config = options.config;
    auto session = std::make_unique<Live>();
    session->id = registry.create(account, attach, Clock::now());
    metrics.session_started();
    session->account = account;
    session->user = user;
    session->created = Clock::now();
    // OpenSSL takes bytes as unsigned char.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto* token_bytes = reinterpret_cast<unsigned char*>(session->token.data());
    if (RAND_bytes(token_bytes, static_cast<int>(session->token.size())) != 1) {
        registry.remove(session->id);
        refuse(std::move(client), proto::errinfo::out_of_memory, "no randomness for a session token");
        return;
    }
    const std::string rhost = remote_host(client.peer);
    session->waiting = std::move(client);

    AgentLaunch launch{options.agent, socket_path.string(),   session->id,       config.session.desktop,
                       attach,        config.session.command, options.log_level, true};
    log::info(log_component, "session {}: starting a {} desktop for {}{}", session->id,
              to_string(config.session.desktop), account,
              attach          ? " in the local session"
              : replace_local ? ", ending the local session it replaces"
              : own_shell     ? ", beside the local session it leaves alone"
                              : "");
    Result<pid_t> started = fail(Errc::unsupported, "no launcher");
    if (options.no_pam) {
        started = spawn_agent(launch, session->token, {});
    } else if (attach || (config.session.desktop == DesktopKind::gnome && !own_shell)) {
        // Someone else starts the login session (GDM, or the user at the
        // seat); the agent runs in the user's service manager.
        launch.token_on_fd = false;
        // Either way the agent drives a compositor someone else started.
        launch.attach = true;
        session->gdm = !attach;
        session->unit = std::format("farland-agent-{}.service", session->id);
        std::optional<std::string> local;
        if (attach && user) {
            if (auto s = local_graphical_session(user->uid)) {
                local = s->id;
            }
        }
        const uid_t uid = user ? user->uid : ::getuid();
        std::thread([box = mailbox, id = session->id, account, uid, gdm = session->gdm, unit = session->unit,
                     argv = agent_arguments(launch), token = app::token_to_hex(session->token), local,
                     replace_local]() mutable {
            std::string login = local.value_or("");
            if (replace_local) {
                end_local_sessions(account, uid);
            }
            if (gdm) {
                auto created = create_gdm_user_display(account, uid, std::chrono::seconds(60));
                if (!created) {
                    box->post(Launched{id, false, {}, account, gdm});
                    secure_zero(std::as_writable_bytes(std::span(token)));
                    return;
                }
                login = *created;
            }
            std::vector<std::string> env{std::string(app::agent_token_variable) + "=" + token};
            if (!login.empty()) {
                env.push_back("XDG_SESSION_ID=" + login);
            }
            const bool ok = start_user_unit(account, unit, argv, env).has_value();
            secure_zero(std::as_writable_bytes(std::span(token)));
            secure_zero(std::as_writable_bytes(std::span(env.front())));
            box->post(Launched{id, ok, login, account, gdm});
        }).detach();
        started = -1;
    } else {
        started = spawn_session_helper(options.self, launch, account, rhost, session->token);
    }
    if (!started) {
        Live& s = *session;
        live.push_back(std::move(session));
        end_session(s, "its agent could not be started", "start_failed");
        return;
    }
    session->process = *started;
    live.push_back(std::move(session));
}

void Daemon::Impl::handle(Launched launched)
{
    Live* session = find(launched.session);
    if (session == nullptr) {
        // It ended while GDM was still starting the session: take that down too.
        if (launched.gdm && !launched.login_session.empty() && !registry.find_account(launched.account)) {
            std::thread([account = launched.account] {
                static_cast<void>(destroy_gdm_user_display(account));
            }).detach();
        }
        return;
    }
    session->login_session = launched.login_session;
    if (!launched.ok) {
        end_session(*session, "its agent could not be started in the user's session", "start_failed");
        return;
    }
    log::info(log_component, "session {}: agent unit {} started in login session {}", session->id, session->unit,
              session->login_session.empty() ? "?" : session->login_session);
}

void Daemon::Impl::deliver(Live& session, Authenticated client)
{
    if (!session.link) {
        if (session.waiting) {
            log::info(log_component, "{}: connection {} replaced by {} before the session started",
                      session.waiting->peer, session.waiting->id, client.id);
        }
        session.waiting = std::move(client);
        return;
    }
    // Whoever holds the session now: the client connected to it, or the
    // user at the machine whose session this one shows.
    const auto* state = registry.find(session.id);
    const bool held = state != nullptr && (state->connection != 0 || state->attached);
    switch (consent.admit(held)) {
    case ConsentBroker::Admission::refuse:
        refuse(std::move(client), proto::errinfo::server_denied_connection, consent.refusal(session.account));
        return;
    case ConsentBroker::Admission::ask:
        ask_consent(session, std::move(client));
        return;
    case ConsentBroker::Admission::hand_over:
        break;
    }
    hand_over(session, std::move(client));
}

void Daemon::Impl::ask_consent(Live& session, Authenticated client)
{
    if (session.asking) {
        auto older = std::move(*session.asking);
        session.asking.reset();
        consent.forget(older.id);
        log::info(log_component, "{}: connection {} replaced by {} while the session's user was being asked",
                  older.peer, older.id, client.id);
        refuse(std::move(older), proto::errinfo::server_denied_connection,
               "another connection asked for the session first");
    }
    const auto& identity = client.negotiation.identity;
    ConsentBroker::Party who;
    who.user = identity ? user_name(*identity) : client.account;
    who.peer = client.peer;
    who.client_name = peek_client_name(client.plain.get());
    const auto request = consent.ask(session.id, client.id, who, Clock::now());
    log::info(log_component,
              "session {} of {}: asking whether connection {} from {} may take it over ({} s, "
              "takeover_on_timeout = \"{}\")",
              session.id, session.account, client.id, client.peer, request.timeout_seconds,
              to_string(options.config.policy.takeover_on_timeout));
    session.asking = std::move(client);
    // send_to ends the session when the agent does not take the question,
    // and that refuses the client it holds.
    static_cast<void>(send_to(session, request));
}

void Daemon::Impl::ask_the_seat()
{
    // Only a session a client holds is worth asking about; the gate lets
    // every other login straight through.
    std::vector<std::string> held;
    for (const auto& session : registry.sessions()) {
        if (session.connection != 0) {
            held.push_back(session.account);
        }
    }
    seat_gate.set_held_accounts(std::move(held));

    for (const auto& waiting : seat_gate.take_new()) {
        const auto* state = registry.find_account(waiting.account);
        Live* session = state != nullptr ? find(state->id) : nullptr;
        if (state == nullptr || session == nullptr || state->connection == 0 || !session->link) {
            seat_gate.resolve(waiting.cookie, true);  // nobody holds it any more
            continue;
        }
        switch (options.config.policy.seat_takeover) {
        case TakeoverPolicy::always:
            static_cast<void>(send_to(*session, broker::SeatTakeover{true}));
            seat_gate.resolve(waiting.cookie, true);
            continue;
        case TakeoverPolicy::never:
            log::info(log_component, "session {} of {}: the login at the machine is refused: seat_takeover = \"never\"",
                      session->id, session->account);
            // Before the login is let go: the seat falls back to this
            // session the moment the display manager gives its login screen
            // up, and the agent has to know that is not a takeover.
            if (!send_to(*session, broker::SeatTakeover{false})) {
                seat_gate.resolve(waiting.cookie, true);  // the session ended; nobody holds it now
                continue;
            }
            seat_gate.resolve(waiting.cookie, false);
            continue;
        case TakeoverPolicy::ask:
            break;
        }
        const auto connection = next_seat_connection++;
        ConsentBroker::Party who;
        who.user = waiting.account;
        auto question = consent.ask(session->id, connection, who, Clock::now());
        question.from_seat = true;
        log::info(log_component,
                  "session {} of {}: asking whether somebody logging in at the machine may take it back ({} s, "
                  "takeover_on_timeout = \"{}\")",
                  session->id, session->account, question.timeout_seconds,
                  to_string(options.config.policy.takeover_on_timeout));
        seat_questions.emplace(connection, waiting.cookie);
        if (!send_to(*session, question)) {
            // send_to ended the session; whoever asked gets their login.
            seat_questions.erase(connection);
            seat_gate.resolve(waiting.cookie, true);
        }
    }
}

void Daemon::Impl::resolve(const ConsentBroker::Resolved& resolved)
{
    if (const auto found = seat_questions.find(resolved.connection); found != seat_questions.end()) {
        // A login at the machine, not a client: the answer decides whether
        // whoever is there gets in, and the session goes back to the seat
        // by itself once they do.
        log::info(log_component, "session {}: the login at the machine may {}take the session back: {}",
                  resolved.session, resolved.allowed ? "" : "not ", resolved.reason);
        Live* session = find(resolved.session);
        // A send_to that fails ends the session, so the second one only goes
        // out while there is still a session to send it to.
        const bool cancelled =
            session == nullptr || !resolved.withdraw || send_to(*session, broker::ConsentCancel{resolved.connection});
        if (session != nullptr && cancelled) {
            // Before the login is let go, so that the agent knows what the
            // seat coming back means: a refused login only gets it back
            // because the display manager gives its login screen up.
            static_cast<void>(send_to(*session, broker::SeatTakeover{resolved.allowed}));
        }
        // Nobody holds a session that just ended, so that login goes through.
        seat_gate.resolve(found->second, resolved.allowed || find(resolved.session) == nullptr);
        seat_questions.erase(found);
        return;
    }
    Live* session = find(resolved.session);
    if (session == nullptr || !session->asking || session->asking->id != resolved.connection) {
        return;
    }
    auto client = std::move(*session->asking);
    session->asking.reset();
    if (resolved.withdraw && !send_to(*session, broker::ConsentCancel{resolved.connection})) {
        refuse(std::move(client), proto::errinfo::server_denied_connection, "its session ended");
        return;  // send_to ended the session
    }
    if (!resolved.allowed) {
        refuse(std::move(client), proto::errinfo::server_denied_connection, resolved.reason);
        return;
    }
    log::info(log_component, "session {} of {}: connection {} may take it over: {}", session->id, session->account,
              client.id, resolved.reason);
    hand_over(*session, std::move(client));
}

void Daemon::Impl::settle()
{
    while (!resolutions.empty()) {
        const ConsentBroker::Resolved resolved = resolutions.front();
        resolutions.erase(resolutions.begin());
        resolve(resolved);
    }
}

void Daemon::Impl::hand_over(Live& session, Authenticated client)
{
    if (const std::uint64_t replaced = registry.connect(session.id, client.id); replaced != 0) {
        log::info(log_component, "session {}: connection {} takes over from {}", session.id, client.id, replaced);
        if (!send_to(session, broker::Disconnect{replaced, proto::errinfo::disconnected_by_other_connection})) {
            return;
        }
    }
    broker::NewConnection message;
    message.connection_id = client.id;
    message.negotiation = client.negotiation;
    message.peer = client.peer.substr(0, 255);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - client.accepted).count();
    message.elapsed_ms = static_cast<std::uint32_t>(std::clamp<std::int64_t>(elapsed, 0, 3'600'000));
    if (send_to(session, message, client.plain.get())) {
        metrics.connection();
        session.peer = client.peer;
        log::info(log_component, "{}: connection {} goes to session {} of {}", client.peer, client.id, session.id,
                  session.account);
    }
}

void Daemon::Impl::refuse(Authenticated client, std::uint32_t error_info, const std::string& reason)
{
    log::warn(log_component, "{}: refused: {}", client.peer, reason);
    metrics.connection_refused(errinfo_name(error_info));
    // A sandboxed process takes the client as far as Set Error Info can go.
    const std::vector<std::string> args{options.self.string(), "--refuse-child",
                                        "--error-info",        std::to_string(error_info),
                                        "--selected-protocol", std::to_string(client.negotiation.selected_protocol),
                                        "--log-level",         options.log_level};
    std::vector<char*> argv;
    for (const auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));  // NOLINT(cppcoreguidelines-pro-type-const-cast): exec only reads
    }
    argv.push_back(nullptr);
    std::array<char*, 1> envp{nullptr};
    posix_spawn_file_actions_t actions;
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawn_file_actions_adddup2(&actions, client.plain.get(), 3);
    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, argv.front(), &actions, nullptr, argv.data(), envp.data());
    ::posix_spawn_file_actions_destroy(&actions);
    if (rc == 0) {
        std::thread([pid] {
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
        }).detach();
    }
}

void Daemon::Impl::end_session(Live& session, const std::string& why, std::string_view tag, bool wait)
{
    log::info(log_component, "session {} of {}: cleaning up, {}", session.id, session.account, why);
    if (session.waiting) {
        log::warn(log_component, "{}: connection {} dropped, its session did not start", session.waiting->peer,
                  session.waiting->id);
    }
    const auto abandoned = consent.ended(session.id);
    if (session.asking) {
        auto client = std::move(*session.asking);
        session.asking.reset();
        const auto asked = std::ranges::find(abandoned, client.id, &ConsentBroker::Resolved::connection);
        const std::string reason = asked != abandoned.end() ? asked->reason : std::string("its session ended");
        refuse(std::move(client), proto::errinfo::server_denied_connection, reason);
    }
    if (session.process > 0) {
        ::kill(session.process, SIGTERM);
        orphans.push_back(session.process);
    }
    std::function<void()> teardown;
    if (session.gdm) {
        teardown = [account = session.account, id = session.login_session] {
            if (!destroy_gdm_user_display(account) && !id.empty()) {
                static_cast<void>(terminate_login_session(id));
            }
        };
    } else if (!session.unit.empty() && !session.link && session.user) {
        teardown = [user = session.user->name, unit = session.unit] { static_cast<void>(stop_user_unit(user, unit)); };
    }
    if (teardown && (wait || shutting_down)) {
        teardown();
    } else if (teardown) {
        std::thread(std::move(teardown)).detach();
    }
    metrics.session_ended(session.end_tag.empty() ? std::string(tag) : session.end_tag);
    registry.remove(session.id);
    const std::uint32_t id = session.id;
    std::erase_if(live, [id](const auto& s) { return s->id == id; });
    if (!shutting_down) {
        remember_sessions();  // it is not coming back
    }
}

bool Daemon::Impl::send_to(Live& session, const broker::Message& message, int fd)
{
    if (auto sent = app::send_message(session.agent.get(), broker::encode(message), fd, agent_send_timeout_ms); !sent) {
        log::warn(log_component, "session {}: cannot reach its agent: {}", session.id, sent.error().message());
        end_session(session, "its agent does not answer", "agent_unreachable");
        return false;
    }
    return true;
}

void Daemon::Impl::remember_sessions()
{
    std::vector<StoredSession> stored;
    stored.reserve(live.size());
    for (const auto& s : live) {
        // A session that is ending, or whose agent never greeted, has
        // nothing to come back to.
        if (s->terminate_sent || !s->link) {
            continue;
        }
        StoredSession entry;
        entry.id = s->id;
        entry.account = s->account;
        entry.uid = static_cast<std::uint32_t>(s->user ? s->user->uid : ::getuid());
        entry.attached = registry.find(s->id) != nullptr && registry.find(s->id)->attached;
        entry.gdm = s->gdm;
        entry.login_session = s->login_session;
        entry.unit = s->unit;
        entry.token = s->reattach_token;
        stored.push_back(std::move(entry));
    }
    if (auto saved = save_sessions(session_table, stored); !saved) {
        log::warn(log_component, "cannot write {}: {}; sessions will not survive a restart", session_table.string(),
                  saved.error().message());
    }
}

void Daemon::Impl::recover_sessions()
{
    const auto stored = load_sessions(session_table);
    if (stored.empty()) {
        return;
    }
    const auto now = Clock::now();
    std::size_t taken = 0;
    for (const auto& entry : stored) {
        auto session = std::make_unique<Live>();
        session->id = entry.id;
        session->account = entry.account;
        session->token = entry.token;
        session->reattach_token = entry.token;
        session->gdm = entry.gdm;
        session->login_session = entry.login_session;
        session->unit = entry.unit;
        session->created = now;
        session->reattach_deadline = now + reattach_window;
        // The uid decides which process may claim it; the account's own
        // entry may have changed since, so the stored uid is what counts.
        session->user = lookup_account(entry.account);
        if (session->user && session->user->uid != entry.uid) {
            log::warn(log_component, "session {} of {} changed uid while farlandd was away; not picking it up",
                      entry.id, entry.account);
            continue;
        }
        if (!session->user) {
            Account account;
            account.name = entry.account;
            account.uid = entry.uid;
            session->user = account;
        }
        registry.restore(entry.id, entry.account, entry.attached, now);
        live.push_back(std::move(session));
        ++taken;
    }
    log::info(log_component, "{} session{} from before the restart; waiting {} s for their agents", taken,
              taken == 1 ? "" : "s", std::chrono::duration_cast<std::chrono::seconds>(reattach_window).count());
}

/// What the control API reports, from the registry and the live sessions.
std::vector<SessionView::Entry> Daemon::Impl::describe_sessions(Clock::time_point now) const
{
    const auto seconds = [](Clock::duration d) {
        return static_cast<std::uint64_t>(
            std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(d).count()));
    };
    std::vector<SessionView::Entry> out;
    out.reserve(registry.sessions().size());
    for (const auto& s : registry.sessions()) {
        SessionView::Entry entry;
        entry.id = s.id;
        entry.account = s.account;
        switch (s.state) {
        case SessionRegistry::State::starting:
            entry.state = "starting";
            break;
        case SessionRegistry::State::running:
            entry.state = "running";
            break;
        case SessionRegistry::State::ending:
            entry.state = "ending";
            break;
        }
        entry.attached = s.attached;
        entry.connected = s.connection != 0;
        entry.idle_seconds = s.idle_seconds;
        entry.disconnected_seconds = s.disconnected_since ? seconds(now - *s.disconnected_since) : 0;
        entry.desktop = to_string(options.config.session.desktop);
        for (const auto& l : live) {
            if (l->id != s.id) {
                continue;
            }
            entry.age_seconds = seconds(now - l->created);
            entry.peer = l->peer;
            break;
        }
        out.push_back(std::move(entry));
    }
    return out;
}

void Daemon::Impl::tick()
{
    const auto now = Clock::now();
    ask_the_seat();
    // Sessions somebody asked to end through the control API.
    for (const std::uint32_t id : session_view.take_terminations()) {
        Live* session = find(id);
        if (session == nullptr) {
            continue;  // it went away between the snapshot and here
        }
        log::info(log_component, "session {} of {}: ending it, asked through the control API", session->id,
                  session->account);
        session->terminate_sent = now;
        session->end_tag = "requested";
        static_cast<void>(send_to(*session, broker::Terminate{broker::EndReason::terminated}));
        registry.set_ending(session->id);
    }
    for (auto& resolved : consent.due(now)) {
        resolutions.push_back(std::move(resolved));
    }
    settle();
    for (const auto& action : registry.due(now)) {
        Live* session = find(action.session);
        if (session == nullptr) {
            continue;
        }
        if (action.kind == SessionRegistry::Action::Kind::disconnect_idle) {
            log::info(log_component, "session {}: connection {} idle for {} s, disconnecting it", session->id,
                      action.connection, options.config.policy.idle_timeout.count());
            static_cast<void>(send_to(*session, broker::Disconnect{action.connection, proto::errinfo::idle_timeout}));
        } else {
            log::info(log_component, "session {} of {}: nobody reconnected within {} s, ending it", session->id,
                      session->account, options.config.policy.disconnected_timeout.count());
            session->terminate_sent = now;
            session->end_tag = "disconnected_timeout";
            static_cast<void>(send_to(*session, broker::Terminate{broker::EndReason::disconnected_timeout}));
        }
    }
    // A session from before the restart whose agent never came back: its
    // process is gone, or too old to speak to us.
    for (auto& s : live) {
        if (s->reattach_deadline && now >= *s->reattach_deadline) {
            s->reattach_deadline.reset();
            log::info(log_component, "session {} of {}: its agent did not come back after the restart", s->id,
                      s->account);
            end_session(*s, "the agent did not come back after the restart", "not_reattached");
        }
    }
    session_view.publish(describe_sessions(now));
    if (metrics_server) {
        std::vector<SessionMetrics> figures;
        figures.reserve(live.size());
        for (const auto& s : live) {
            const auto* state = registry.find(s->id);
            if (state == nullptr) {
                continue;
            }
            SessionMetrics m;
            m.id = s->id;
            m.account = s->account;
            m.state = state->state == SessionRegistry::State::starting  ? "starting"
                      : state->state == SessionRegistry::State::running ? "running"
                                                                        : "ending";
            m.connected = state->connection != 0;
            m.uptime_seconds = static_cast<std::uint64_t>(
                std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(now - s->created).count()));
            m.idle_seconds = state->idle_seconds;
            m.frames_sent = s->stats.frames_sent;
            m.bytes_sent = s->stats.bytes_sent;
            m.bytes_received = s->stats.bytes_received;
            m.rtt_ms = s->stats.rtt_ms;
            m.bandwidth_kbps = s->stats.bandwidth_kbps;
            figures.push_back(std::move(m));
        }
        metrics.publish(std::move(figures));
    }
    std::vector<std::uint32_t> overdue;
    for (auto& s : live) {
        if (s->process > 0) {
            int status = 0;
            if (::waitpid(s->process, &status, WNOHANG) == s->process) {
                s->process = -1;
                if (!s->link) {
                    overdue.push_back(s->id);  // gone before it said hello
                }
            }
        }
        const bool start_overdue = !s->link && now - s->created > start_timeout;
        const bool terminate_overdue = s->terminate_sent && now - *s->terminate_sent > terminate_grace;
        if (start_overdue || terminate_overdue) {
            overdue.push_back(s->id);
        }
    }
    for (const std::uint32_t id : overdue) {
        if (Live* session = find(id)) {
            end_session(*session, session->link ? "its agent did not end in time" : "its agent did not start",
                        session->link ? "end_timeout" : "start_timeout");
        }
    }
    std::erase_if(orphans, [](pid_t pid) {
        int status = 0;
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        return done == pid || (done < 0 && errno == ECHILD);
    });
}

Live* Daemon::Impl::find(std::uint32_t id)
{
    const auto found = std::ranges::find_if(live, [id](const auto& s) { return s->id == id; });
    return found != live.end() ? found->get() : nullptr;
}

void Daemon::Impl::shutdown()
{
    seat_gate.release_all();
    shutting_down = true;
    mailbox->stopping = true;
    listener.reset();
    const auto now = Clock::now();
    // The sessions stay: the agent owns the desktop and the client's socket,
    // and reconnects to whichever farlandd is here next. Written down first,
    // because after this the daemon knows nothing.
    remember_sessions();
    std::size_t kept = 0;
    std::vector<std::uint32_t> unstarted;
    for (auto& s : live) {
        if (s->link && s->agent.valid()) {
            ++kept;
        } else {
            // Nothing to come back to: no agent ever greeted.
            unstarted.push_back(s->id);
        }
    }
    log::info(log_component, "shutting down; {} session{} left running for the next farlandd, {} ended", kept,
              kept == 1 ? "" : "s", unstarted.size());
    for (const std::uint32_t id : unstarted) {
        if (Live* s = find(id)) {
            s->terminate_sent = now;
            s->end_tag = "shutdown";
            end_session(*s, "farlandd stops", "shutdown", true);
        }
    }
    // Wait only for the ones that are ending. A session that is staying is
    // left untouched: closing its socket is all it needs to start looking
    // for the next farlandd.
    const auto ending = [this] {
        return std::ranges::any_of(live, [](const auto& s) { return s->terminate_sent.has_value(); });
    };
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (ending() && Clock::now() < deadline) {
        std::vector<pollfd> fds;
        std::vector<std::uint32_t> ids;
        for (const auto& s : live) {
            if (s->terminate_sent) {
                fds.push_back(pollfd{s->agent.get(), POLLIN, 0});
                ids.push_back(s->id);
            }
        }
        ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 250);
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                if (Live* s = find(ids[i])) {
                    read_agent(*s);
                }
            }
        }
    }
    for (std::size_t i = live.size(); i-- > 0;) {
        if (live[i]->terminate_sent) {
            end_session(*live[i], "farlandd stops", "shutdown", true);
        }
    }
    mailbox->close();
    agent_listener.reset();
    ::unlink(socket_path.c_str());
    for (int i = 0; i < 40 && (!orphans.empty() || mailbox->client_threads.load() > 0); ++i) {
        tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

int run_refusal(int fd, std::uint32_t selected_protocol, std::uint32_t error_info)
{
    app::prepare_socket(fd);
    if (!app::enter_network_sandbox()) {
        return 1;
    }
    server::Negotiation negotiation;
    negotiation.selected_protocol = selected_protocol;
    negotiation.requested_protocols = selected_protocol;
    server::ServerConfig config;
    config.autodetect = server::AutoDetectMode::off;
    config.heartbeat_period = std::chrono::seconds(0);
    server::Connection connection(config, negotiation);
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    std::array<std::byte, 16384> chunk{};
    bool done = false;
    while (!done && Clock::now() < deadline) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 250) > 0) {
            const ssize_t n = ::recv(fd, chunk.data(), chunk.size(), 0);
            if (n <= 0) {
                break;
            }
            connection.tick(Clock::now());
            connection.receive(std::span(chunk).first(static_cast<std::size_t>(n)));
        }
        const auto state = connection.state();
        if (state == server::State::wait_confirm_active || state == server::State::finalizing ||
            state == server::State::active) {
            connection.disconnect(error_info);
            done = true;
        }
        if (!app::send_all(fd, connection.take_output())) {
            break;
        }
        while (const auto event = connection.poll_event()) {
            done = done || std::holds_alternative<server::event::Closed>(*event);
        }
    }
    // Let the client read the ultimatum and hang up itself: it may still be
    // writing (its Confirm Active), and when this end closes first, the
    // network process closes the TCP connection with unread input, which
    // resets it before the client read what came before.
    const auto linger = Clock::now() + std::chrono::seconds(3);
    while (Clock::now() < linger) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 250) > 0 && ::recv(fd, chunk.data(), chunk.size(), 0) <= 0) {
            break;
        }
    }
    ::close(fd);
    return 0;
}

}  // namespace farland::daemon
