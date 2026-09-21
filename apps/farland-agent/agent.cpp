// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "agent.hpp"

#include <farland/base/log.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/text.hpp>
#include <farland/proto/framing.hpp>
#include <farland/proto/mcs.hpp>
#include <farland/proto/save_session_info.hpp>
#include <farland/proto/share.hpp>
#include <farland/proto/x224.hpp>

#include "transport.hpp"
#include "unix_socket.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace farland::agent {

namespace {

namespace broker = server::broker;
using Clock = std::chrono::steady_clock;
constexpr std::string_view log_component = "agent";
constexpr std::uint32_t min_size = 64;
constexpr std::uint32_t max_size = 8192;
/// How long farlandd may take to finish a message it started.
constexpr int daemon_message_timeout_ms = 5000;

std::uint32_t clamp_size(std::int64_t value)
{
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(value, min_size, max_size));
}

/// The Set Error Info code for ending the connection for `reason`.
std::uint32_t error_info_for(broker::EndReason reason)
{
    switch (reason) {
    case broker::EndReason::logout:
        return proto::errinfo::logoff_by_user;
    case broker::EndReason::idle_timeout:
        return proto::errinfo::idle_timeout;
    case broker::EndReason::terminated:
    case broker::EndReason::disconnected_timeout:
        return proto::errinfo::rpc_initiated_logoff;
    case broker::EndReason::desktop_failed:
    case broker::EndReason::error:
        break;
    }
    return proto::errinfo::rpc_initiated_disconnect;
}

std::string_view to_string(broker::EndReason reason)
{
    switch (reason) {
    case broker::EndReason::logout:
        return "logout";
    case broker::EndReason::desktop_failed:
        return "the desktop failed";
    case broker::EndReason::idle_timeout:
        return "idle timeout";
    case broker::EndReason::disconnected_timeout:
        return "disconnected timeout";
    case broker::EndReason::terminated:
        return "terminated";
    case broker::EndReason::error:
        break;
    }
    return "error";
}

std::string_view to_string(broker::ConsentAnswer answer)
{
    switch (answer) {
    case broker::ConsentAnswer::allowed:
        return "yes";
    case broker::ConsentAnswer::denied:
        return "no, the user cancelled it";
    case broker::ConsentAnswer::timed_out:
        return "nobody answered";
    case broker::ConsentAnswer::unavailable:
        return "nobody could be asked";
    }
    return "nobody could be asked";
}

std::optional<broker::ClientSummary> parse_connect_initial(std::span<const std::byte> pdu)
{
    Reader r(pdu);
    auto tpdu = proto::read_tpkt(r);
    if (!tpdu) {
        return std::nullopt;
    }
    const auto code = proto::peek_tpdu_code(*tpdu);
    if (!code || *code != proto::TpduCode::data) {
        return std::nullopt;
    }
    auto data = proto::decode_data_tpdu(*tpdu);
    if (!data) {
        return std::nullopt;
    }
    const auto initial = proto::mcs::decode_connect_initial(*data);
    if (!initial) {
        return std::nullopt;
    }
    Reader user_data(initial->user_data);
    const auto blocks = proto::gcc::decode_conference_create_request(user_data);
    if (!blocks) {
        return std::nullopt;
    }
    Reader block_reader(*blocks);
    const auto client = proto::gcc::decode_client_data(block_reader);
    if (!client) {
        return std::nullopt;
    }
    return summarize(*client);
}

}  // namespace

broker::ClientSummary summarize(const proto::gcc::ClientData& data)
{
    const auto& core = data.core;
    broker::ClientSummary summary;
    summary.desktop_width = core.desktop_width;
    summary.desktop_height = core.desktop_height;
    summary.keyboard_layout = core.keyboard_layout;
    summary.keyboard_type = core.keyboard_type;
    summary.keyboard_subtype = core.keyboard_subtype;
    summary.client_build = core.client_build;
    summary.client_name = core.client_name.substr(0, 64);
    summary.desktop_scale_factor = core.desktop_scale_factor;
    if (data.monitor) {
        for (const auto& m : data.monitor->monitors) {
            if (summary.monitors.size() == broker::max_monitors) {
                break;
            }
            if (m.left <= m.right && m.top <= m.bottom) {
                summary.monitors.push_back(broker::Monitor{m.left, m.top, m.right, m.bottom, (m.flags & 1U) != 0});
            }
        }
    }
    return summary;
}

std::pair<std::uint32_t, std::uint32_t> initial_size(const broker::ClientSummary& client)
{
    if (!client.monitors.empty()) {
        const auto primary = std::ranges::find_if(client.monitors, &broker::Monitor::primary);
        const auto& m = primary != client.monitors.end() ? *primary : client.monitors.front();
        return {clamp_size(std::int64_t{m.right} - m.left + 1), clamp_size(std::int64_t{m.bottom} - m.top + 1)};
    }
    if (client.desktop_width == 0 || client.desktop_height == 0) {
        return {1024, 768};
    }
    return {clamp_size(client.desktop_width), clamp_size(client.desktop_height)};
}

ConnectInitialPeek read_connect_initial(int fd, int timeout_ms)
{
    ConnectInitialPeek peek;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::array<std::byte, 4096> chunk{};
    while (peek.bytes.size() < broker::max_pending_input) {
        const auto frame = proto::peek_frame(peek.bytes);
        if (!frame) {
            return peek;  // not RDP; the session reports it
        }
        if (*frame && peek.bytes.size() >= (*frame)->length) {
            peek.client = parse_connect_initial(std::span(peek.bytes).first((*frame)->length));
            return peek;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) {
            return peek;
        }
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, static_cast<int>(left)) <= 0) {
            continue;
        }
        const std::size_t room = std::min(chunk.size(), broker::max_pending_input - peek.bytes.size());
        const ssize_t received = ::recv(fd, chunk.data(), room, 0);
        if (received < 0 && (errno == EINTR || app::would_block(errno))) {
            continue;
        }
        if (received <= 0) {
            return peek;
        }
        peek.bytes.insert(peek.bytes.end(), chunk.begin(), chunk.begin() + received);
    }
    return peek;
}

/// One client connection: its session thread and what it shares with it.
struct Agent::Connection {
    std::uint64_t id = 0;
    std::string peer;
    app::SessionControl control;
    std::atomic<bool> stop{false};
    /// Set by the session thread as it ends.
    std::atomic<bool> finished{false};
    std::thread thread;
};

/// The takeover question that is out, and the thread that asks it.
struct Agent::Prompt {
    std::uint64_t connection = 0;
    std::atomic<bool> cancel{false};
    /// Set by the asking thread once `answer` holds.
    std::atomic<bool> finished{false};
    server::broker::ConsentAnswer answer = server::broker::ConsentAnswer::unavailable;
    std::thread thread;
};

Agent::Agent(AgentConfig config) : config_(std::move(config)), arc_(config_.logon_id)
{
    if (!config_.ask_consent) {
        config_.ask_consent = [](const ConsentQuestion& question, const std::atomic<bool>& cancel) {
            return ask_over_notifications(question, cancel);
        };
    }
    std::array<int, 2> fds{-1, -1};
    if (::pipe(fds.data()) == 0) {
        wake_read_.reset(fds[0]);
        wake_write_.reset(fds[1]);
        for (const int fd : fds) {
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            ::fcntl(fd, F_SETFL, O_NONBLOCK);
        }
    }
}

Agent::~Agent()
{
    withdraw_prompt();
    stop_connection(proto::errinfo::rpc_initiated_disconnect);
    if (current_ && current_->thread.joinable()) {
        current_->thread.join();
    }
    secure_zero(config_.token);
}

bool Agent::send(const broker::Message& message)
{
    if (daemon_gone_) {
        return false;
    }
    if (auto sent = app::send_message(config_.daemon.get(), broker::encode(message)); !sent) {
        log::warn(log_component, "session {}: cannot reach farlandd: {}", config_.logon_id, sent.error().message());
        daemon_gone_ = true;
        return false;
    }
    return true;
}

void Agent::apply(const broker::Settings& settings)
{
    if (desktop_) {
        // The desktop was built from the settings that were in force; a
        // later change would only half apply.
        log::warn(log_component, "session {}: farlandd's settings arrived after the desktop started; keeping the old",
                  config_.logon_id);
        return;
    }
    auto& session = config_.session;
    session.frames_per_second = settings.frames_per_second;
    session.h264_bitrate_kbps = settings.h264_bitrate_kbps;
    session.h264_min_bitrate_kbps = settings.h264_min_bitrate_kbps;
    session.h264_max_bitrate_kbps = settings.h264_max_bitrate_kbps;
    session.codec = settings.bitmap_codec;
    session.gfx_codec = settings.gfx_codec;
    session.h264_backend = settings.h264_backend;
    session.openh264_library = settings.openh264_library;
    session.render_node = settings.render_node;
    session.zero_copy = settings.zero_copy;
    session.clearcodec = settings.clearcodec;
    session.refine = settings.refine;
    session.video_regions = settings.video_regions;
    session.lossless_still = settings.lossless_still;
    session.audio = settings.audio;
    session.microphone = settings.microphone;
    session.camera = settings.camera;
    session.clipboard = settings.clipboard;
    session.autodetect = settings.autodetect;
    session.activation_timeout = settings.activation_seconds;
    log::info(log_component, "session {}: settings from farlandd: {}", config_.logon_id, broker::describe(settings));
}

broker::EndReason Agent::run(const std::atomic<bool>& stop)
{
    // The token is kept, not wiped: it is what the next farlandd recognises
    // this session by after a restart. It goes in the destructor.
    if (!send(broker::Hello{broker::protocol_version, config_.token})) {
        return broker::EndReason::error;
    }
    log::info(log_component, "session {}: ready", config_.logon_id);
    auto next_stats = Clock::now() + config_.stats_period;
    while (true) {
        if (stop.load()) {
            return finish(broker::EndReason::terminated, "the agent was stopped");
        }
        std::vector<pollfd> fds{pollfd{config_.daemon.get(), POLLIN, 0}, pollfd{wake_read_.get(), POLLIN, 0}};
        const bool idle = !current_ && desktop_;
        if (idle) {
            for (const int fd : desktop_->dispatch_fds()) {
                if (fd >= 0) {
                    fds.push_back(pollfd{fd, POLLIN, 0});
                }
            }
        }
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_stats - Clock::now()).count();
        ::poll(fds.data(), static_cast<nfds_t>(fds.size()), static_cast<int>(std::clamp<std::int64_t>(wait, 0, 250)));

        if (daemon_gone_ && !reattach(stop)) {
            return finish(broker::EndReason::error, "farlandd went away");
        }
        if ((fds[1].revents & POLLIN) != 0) {
            std::array<char, 16> drain{};
            while (::read(wake_read_.get(), drain.data(), drain.size()) > 0) {
            }
            // A takeover may have joined the connection that woke us already.
            if (current_ && current_->finished.load()) {
                join_connection();
            }
            finish_prompt();
            if (desktop_ && desktop_->closed()) {
                return finish(broker::EndReason::logout, "the desktop ended");
            }
        }
        if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            auto received =
                app::receive_message(config_.daemon.get(), broker::max_message_size, daemon_message_timeout_ms);
            if (!received || !*received) {
                daemon_gone_ = true;
                if (reattach(stop)) {
                    continue;  // a new farlandd has the session
                }
                return finish(broker::EndReason::error, "farlandd closed the connection");
            }
            auto message = broker::decode_from(broker::Sender::daemon, (*received)->frame, (*received)->fd.valid());
            if (!message) {
                log::error(log_component, "session {}: bad message from farlandd: {}", config_.logon_id,
                           message.error().message());
                return finish(broker::EndReason::error, "bad message from farlandd");
            }
            if (const auto* settings = std::get_if<broker::Settings>(&*message)) {
                apply(*settings);
            } else if (auto* connection = std::get_if<broker::NewConnection>(&*message)) {
                if (!on_new_connection(std::move(*connection), std::move((*received)->fd))) {
                    return finish(broker::EndReason::desktop_failed, "the desktop did not start");
                }
            } else if (const auto* disconnect = std::get_if<broker::Disconnect>(&*message)) {
                if (current_ && current_->id == disconnect->connection_id) {
                    log::info(log_component, "session {}: farlandd ends connection {} (error info {:#x})",
                              config_.logon_id, disconnect->connection_id, disconnect->error_info);
                    stop_connection(disconnect->error_info);
                }
            } else if (const auto* request = std::get_if<broker::ConsentRequest>(&*message)) {
                start_prompt(*request);
            } else if (const auto* withdrawn = std::get_if<broker::ConsentCancel>(&*message)) {
                if (prompt_ && prompt_->connection == withdrawn->connection_id) {
                    log::info(log_component, "session {}: farlandd takes back the question about connection {}",
                              config_.logon_id, withdrawn->connection_id);
                    withdraw_prompt();
                }
            } else if (const auto* seat = std::get_if<broker::SeatTakeover>(&*message)) {
                // A refused login leaves the display manager giving up the
                // login screen on the seat, so the desktop has to know that
                // the seat coming back now is not somebody taking it.
                log::info(log_component, "session {}: the login at the machine may {}take the session back",
                          config_.logon_id, seat->allowed ? "" : "not ");
                if (desktop_) {
                    desktop_->seat_takeover_decided(seat->allowed);
                }
            } else if (const auto* terminate = std::get_if<broker::Terminate>(&*message)) {
                return finish(terminate->reason, "farlandd ended the session");
            }
        }
        if (idle && desktop_) {
            desktop_->dispatch();
            if (desktop_->closed()) {
                return finish(broker::EndReason::logout, "the desktop ended");
            }
        }
        if (Clock::now() >= next_stats) {
            send_stats();
            next_stats = Clock::now() + config_.stats_period;
        }
    }
}

namespace {

/// Connects to farlandd's agent socket, or nothing when it is not there
/// (which is the usual answer while farlandd is restarting).
std::optional<UniqueFd> connect_agent_socket(const std::string& path)
{
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        return std::nullopt;
    }
    std::memcpy(static_cast<void*>(address.sun_path), path.data(), path.size());
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd.valid()) {
        return std::nullopt;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the sockets API takes a generic sockaddr
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        return std::nullopt;
    }
    app::prepare_socket(fd.get());
    return fd;
}

}  // namespace

bool Agent::reattach(const std::atomic<bool>& stop)
{
    // Nowhere to look without the socket's path (an agent started the old
    // way). A desktop is not required: a session whose client has gone has
    // released it -- on GNOME the Mutter remote desktop session ends and the
    // seat gets its session back -- and that is precisely the session most
    // likely to be sitting there when a package upgrade restarts farlandd.
    // What comes back is the login session and its applications, which is
    // what the user cares about; the desktop is made again for the next
    // client either way.
    if (config_.socket_path.empty() || config_.reattach_timeout.count() <= 0) {
        return false;
    }
    log::warn(log_component, "session {}: farlandd is gone; keeping the session{} and looking for it for {} s",
              config_.logon_id, desktop_ ? " and its desktop" : "", config_.reattach_timeout.count());
    config_.daemon.reset();
    const auto deadline = Clock::now() + config_.reattach_timeout;
    while (!stop.load() && Clock::now() < deadline) {
        // The session goes on meanwhile: the client's socket is ours, not
        // farlandd's, so whoever is connected notices nothing.
        if (current_ && current_->finished.load()) {
            join_connection();
        }
        if (desktop_) {
            desktop_->dispatch();
            if (desktop_->closed()) {
                log::info(log_component, "session {}: the desktop ended while farlandd was away", config_.logon_id);
                return false;
            }
        }
        if (auto fd = connect_agent_socket(config_.socket_path)) {
            config_.daemon = std::move(*fd);
            daemon_gone_ = false;
            if (send(broker::Hello{broker::protocol_version, config_.token})) {
                log::info(log_component, "session {}: farlandd is back and has the session again", config_.logon_id);
                // Whoever is connected has to be reported again: the new
                // farlandd knows the session but not its connection.
                if (current_) {
                    send_stats();
                }
                return true;
            }
            config_.daemon.reset();
            daemon_gone_ = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    log::warn(log_component, "session {}: farlandd did not come back", config_.logon_id);
    return false;
}

bool Agent::on_new_connection(broker::NewConnection message, UniqueFd fd)
{
    if (current_) {
        log::info(log_component, "session {}: connection {} takes over from {}", config_.logon_id,
                  message.connection_id, current_->id);
        stop_connection(proto::errinfo::disconnected_by_other_connection);
        join_connection();
    }
    const auto started = Clock::now() - std::chrono::milliseconds(message.elapsed_ms);
    std::vector<std::byte> initial = std::move(message.pending_input);
    auto client = std::move(message.client);
    if (!desktop_) {
        if (!client && initial.empty()) {
            auto peek = read_connect_initial(fd.get(), 10'000);
            initial = std::move(peek.bytes);
            client = std::move(peek.client);
        }
        const auto [width, height] =
            client ? initial_size(*client) : std::pair<std::uint32_t, std::uint32_t>{1024, 768};
        // ask_for_greeter: farlandd runs as root, so it can switch the seat
        // to a greeter this process is refused (broker::SeatGreeter). Safe to
        // capture `this` -- the agent owns the desktop and outlives it, and
        // set_held() runs on this same thread.
        const DesktopRequest request{
            .width = width,
            .height = height,
            .render_node = config_.session.render_node,
            .frames_per_second = config_.session.frames_per_second,
            .ask_for_greeter = [this] { static_cast<void>(send(broker::SeatGreeter{})); },
        };
        auto desktop = config_.make_desktop(request);
        if (!desktop) {
            log::error(log_component, "session {}: cannot start the desktop: {}", config_.logon_id,
                       desktop.error().message());
            return false;
        }
        desktop_ = std::move(*desktop);
        log::info(log_component, "session {}: desktop started at {}x{}", config_.logon_id, width, height);
    }

    auto connection = std::make_unique<Connection>();
    connection->id = message.connection_id;
    connection->peer = message.peer;
    connection->control.on_client_info = [this](const std::optional<proto::AutoReconnectCookie>& cookie) {
        if (!cookie) {
            return;
        }
        // NLA already decided who this is; the cookie only says whether the
        // client comes back from this session's latest connection.
        if (arc_.verify(cookie->logon_id, cookie->security_verifier)) {
            ++cookies_matched_;
            log::info(log_component, "session {}: the client's auto-reconnect cookie matches", config_.logon_id);
        } else {
            ++cookies_mismatched_;
            log::warn(log_component, "session {}: the client's auto-reconnect cookie (logon id {}) does not match",
                      config_.logon_id, cookie->logon_id);
        }
    };
    connection->control.on_activated = [this]() -> std::optional<proto::LogonInfoExtended> {
        auto random = arc_.rotate();
        if (!random) {
            log::warn(log_component, "session {}: no auto-reconnect cookie: {}", config_.logon_id,
                      random.error().message());
            return std::nullopt;
        }
        proto::LogonInfoExtended info;
        info.auto_reconnect_cookie =
            proto::ServerAutoReconnectCookie{proto::auto_reconnect_version_1, config_.logon_id, *random};
        secure_zero(*random);
        return info;
    };
    app::SessionOptions options = config_.session;
    options.desktop = desktop_.get();
    options.control = &connection->control;
    log::info(log_component, "session {}: connection {} from {}", config_.logon_id, message.connection_id,
              message.peer);
    auto* raw = connection.get();
    connection->thread =
        std::thread([this, raw, options, fd = fd.release(), negotiation = std::move(message.negotiation), started,
                     initial = std::move(initial)]() mutable {
            {
                app::PlainTransport transport(fd, std::move(negotiation));
                app::run_session(transport, raw->peer, options, raw->stop, started, initial);
            }
            raw->finished = true;
            const char byte = 1;
            [[maybe_unused]] const ssize_t written = ::write(wake_write_.get(), &byte, 1);  // full pipe: already woken
        });
    current_ = std::move(connection);
    // The desktop takes a session on a local seat from the seat while a
    // client holds it, and gives it back when none does.
    desktop_->set_held(true);
    return true;
}

void Agent::stop_connection(std::uint32_t error_info)
{
    if (current_) {
        current_->control.stop_error_info = error_info;
        current_->stop = true;
    }
}

void Agent::join_connection()
{
    if (!current_) {
        return;
    }
    current_->thread.join();
    const std::uint32_t sent = current_->control.sent_error_info.load();
    log::info(log_component, "session {}: connection {} ended{}", config_.logon_id, current_->id,
              sent != 0 ? std::format(" (error info {:#x})", sent) : std::string());
    static_cast<void>(send(broker::Disconnect{current_->id, sent}));
    current_.reset();
    if (desktop_ && desktop_->keep_when_released()) {
        desktop_->set_held(false);
    } else if (desktop_) {
        // The session belongs to the seat again, whole: this desktop goes,
        // and the next connection takes it over anew.
        desktop_.reset();
        log::info(log_component, "session {}: the session is the seat's again until a client takes it",
                  config_.logon_id);
    }
}

void Agent::start_prompt(const broker::ConsentRequest& request)
{
    withdraw_prompt();  // one question at a time
    ConsentQuestion question;
    question.user = request.user;
    question.peer = request.peer;
    question.client_name = request.client_name;
    question.timeout = std::chrono::seconds(request.timeout_seconds);
    question.allow_on_timeout = request.allow_on_timeout;
    question.from_seat = request.from_seat;
    auto prompt = std::make_unique<Prompt>();
    prompt->connection = request.connection_id;
    auto* raw = prompt.get();
    prompt->thread = std::thread([this, raw, ask = config_.ask_consent, question = std::move(question)] {
        raw->answer = ask(question, raw->cancel);
        raw->finished = true;
        const char byte = 1;
        [[maybe_unused]] const ssize_t written = ::write(wake_write_.get(), &byte, 1);  // full pipe: already woken
    });
    prompt_ = std::move(prompt);
}

void Agent::withdraw_prompt()
{
    if (!prompt_) {
        return;
    }
    prompt_->cancel = true;
    if (prompt_->thread.joinable()) {
        prompt_->thread.join();
    }
    prompt_.reset();
}

void Agent::finish_prompt()
{
    if (!prompt_ || !prompt_->finished.load()) {
        return;
    }
    prompt_->thread.join();
    const broker::ConsentReply reply{prompt_->connection, prompt_->answer};
    prompt_.reset();
    log::info(log_component, "session {}: connection {} may take it over: {}", config_.logon_id, reply.connection_id,
              to_string(reply.answer));
    static_cast<void>(send(reply));
}

void Agent::send_stats()
{
    broker::Stats stats;
    const auto now = Clock::now();
    stats.session_seconds =
        static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::seconds>(now - started_).count());
    if (current_) {
        const auto& c = current_->control;
        stats.connection_id = current_->id;
        const std::int64_t last = c.last_input.load();
        if (last != 0) {
            const auto idle = now - Clock::time_point(Clock::duration(last));
            stats.idle_seconds = static_cast<std::uint32_t>(
                std::max<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(idle).count(), 0));
        }
        stats.desktop_width = static_cast<std::uint16_t>(c.desktop_width.load());
        stats.desktop_height = static_cast<std::uint16_t>(c.desktop_height.load());
        stats.frames_sent = c.frames_sent.load();
        stats.bytes_sent = c.bytes_sent.load();
        stats.bytes_received = c.bytes_received.load();
        stats.rtt_ms = c.rtt_ms.load();
        stats.bandwidth_kbps = c.bandwidth_kbps.load();
    }
    static_cast<void>(send(stats));
}

broker::EndReason Agent::finish(broker::EndReason reason, std::string detail)
{
    log::info(log_component, "session {}: ending ({}): {}", config_.logon_id, to_string(reason), detail);
    withdraw_prompt();
    stop_connection(error_info_for(reason));
    join_connection();
    desktop_.reset();
    static_cast<void>(send(broker::SessionEnded{reason, detail.substr(0, 1024)}));
    return reason;
}

}  // namespace farland::agent
