// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "session.hpp"

#include <farland/base/log.hpp>
#include <farland/platform/input_translator.hpp>
#include <farland/server/connection.hpp>
#include <farland/server/cursor_encoder.hpp>
#include <farland/server/dynamic_channels.hpp>
#include <farland/server/frame_scheduler.hpp>
#include <farland/server/graphics_pipeline.hpp>
#include <farland/server/test_pattern.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <poll.h>
#include <vector>

namespace farland::app {

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::string_view log_component = "app.session";

/// One client over a transport: the Connection once pre-authentication is
/// done, with the shared desktop (or the synthetic test pattern) behind.
class SessionRunner {
public:
    SessionRunner(Transport& transport, std::string peer, const SessionOptions& options)
        : transport_(transport), peer_(std::move(peer)), options_(options), desktop_(options.desktop),
          frame_interval_(std::chrono::microseconds(1'000'000 / std::max(options.frames_per_second, 1U)))
    {
    }

    void run(const std::atomic<bool>& stop, Clock::time_point started)
    {
        std::vector<std::byte> data;
        start_connection_if_ready();
        while (running_) {
            if (stop.load()) {
                if (connection_) {
                    connection_->disconnect(proto::errinfo::rpc_initiated_disconnect);
                    pump();
                }
                break;
            }
            if (!active() && Clock::now() - started > std::chrono::seconds(options_.activation_timeout)) {
                log::warn(log_component, "{}: no active connection after {} s, dropping it", peer_,
                          options_.activation_timeout);
                break;
            }
            std::vector<pollfd> fds{pollfd{transport_.fd(), POLLIN, 0}};
            add_desktop_fds(fds);
            ::poll(fds.data(), static_cast<nfds_t>(fds.size()), poll_timeout_ms());
            if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                data.clear();
                if (!transport_.read(data)) {
                    break;
                }
                start_connection_if_ready();
                if (connection_ && !data.empty()) {
                    connection_->receive(data);
                    pump();
                }
            }
            if (running_ && desktop_ != nullptr) {
                service_desktop(std::span(fds).subspan(1));
            }
            if (running_) {
                send_frame_if_due();
            }
        }
        if (translator_) {
            translator_->release_all();  // never leave keys held on the shared desktop
        }
        transport_.close();
    }

private:
    [[nodiscard]] bool active() const { return connection_ && connection_->active(); }

    [[nodiscard]] int poll_timeout_ms() const
    {
        if (!active() || suppressed_) {
            return 250;
        }
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_frame_ - Clock::now()).count();
        return static_cast<int>(std::clamp<std::int64_t>(wait, 0, 250));
    }

    /// Creates the Connection as soon as the transport has pre-authenticated.
    /// A shared desktop dictates the desktop size.
    void start_connection_if_ready()
    {
        if (connection_ || !transport_.ready()) {
            return;
        }
        server::ServerConfig config;
        if (desktop_ != nullptr) {
            const auto [width, height] = desktop_->frames().size();
            if (width > 0 && height > 0) {
                config.desktop_size = std::pair{static_cast<std::uint16_t>(std::min<std::uint32_t>(width, 0xFFFF)),
                                                static_cast<std::uint16_t>(std::min<std::uint32_t>(height, 0xFFFF))};
            }
        }
        connection_.emplace(config, transport_.negotiation());
    }

    void add_desktop_fds(std::vector<pollfd>& fds) const
    {
        if (desktop_ == nullptr) {
            return;
        }
        const int cursor_fd = desktop_->cursor() != nullptr ? desktop_->cursor()->wake_fd() : -1;
        for (const int fd : {desktop_->frames().wake_fd(), cursor_fd}) {
            if (fd >= 0) {
                fds.push_back(pollfd{fd, POLLIN, 0});
            }
        }
        for (const int fd : desktop_->dispatch_fds()) {
            if (fd >= 0) {
                fds.push_back(pollfd{fd, POLLIN, 0});
            }
        }
    }

    /// Takes what the desktop has: its events, the newest frame, cursor changes.
    void service_desktop(std::span<const pollfd> fds)
    {
        const bool readable = std::ranges::any_of(
            fds, [](const pollfd& pfd) { return (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0; });
        if (!readable && desktop_->frames().wake_fd() >= 0) {
            return;
        }
        desktop_->dispatch();
        if (desktop_->closed()) {
            log::info(log_component, "{}: the shared desktop went away", peer_);
            if (connection_) {
                connection_->disconnect(proto::errinfo::none);
                pump();
            }
            running_ = false;
            return;
        }
        if (auto frame = desktop_->frames().take_frame()) {
            desktop_image_ = frame->image;
            desktop_dirty_ = true;
            if (scheduler_) {
                scheduler_->damage();
            }
        }
        if (auto* source = desktop_->cursor()) {
            if (auto update = source->take_cursor()) {
                on_cursor(std::move(*update));
            }
        }
    }

    /// Sends a cursor change, or keeps it until the connection is active.
    void on_cursor(platform::CursorUpdate update)
    {
        if (cursor_ && active()) {
            for (const auto& pointer : cursor_->encode(update)) {
                connection_->send_pointer(pointer);
            }
            pump();
            return;
        }
        if (!pending_cursor_) {
            pending_cursor_ = std::move(update);
            return;
        }
        if (update.shape) {
            pending_cursor_->shape = std::move(update.shape);
        }
        if (update.position) {
            pending_cursor_->position = update.position;
        }
        pending_cursor_->visible = update.visible;
    }

    [[nodiscard]] bool has_picture() const { return desktop_ != nullptr || pattern_.has_value(); }

    /// The picture for the next frame: the desktop's newest frame when it
    /// changed, or the test pattern's next frame.
    [[nodiscard]] std::optional<codec::ImageView> next_image()
    {
        if (desktop_ == nullptr) {
            return pattern_->render(frame_++);
        }
        if (!desktop_image_ || !desktop_dirty_) {
            return std::nullopt;
        }
        const auto& session = connection_->session();
        if (desktop_image_->width != session.desktop_width || desktop_image_->height != session.desktop_height) {
            return std::nullopt;  // the desktop was resized; following it needs the disp channel (M6)
        }
        desktop_dirty_ = false;
        return desktop_image_;
    }

    void send(std::span<const std::byte> bytes)
    {
        if (!transport_.send(bytes)) {
            running_ = false;
        }
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
            if (gfx_ && gfx_->handle(*event)) {
                poll_pipeline();
                continue;
            }
            std::visit(
                [this](const auto& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, channels::dvc_event::CapabilitiesReady>) {
                        log::info(log_component, "{}: dynamic channels ready (drdynvc version {})", peer_, e.version);
                        start_graphics();
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

    [[nodiscard]] bool gfx_ready() const { return gfx_ && gfx_->ready() && scheduler_; }

    /// Opens the Graphics Pipeline when the client can run it.
    void start_graphics()
    {
        const auto& session = connection_->session();
        if (gfx_ || !session.supports_gfx()) {
            return;
        }
        channels::rdpgfx::GfxServerConfig config;
        config.avc420 = options_.gfx_codec == server::TileCodec::avc420;
        config.avc444 = config.avc444v2 = false;  // the chroma split comes with M5
        server::H264Factory make_h264;
        if (config.avc420) {
            make_h264 = [library = options_.openh264_library,
                         fps = options_.frames_per_second](const video::EncoderConfig& requested) {
                auto encoder_config = requested;
                encoder_config.fps = fps;
                return video::create_encoder(video::compiled_backends(), encoder_config,
                                             video::BackendOptions{library});
            };
        }
        gfx_.emplace(*dvc_, session.desktop_width, session.desktop_height, config, options_.gfx_codec,
                     std::move(make_h264));
    }

    void poll_pipeline()
    {
        bool closed = false;
        while (auto event = gfx_->poll_event()) {
            std::visit(
                [this, &closed](const auto& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, server::pipeline_event::Ready>) {
                        // The frame clock sets the pace; the scheduler's own
                        // cap only has to stay out of its way.
                        scheduler_.emplace(server::FrameScheduler::Config{
                            .max_fps = options_.frames_per_second * 2,
                            .max_frames_in_flight = 2,
                            .ack_timeout = std::chrono::seconds(1),
                            .acknowledgements = true,
                        });
                        scheduler_->damage();
                        next_frame_ = Clock::now();
                        log::info(log_component, "{}: graphics pipeline active, {}", peer_, [&] {
                            switch (gfx_->codec()) {
                            case server::TileCodec::avc420:
                                return "H.264 (AVC420)";
                            case server::TileCodec::progressive:
                                return "Progressive tiles";
                            case server::TileCodec::planar:
                                break;
                            }
                            return "planar tiles";
                        }());
                    } else if constexpr (std::is_same_v<T, server::pipeline_event::FrameAcked>) {
                        if (scheduler_) {
                            const bool suspend = e.queue_depth == channels::rdpgfx::suspend_frame_acknowledgement;
                            scheduler_->set_acknowledgements_suspended(suspend);
                            if (!suspend) {
                                scheduler_->frame_acknowledged(e.frame_id, Clock::now());
                            }
                        }
                    } else if constexpr (std::is_same_v<T, server::pipeline_event::Closed>) {
                        log::info(log_component, "{}: back to bitmap updates: {}", peer_, e.reason);
                        closed = true;
                    }
                },
                *event);
        }
        if (closed) {
            gfx_.reset();
            scheduler_.reset();
            if (encoder_) {
                encoder_->invalidate_all();
            }
        }
    }

    /// Every few seconds: frame rate, acknowledgements and round trip.
    void log_gfx_statistics(Clock::time_point now)
    {
        constexpr auto period = std::chrono::seconds(5);
        if (gfx_statistics_since_ == Clock::time_point{}) {
            gfx_statistics_since_ = now;
            return;
        }
        const auto elapsed = now - gfx_statistics_since_;
        if (elapsed < period) {
            return;
        }
        const double seconds = std::chrono::duration<double>(elapsed).count();
        const auto round_trip = scheduler_->round_trip();
        log::info(log_component, "{}: GFX {:.1f} fps, {} in flight, round trip {}", peer_,
                  static_cast<double>(gfx_frames_) / seconds, scheduler_->frames_in_flight(),
                  round_trip ? std::format("{:.1f} ms", std::chrono::duration<double, std::milli>(*round_trip).count())
                             : std::string("unknown"));
        gfx_frames_ = 0;
        gfx_statistics_since_ = now;
    }

    void send_gfx_frame_if_due()
    {
        if (suppressed_ || !has_picture()) {
            return;
        }
        const auto now = Clock::now();
        if (now < next_frame_) {
            return;
        }
        next_frame_ += frame_interval_;
        if (next_frame_ < now) {
            next_frame_ = now + frame_interval_;
        }
        if (desktop_ == nullptr) {
            scheduler_->damage();  // the test pattern moves in every frame
        }
        if (!scheduler_->due(now)) {
            return;  // the client is behind, or nothing changed; the next frame covers everything
        }
        const auto image = next_image();
        if (!image) {
            return;
        }
        if (const auto frame_id = gfx_->send_frame(*image)) {
            scheduler_->frame_sent(*frame_id, now);
            ++gfx_frames_;
        }
        log_gfx_statistics(now);
        pump();
        poll_pipeline();  // an encoder failure closes the pipeline
    }

    void on_event(server::event::ClientInfo& e)
    {
        log::info(log_component, "{}: user '{}'{}", peer_, e.user_name, e.password.empty() ? "" : " (password sent)");
    }

    void on_event(server::event::Activated& e)
    {
        const auto& session = connection_->session();
        const auto codec = session.bits_per_pixel == 32 ? options_.codec : server::BitmapCodec::uncompressed;
        if (desktop_ != nullptr) {
            start_desktop_session(session);
        } else if (!pattern_) {
            pattern_.emplace(session.desktop_width, session.desktop_height);
        } else {
            pattern_->resize(session.desktop_width, session.desktop_height);
        }
        encoder_.emplace(session.desktop_width, session.desktop_height, session.bits_per_pixel, codec,
                         session.no_bitmap_compression_header);
        next_frame_ = Clock::now();
        log::info(log_component, "{}: {} {}x{}, {} bitmaps", peer_, e.reactivation ? "reactivated" : "active",
                  session.desktop_width, session.desktop_height,
                  codec == server::BitmapCodec::planar ? "planar" : "uncompressed");
        if (e.reactivation && gfx_) {
            // The surface has the old size; resizing it comes with the disp channel (M6).
            log::info(log_component, "{}: desktop resized, graphics pipeline dropped", peer_);
            gfx_.reset();
            scheduler_.reset();
        }
        start_dynamic_channels();
    }

    /// On (re)activation with a shared desktop: the cursor (re)starts and
    /// input maps client coordinates onto the desktop.
    void start_desktop_session(const server::Session& session)
    {
        desktop_dirty_ = desktop_image_.has_value();
        if (desktop_->cursor() != nullptr) {
            const auto config = server::CursorEncoder::Config::negotiated(session, connection_->max_update_size());
            if (!cursor_) {
                cursor_.emplace(config);
            }
            for (const auto& pointer : cursor_->reset(config)) {
                connection_->send_pointer(pointer);
            }
            if (pending_cursor_) {
                for (const auto& pointer : cursor_->encode(*pending_cursor_)) {
                    connection_->send_pointer(pointer);
                }
                pending_cursor_.reset();
            }
        }
        if (!translator_) {
            translator_.emplace(desktop_->input());
        }
        const auto [width, height] = desktop_->frames().size();
        translator_->set_geometry(session.desktop_width, session.desktop_height, width, height);
    }

    void on_event(server::event::Input& e)
    {
        if (translator_) {
            translator_->translate(e.events);
            return;
        }
        for (const auto& input : e.events) {
            if (pattern_) {
                pattern_->apply(input);
            }
            log::debug(log_component, "{}: input {}", peer_, server::TestPattern::describe(input));
        }
    }

    void on_event(server::event::RefreshRequested& e)
    {
        if (gfx_ready()) {
            gfx_->invalidate_all();
            return;
        }
        if (encoder_) {
            for (const auto& area : e.areas) {
                encoder_->invalidate(area);
            }
        }
    }

    void on_event(server::event::OutputSuppressed& e)
    {
        suppressed_ = e.suppressed;
        if (!suppressed_ && gfx_ready()) {
            gfx_->invalidate_all();
        }
        if (!suppressed_ && encoder_) {
            encoder_->invalidate_all();
        }
    }

    void on_event(server::event::ChannelData& e)
    {
        if (dvc_ && e.channel_id == dvc_channel_) {
            if (auto received = dvc_->receive(e.data); !received) {
                log::warn(log_component, "{}: drdynvc: {}", peer_, received.error().message());
                connection_->disconnect(proto::errinfo::none);
                return;
            }
            poll_dynamic_channels();
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

    void send_frame_if_due()
    {
        if (gfx_ready()) {
            send_gfx_frame_if_due();
            return;
        }
        if (!active() || suppressed_ || !encoder_ || !has_picture()) {
            return;
        }
        const auto now = Clock::now();
        if (now < next_frame_) {
            return;
        }
        if (const auto image = next_image()) {
            for (const auto& update : encoder_->encode(*image, connection_->max_update_size())) {
                connection_->send_bitmap_update(update);
            }
            pump();
        }
        next_frame_ += frame_interval_;
        if (next_frame_ < now) {
            next_frame_ = now + frame_interval_;  // fell behind: drop frames rather than burst
        }
    }

    Transport& transport_;
    std::string peer_;
    SessionOptions options_;
    Desktop* desktop_;
    Clock::duration frame_interval_;
    std::optional<server::Connection> connection_;
    std::optional<server::DynamicChannels> dvc_;
    std::uint16_t dvc_channel_ = 0;
    std::optional<server::GraphicsPipeline> gfx_;  // after dvc_, which it uses
    std::optional<server::FrameScheduler> scheduler_;
    std::uint64_t gfx_frames_ = 0;
    Clock::time_point gfx_statistics_since_;
    bool running_ = true;
    bool suppressed_ = false;
    std::optional<server::TestPattern> pattern_;
    std::optional<codec::ImageView> desktop_image_;  ///< valid until the next take_frame()
    bool desktop_dirty_ = false;
    std::optional<server::CursorEncoder> cursor_;
    std::optional<platform::CursorUpdate> pending_cursor_;
    std::optional<platform::InputTranslator> translator_;
    std::optional<server::FrameEncoder> encoder_;
    std::uint64_t frame_ = 0;
    Clock::time_point next_frame_;
};

}  // namespace

void run_session(Transport& transport, const std::string& peer, const SessionOptions& options,
                 const std::atomic<bool>& stop, std::chrono::steady_clock::time_point started)
{
    SessionRunner runner(transport, peer, options);
    runner.run(stop, started);
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
