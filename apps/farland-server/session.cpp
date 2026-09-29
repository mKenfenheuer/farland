// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "session.hpp"

#include <farland/base/log.hpp>
#include <farland/channels/cliprdr.hpp>
#include <farland/server/clipboard_bridge.hpp>
#include <farland/server/connection.hpp>
#include <farland/server/display_control.hpp>
#include <farland/server/display_layout.hpp>
#include <farland/server/dynamic_channels.hpp>
#include <farland/server/graphics_pipeline.hpp>
#include <farland/server/loopback_clipboard.hpp>
#include <farland/server/touch_input.hpp>

#include "desktop_session.hpp"
#include "input_thread.hpp"
#include "session_audio.hpp"
#include "session_camera.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <poll.h>
#include <span>
#include <vector>

namespace farland::app {

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::string_view log_component = "app.session";
/// How often the quality ladder on the desktop thread hears what the
/// connection measured.
constexpr auto network_period = std::chrono::milliseconds(100);

/// One client over a transport, on the connection thread: the socket, the
/// RDP connection once pre-authentication is done, and its channels. Each
/// other job runs on a thread of its own and talks to this one by queue:
/// the picture (DesktopSession: the desktop or test pattern, its encoding
/// and its clipboard backend), keyboard, mouse and touch (InputThread), and
/// audio output (SessionAudio's PlaybackThread). This thread never waits for
/// any of them, so what the client sends is read, and input handed on, the
/// moment it arrives.
class SessionRunner {
public:
    SessionRunner(Transport& transport, std::string peer, const SessionOptions& options)
        : transport_(transport), peer_(std::move(peer)), options_(options), desktop_(options.desktop),
          picture_(std::make_unique<DesktopSession>(peer_, options_))
    {
    }

    void run(const std::atomic<bool>& stop, Clock::time_point started, std::span<const std::byte> initial_input)
    {
        std::vector<std::byte> data;
        start_connection_if_ready();
        if (connection_ && !initial_input.empty()) {
            count_received(initial_input.size());
            connection_->tick(Clock::now());
            connection_->receive(initial_input);
            pump();
        }
        while (running_) {
            if (stop.load()) {
                if (connection_) {
                    const std::uint32_t code = control() != nullptr ? control()->stop_error_info.load()
                                                                    : proto::errinfo::rpc_initiated_disconnect;
                    connection_->disconnect(code);
                    if (control() != nullptr) {
                        control()->sent_error_info = code;
                    }
                    pump();
                }
                break;
            }
            if (!active() && Clock::now() - started > std::chrono::seconds(options_.activation_timeout)) {
                log::warn(log_component, "{}: no active connection after {} s, dropping it", peer_,
                          options_.activation_timeout);
                break;
            }
            std::vector<pollfd> fds{pollfd{transport_.fd(), POLLIN, 0}, pollfd{picture_->wake_fd(), POLLIN, 0}};
            if (audio_) {
                audio_->add_fds(fds);
            }
            add_clipboard_fds(fds);
            ::poll(fds.data(), static_cast<nfds_t>(fds.size()), poll_timeout_ms());
            // The connection times auto-detect answers by its last tick, so
            // one comes before every receive.
            tick_connection();
            if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                data.clear();
                if (!transport_.read(data)) {
                    break;
                }
                count_received(data.size());
                start_connection_if_ready();
                if (connection_ && !data.empty()) {
                    connection_->tick(Clock::now());
                    connection_->receive(data);
                    pump();
                }
            }
            if (running_) {
                carry_out_desktop_output();
            }
            if (running_) {
                start_display_control();
                service_display();
            }
            if (running_) {
                service_clipboard();
            }
            if (running_ && audio_) {
                audio_->service(connection_->network().bandwidth_kbps);
                pump();
            }
            if (running_ && camera_) {
                camera_->service();
                pump();
            }
            publish_network();
        }
        // The picture first: it lets go of the input thread, which then
        // releases what is held and gives the input back to the desktop.
        picture_->stop();
        input_thread_.reset();
        camera_.reset();  // the local camera goes with the session
        audio_.reset();   // the capture and the microphone source go at once
        transport_.close();
    }

private:
    [[nodiscard]] bool active() const { return connection_ && connection_->active(); }

    [[nodiscard]] SessionControl* control() const { return options_.control; }

    void count_received(std::size_t bytes) const
    {
        if (control() != nullptr) {
            control()->bytes_received += bytes;
        }
    }

    /// Input (or the activation) now, for [policy] idle_timeout.
    void note_input() const
    {
        if (control() != nullptr) {
            control()->last_input = Clock::now().time_since_epoch().count();
        }
    }

    [[nodiscard]] int poll_timeout_ms() const
    {
        const auto now = Clock::now();
        auto until = now + std::chrono::milliseconds(250);
        if (display_) {
            if (const auto due = display_->deadline()) {
                until = std::min(until, *due);
            }
        }
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count();
        return static_cast<int>(std::clamp<std::int64_t>(wait, 0, 250));
    }

    /// Creates the Connection as soon as the transport has pre-authenticated.
    /// The desktop size is the bounding box of the layout the session shows,
    /// which the desktop thread chooses.
    void start_connection_if_ready()
    {
        if (connection_ || !transport_.ready()) {
            return;
        }
        server::ServerConfig config;
        config.autodetect = options_.autodetect;
        config.max_desktop_size = static_cast<std::uint16_t>(display_limits_.max_extent);
        config.choose_desktop_size = [this](const proto::gcc::ClientData& data) {
            return picture_->choose_desktop_size(data);
        };
        connection_.emplace(config, transport_.negotiation());
    }

    void send(std::span<const std::byte> bytes)
    {
        bytes_sent_ += bytes.size();
        if (control() != nullptr) {
            control()->bytes_sent += bytes.size();
        }
        if (!transport_.send(bytes)) {
            running_ = false;
        }
    }

    /// Lets the connection send what is due: RTT probes, heartbeats, the end
    /// of the connect-time detection.
    void tick_connection()
    {
        if (connection_ && running_) {
            connection_->tick(Clock::now());
            pump();
        }
        if (control() != nullptr && connection_) {
            const auto& network = connection_->network();
            const auto rtt =
                network.rtt ? std::chrono::duration_cast<std::chrono::milliseconds>(*network.rtt).count() : 0;
            control()->rtt_ms =
                static_cast<std::uint32_t>(std::clamp<std::int64_t>(rtt, 0, std::numeric_limits<std::uint32_t>::max()));
            control()->bandwidth_kbps = network.bandwidth_kbps.value_or(0);
        }
    }

    /// What the connection measured, to the quality ladder, now and then.
    void publish_network()
    {
        const auto now = Clock::now();
        if (!connection_ || now < network_due_) {
            return;
        }
        network_due_ = now + network_period;
        picture_->set_network(connection_->network(), bytes_sent_);
    }

    /// Output first, then events (the Connection's contract); again after
    /// each event, so whatever a handler queued goes out at once.
    void pump()
    {
        for (;;) {
            send(connection_->take_output());
            auto event = connection_->poll_event();
            if (!event) {
                break;
            }
            std::visit([this](auto& e) { on_event(e); }, *event);
        }
    }

    /// What the desktop thread produced: the graphics channel's messages,
    /// pointer and bitmap updates, and what it asks of the connection.
    void carry_out_desktop_output()
    {
        for (auto& output : picture_->take_output()) {
            std::visit(
                [this](auto& o) {
                    using T = std::decay_t<decltype(o)>;
                    if constexpr (std::is_same_v<T, desktop_output::Graphics>) {
                        if (dvc_ && gfx_channel_ == o.channel) {
                            static_cast<void>(dvc_->send(o.channel, o.message));
                        }
                    } else if constexpr (std::is_same_v<T, desktop_output::CloseGraphics>) {
                        if (dvc_ && gfx_channel_ == o.channel) {
                            dvc_->close(o.channel);
                            gfx_channel_.reset();
                        }
                    } else if constexpr (std::is_same_v<T, desktop_output::Pointer>) {
                        if (active()) {
                            connection_->send_pointer(o.update);
                        }
                    } else if constexpr (std::is_same_v<T, desktop_output::Bitmap>) {
                        // Not while reactivating: the next picture comes at the new size.
                        if (active()) {
                            connection_->send_bitmap_update(o.update);
                        }
                    } else if constexpr (std::is_same_v<T, desktop_output::Reactivate>) {
                        if (active()) {
                            connection_->reactivate(o.width, o.height);
                        }
                    } else if constexpr (std::is_same_v<T, desktop_output::LayoutShown>) {
                        if (display_) {
                            display_->set_current(o.layout);
                        }
                    } else if constexpr (std::is_same_v<T, desktop_output::Disconnect>) {
                        if (connection_) {
                            connection_->disconnect(o.error_info);
                            if (control() != nullptr) {
                                control()->sent_error_info = o.error_info;
                            }
                        }
                        ending_ = true;
                    }
                },
                output);
            if (connection_) {
                pump();
            }
        }
        if (ending_) {
            running_ = false;
        }
    }

    /// Starts the dynamic virtual channels once the connection is active.
    void start_dynamic_channels()
    {
        const auto id = connection_->session().static_channel_id("drdynvc");
        if (dvc_ || !id) {
            return;
        }
        dvc_channel_ = *id;
        dvc_.emplace([this](std::span<const std::byte> chunk) { connection_->send_channel_data(dvc_channel_, chunk); });
        dvc_->start();
    }

    void poll_dynamic_channels()
    {
        while (auto event = dvc_->poll_event()) {
            if (graphics_event(*event)) {
                continue;
            }
            if (touch_ && touch_->handle(*event)) {
                poll_touch();
                continue;
            }
            if (display_ && display_->handle(*event, Clock::now())) {
                continue;
            }
            if (audio_ && audio_->handle(*event)) {
                continue;
            }
            if (camera_ && camera_->handle(*event)) {
                continue;
            }
            std::visit(
                [this](const auto& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, channels::dvc_event::CapabilitiesReady>) {
                        log::info(log_component, "{}: dynamic channels ready (drdynvc version {})", peer_, e.version);
                        dynamic_channels_ready_ = true;
                        start_graphics();
                        start_touch();
                        start_display_control();
                        if (audio_) {
                            audio_->dynamic_channels_ready(*dvc_);
                        }
                        if (options_.camera) {
                            camera_.emplace(peer_);
                            camera_->dynamic_channels_ready(*dvc_);
                        }
                    } else if constexpr (std::is_same_v<T, channels::dvc_event::ChannelOpened>) {
                        log::info(log_component, "{}: dynamic channel {} '{}' open", peer_, e.id, e.name);
                    } else if constexpr (std::is_same_v<T, channels::dvc_event::ChannelOpenFailed>) {
                        log::info(log_component, "{}: client refused dynamic channel '{}' ({:#x})", peer_, e.name,
                                  static_cast<std::uint32_t>(e.status));
                    } else if constexpr (std::is_same_v<T, channels::dvc_event::ChannelData>) {
                        log::debug(log_component, "{}: {} bytes on dynamic channel {}", peer_, e.data.size(), e.id);
                    } else if constexpr (std::is_same_v<T, channels::dvc_event::ChannelClosed>) {
                        log::info(log_component, "{}: dynamic channel {} closed", peer_, e.id);
                    }
                },
                *event);
        }
    }

    /// Events of the graphics channel go to the pipeline on the desktop
    /// thread. False for other events.
    bool graphics_event(const channels::DvcEvent& event)
    {
        if (!gfx_channel_) {
            return false;
        }
        return std::visit(
            [this, &event](const auto& e) -> bool {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, channels::dvc_event::CapabilitiesReady>) {
                    return false;
                } else {
                    if (e.id != *gfx_channel_) {
                        return false;
                    }
                    if constexpr (std::is_same_v<T, channels::dvc_event::ChannelOpened>) {
                        log::info(log_component, "{}: dynamic channel {} '{}' open", peer_, e.id, e.name);
                    }
                    picture_->graphics_event(event);
                    if constexpr (std::is_same_v<T, channels::dvc_event::ChannelOpenFailed> ||
                                  std::is_same_v<T, channels::dvc_event::ChannelClosed>) {
                        gfx_channel_.reset();  // gone from the client's side
                    }
                    return true;
                }
            },
            event);
    }

    /// Opens the graphics channel when the client can run the Graphics
    /// Pipeline; the pipeline itself runs on the desktop thread.
    void start_graphics()
    {
        if (gfx_channel_ || !connection_->session().supports_gfx()) {
            return;
        }
        gfx_channel_ = dvc_->open(std::string(server::GraphicsPipeline::channel_name));
        picture_->start_graphics(*gfx_channel_);
    }

    /// Opens the touch and pen input channel ([MS-RDPEI]); clients without
    /// a digitizer refuse it.
    void start_touch()
    {
        if (!touch_) {
            touch_.emplace(*dvc_);
        }
    }

    /// Hands touch and pen frames to the input thread, or to the desktop
    /// thread for the test pattern and for input that stays there.
    void poll_touch()
    {
        bool closed = false;
        while (auto event = touch_->poll_event()) {
            std::visit(
                [this, &closed](auto& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, channels::rdpei::event::Ready>) {
                        log::info(log_component, "{}: touch input ready (RDPEI version {:#010x}, {} contacts{})", peer_,
                                  e.client.protocol_version, e.max_touch_contacts, e.pen ? ", pen" : "");
                    } else if constexpr (std::is_same_v<T, channels::rdpei::event::Frame>) {
                        note_input();
                        if (input_thread_) {
                            input_thread_->translate(std::move(e.contacts));
                        } else {
                            picture_->touch(std::move(e.contacts));
                        }
                    } else if constexpr (std::is_same_v<T, server::touch_event::Closed>) {
                        log::info(log_component, "{}: no touch input: {}", peer_, e.reason);
                        closed = true;
                    }
                },
                *event);
        }
        if (closed) {
            touch_.reset();
        }
    }

    /// Opens the Display Control channel, over which the client resizes the
    /// desktop and changes its monitors, once the desktop thread showed a
    /// layout.
    void start_display_control()
    {
        if (display_ || !dynamic_channels_ready_ || !dvc_) {
            return;
        }
        const auto layout = picture_->current_layout();
        if (!layout) {
            return;
        }
        server::DisplayControl::Config config;
        config.limits = display_limits_;
        display_.emplace(*dvc_, *layout, config);
    }

    /// Hands the layout the client asked for to the desktop thread, once it
    /// settled.
    void service_display()
    {
        if (!display_ || !active()) {
            return;
        }
        if (auto layout = display_->poll_layout(Clock::now())) {
            picture_->client_layout(std::move(*layout));
        }
    }

    void on_event(server::event::ClientInfo& e)
    {
        log::info(log_component, "{}: user '{}'{}", peer_, e.user_name, e.password.empty() ? "" : " (password sent)");
        if (control() != nullptr && control()->on_client_info) {
            control()->on_client_info(e.auto_reconnect_cookie);
        }
    }

    void on_event(server::event::Activated& e)
    {
        const auto& session = connection_->session();
        if (desktop_ != nullptr && !input_started_) {
            // Keyboard, mouse and touch on a thread of their own, where the
            // desktop lets its input go; otherwise on the desktop thread.
            input_started_ = true;
            input_thread_ = InputThread::start(*desktop_);
        }
        picture_->activated(session, connection_->max_update_size(), e.reactivation, input_thread_);
        start_dynamic_channels();
        start_audio();
        start_clipboard();
        if (control() != nullptr) {
            control()->desktop_width = session.desktop_width;
            control()->desktop_height = session.desktop_height;
            if (!e.reactivation) {
                note_input();
                if (control()->on_activated) {
                    if (const auto info = control()->on_activated()) {
                        connection_->send_save_session_info(*info);
                    }
                }
            }
        }
    }

    /// Audio starts with the first activation and lasts the session.
    void start_audio()
    {
        if (audio_ || (!options_.audio && !options_.microphone)) {
            return;
        }
        audio_.emplace(peer_, AudioOptions{.playback = options_.audio, .microphone = options_.microphone});
        audio_->start(connection_->session(), [this](std::uint16_t channel_id, std::span<const std::byte> chunk) {
            connection_->send_channel_data(channel_id, chunk);
        });
    }

    /// The clipboard channel (cliprdr), once, when the client asked for it
    /// and the desktop has a clipboard (through the desktop thread). The test
    /// pattern has a loopback one.
    void start_clipboard()
    {
        const auto id = connection_->session().static_channel_id(channels::cliprdr::channel_name);
        if (clipboard_ || !id || !options_.clipboard) {
            return;
        }
        clipboard_source_ = desktop_ != nullptr ? picture_->clipboard() : &loopback_clipboard_.emplace();
        if (clipboard_source_ == nullptr) {
            return;
        }
        clipboard_channel_ = *id;
        clipboard_.emplace(*clipboard_source_, [this](std::span<const std::byte> chunk) {
            connection_->send_channel_data(clipboard_channel_, chunk);
        });
        clipboard_->start();
    }

    void add_clipboard_fds(std::vector<pollfd>& fds) const
    {
        if (clipboard_) {
            for (const auto& fd : clipboard_source_->poll_fds()) {
                fds.push_back(pollfd{fd.fd, fd.events, 0});
            }
        }
    }

    void service_clipboard()
    {
        if (clipboard_ && connection_) {
            clipboard_->service(Clock::now());
            pump();
        }
    }

    void on_event(server::event::Input& e)
    {
        note_input();
        if (input_thread_) {
            input_thread_->translate(std::move(e.events));
        } else {
            picture_->input(std::move(e.events));
        }
    }

    void on_event(server::event::RefreshRequested& e) { picture_->refresh(std::move(e.areas)); }

    void on_event(server::event::OutputSuppressed& e) { picture_->suppress_output(e.suppressed); }

    void on_event(server::event::ChannelData& e)
    {
        if (clipboard_ && e.channel_id == clipboard_channel_) {
            if (auto received = clipboard_->receive(e.data, Clock::now()); !received) {
                log::warn(log_component, "{}: clipboard channel stopped: {}", peer_, received.error().message());
            }
            return;
        }
        if (dvc_ && e.channel_id == dvc_channel_) {
            if (auto received = dvc_->receive(e.data); !received) {
                log::warn(log_component, "{}: drdynvc: {}", peer_, received.error().message());
                connection_->disconnect(proto::errinfo::none);
                return;
            }
            poll_dynamic_channels();
            return;
        }
        if (audio_ && audio_->receive_static(e.channel_id, e.data)) {
            return;
        }
        log::debug(log_component, "{}: {} bytes on channel {} (no channel handlers yet)", peer_, e.data.size(),
                   e.channel_id);
    }

    void on_event(server::event::ShutdownRequested& /*e*/)
    {
        log::info(log_component, "{}: client requested shutdown", peer_);
        connection_->disconnect(proto::errinfo::none);
    }

    void on_event(server::event::Closed& e)
    {
        log::info(log_component, "{}: {}{}", peer_, e.error ? "protocol error: " : "", e.reason);
        running_ = false;
    }

    Transport& transport_;
    std::string peer_;
    SessionOptions options_;
    Desktop* desktop_;
    /// The picture: after options_, which it reads, and before the channels,
    /// which go first (the clipboard bridge uses its clipboard).
    std::unique_ptr<DesktopSession> picture_;
    std::optional<server::Connection> connection_;
    std::optional<server::DynamicChannels> dvc_;
    std::uint16_t dvc_channel_ = 0;
    bool dynamic_channels_ready_ = false;
    /// The graphics channel, while the pipeline on the desktop thread has one.
    std::optional<std::uint32_t> gfx_channel_;
    std::optional<server::TouchInput> touch_;                      // after dvc_, which it uses
    std::optional<server::LoopbackClipboard> loopback_clipboard_;  // before clipboard_, which uses it
    platform::Clipboard* clipboard_source_ = nullptr;
    std::optional<server::ClipboardBridge> clipboard_;
    std::uint16_t clipboard_channel_ = 0;
    std::uint64_t bytes_sent_ = 0;
    Clock::time_point network_due_;
    bool running_ = true;
    /// The desktop went away; the connection ends once its last words are out.
    bool ending_ = false;
    server::DisplayLimits display_limits_;
    std::optional<server::DisplayControl> display_;  // after dvc_, which it uses
    std::optional<SessionCamera> camera_;            // uses dvc_, so before it
    std::optional<SessionAudio> audio_;              // last of the channels: it uses dvc_
    /// Keyboard, mouse and touch on a thread of their own, where the desktop
    /// lets its input go; shared with the desktop thread, which tells it where
    /// the screens are.
    std::shared_ptr<InputThread> input_thread_;
    bool input_started_ = false;
};

}  // namespace

void run_session(Transport& transport, const std::string& peer, const SessionOptions& options,
                 const std::atomic<bool>& stop, std::chrono::steady_clock::time_point started,
                 std::span<const std::byte> initial_input)
{
    SessionRunner runner(transport, peer, options);
    runner.run(stop, started, initial_input);
}

void run_session(int fd, std::string peer, const auth::TlsIdentity& identity, const SessionOptions& options,
                 const std::atomic<bool>& stop)
{
    const auto started = Clock::now();
    prepare_socket(fd);
    NetworkStage network(fd, peer, identity, options.preauth, options.make_nla);
    run_session(network, peer, options, stop, started);
}

}  // namespace farland::app
