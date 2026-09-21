// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "session.hpp"

#include <farland/base/log.hpp>
#include <farland/channels/cliprdr.hpp>
#include <farland/platform/input_translator.hpp>
#include <farland/server/clipboard_bridge.hpp>
#include <farland/server/compositor.hpp>
#include <farland/server/connection.hpp>
#include <farland/server/cursor_encoder.hpp>
#include <farland/server/display_control.hpp>
#include <farland/server/display_layout.hpp>
#include <farland/server/dynamic_channels.hpp>
#include <farland/server/frame_scheduler.hpp>
#include <farland/server/graphics_pipeline.hpp>
#include <farland/server/latency.hpp>
#include <farland/server/loopback_clipboard.hpp>
#include <farland/server/quality_controller.hpp>
#include <farland/server/test_pattern.hpp>
#include <farland/server/touch_input.hpp>

#include "session_audio.hpp"
#include "session_camera.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <poll.h>
#include <span>
#include <vector>

namespace farland::app {

namespace {

using Clock = std::chrono::steady_clock;
using Size = std::pair<std::uint32_t, std::uint32_t>;
constexpr std::string_view log_component = "app.session";
constexpr std::size_t max_frames_in_flight = 2;
/// Damage rectangles collected between two frames; more mean everything.
constexpr std::size_t max_damage_rects = 64;

/// How long a client that advertised the Graphics Pipeline may take to open
/// it before the legacy bitmap path starts painting anyway. Bitmap updates
/// carry the whole desktop, losslessly, with no quality ladder behind them,
/// so on a slow link the first one is megabytes -- and the Graphics Pipeline's
/// own negotiation queues up behind it on the same TCP connection, which is
/// what turned a connect into a 30 s wait and then a session nobody stayed
/// in. Waiting costs a blank screen for at most this long, and only for
/// clients that said they would open it.
constexpr std::chrono::seconds gfx_grace{5};
/// Dmabufs the encoder refuses in a row before the session stops asking the
/// capture for them. A single refusal costs only that frame's CPU copy.
constexpr unsigned max_dmabuf_refusals = 3;

video::DmabufFrame to_dmabuf_frame(const platform::Dmabuf& dmabuf)
{
    video::DmabufFrame frame;
    frame.fourcc = dmabuf.drm_format;
    frame.modifier = dmabuf.modifier;
    frame.width = dmabuf.width;
    frame.height = dmabuf.height;
    frame.plane_count = dmabuf.plane_count;
    for (std::size_t i = 0; i < frame.planes.size(); ++i) {
        const platform::DmabufPlane& plane = dmabuf.planes.at(i);
        frame.planes.at(i) = video::DmabufPlane{plane.fd, plane.offset, plane.pitch};
    }
    return frame;
}

/// The size of a frame: its pixels', else its dmabuf's; 0 x 0 without either.
Size frame_size(const platform::Frame& frame)
{
    if (!frame.image.data.empty()) {
        return {frame.image.width, frame.image.height};
    }
    if (frame.dmabuf) {
        return {frame.dmabuf->width, frame.dmabuf->height};
    }
    return {0, 0};
}

Clock::duration interval_for(unsigned fps)
{
    return std::chrono::microseconds(1'000'000 / std::max(fps, 1U));
}

std::string milliseconds(std::optional<Clock::duration> d)
{
    return d ? std::format("{:.1f} ms", std::chrono::duration<double, std::milli>(*d).count()) : std::string("unknown");
}

/// One client over a transport: the Connection once pre-authentication is
/// done, with the shared desktop (or the synthetic test pattern) behind.
///
/// The screens (the desktop's monitors, or one test pattern per client
/// monitor) are put on the client's monitors (server::DisplayLayout): GFX
/// clients get a surface per screen, bitmap updates one composed picture.
/// The layout comes from CS_MONITOR, and later from the Display Control
/// channel; a shared desktop whose screens keep their size shows them at
/// their size to single-monitor clients until they ask for another layout.
class SessionRunner {
public:
    SessionRunner(Transport& transport, std::string peer, const SessionOptions& options)
        : transport_(transport), peer_(std::move(peer)), options_(options), desktop_(options.desktop),
          frame_interval_(interval_for(options.frames_per_second))
    {
        if (desktop_ != nullptr) {
            screens_.resize(desktop_->screen_count());
            for (std::size_t i = 0; i < screens_.size(); ++i) {
                screens_[i].size = desktop_->screen_frames(i).size();
            }
        }
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
            std::vector<pollfd> fds{pollfd{transport_.fd(), POLLIN, 0}};
            add_desktop_fds(fds);
            const std::size_t audio_fds = fds.size();
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
            if (running_ && desktop_ != nullptr) {
                service_desktop(std::span(fds).subspan(1, audio_fds - 1));
            }
            if (running_) {
                service_display();
            }
            if (running_) {
                service_clipboard();
            }
            if (running_) {
                send_frame_if_due();
            }
            if (running_ && audio_) {
                audio_->service(connection_->network().bandwidth_kbps);
                pump();
            }
            if (running_ && camera_) {
                camera_->service();
                pump();
            }
        }
        if (translator_) {
            translator_->release_all();  // never leave keys held on the shared desktop
        }
        report_latency();
        camera_.reset();  // the local camera goes with the session
        audio_.reset();   // the capture and the microphone source go at once
        if (desktop_ != nullptr) {
            // The producers get their buffers back, and the next session
            // starts from CPU frames.
            for (std::size_t i = 0; i < screens_.size(); ++i) {
                desktop_->screen_frames(i).release_frame();
                desktop_->screen_frames(i).set_access(platform::FrameAccess::cpu);
            }
        }
        transport_.close();
    }

private:
    /// One screen of the shared desktop: its newest frame and what changed.
    struct Screen {
        /// Of the newest frame; the source's size before one came.
        Size size{0, 0};
        std::optional<codec::ImageView> image;   ///< valid until the next take_frame()
        std::optional<platform::Dmabuf> dmabuf;  ///< the same frame as a dmabuf, valid as long
        bool dirty = false;
        /// Damage of the frames taken since one went out (the dmabuf path's regions).
        std::vector<platform::Rect> damage;
        bool damage_full = false;
        /// What the session last asked the capture for; unset before it did.
        std::optional<platform::FrameAccess> access;
        std::optional<std::uint64_t> dmabuf_generation;
        bool logged_dmabuf_frame = false;
        /// The picture scaled down for a GFX surface the client cannot scale.
        server::ScaledPicture scaled;
    };

    [[nodiscard]] bool active() const { return connection_ && connection_->active(); }

    [[nodiscard]] SessionControl* control() const { return options_.control; }

    void count_received(std::size_t bytes) const
    {
        if (control() != nullptr) {
            control()->bytes_received += bytes;
        }
    }

    void count_frame() const
    {
        if (control() != nullptr) {
            ++control()->frames_sent;
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
        if (active() && !suppressed_) {
            until = std::min(until, next_frame_);
        }
        if (display_) {
            if (const auto due = display_->deadline()) {
                until = std::min(until, *due);
            }
        }
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count();
        return static_cast<int>(std::clamp<std::int64_t>(wait, 0, 250));
    }

    /// Creates the Connection as soon as the transport has pre-authenticated.
    /// The desktop size is the bounding box of the layout the session shows.
    void start_connection_if_ready()
    {
        if (connection_ || !transport_.ready()) {
            return;
        }
        server::ServerConfig config;
        config.autodetect = options_.autodetect;
        config.max_desktop_size = static_cast<std::uint16_t>(display_limits_.max_extent);
        config.choose_desktop_size = [this](const proto::gcc::ClientData& data) {
            auto layout = initial_layout(data);
            const std::pair size{static_cast<std::uint16_t>(layout.width()),
                                 static_cast<std::uint16_t>(layout.height())};
            connect_layout_ = std::move(layout);
            return size;
        };
        connection_.emplace(config, transport_.negotiation());
    }

    /// The layout at connect time: the client's monitors, except that a
    /// shared desktop whose screens keep their size shows them side by side
    /// at their size to a client with one monitor.
    server::DisplayLayout initial_layout(const proto::gcc::ClientData& data)
    {
        const server::ServerConfig defaults;
        const auto clamp = [&](std::uint16_t size) {
            return std::clamp<std::uint32_t>(size, defaults.min_desktop_size, display_limits_.max_size);
        };
        auto client = server::DisplayLayout::from_client_data(data, clamp(data.core.desktop_width),
                                                              clamp(data.core.desktop_height), display_limits_);
        if (desktop_ != nullptr && !desktop_->resizable() && client.monitors().size() == 1) {
            client_layout_ = false;
            return server::DisplayLayout::row(screen_sizes(), display_limits_);
        }
        client_layout_ = true;
        return client;
    }

    [[nodiscard]] std::size_t screen_count() const { return desktop_ != nullptr ? screens_.size() : patterns_.size(); }

    [[nodiscard]] std::vector<Size> screen_sizes() const
    {
        std::vector<Size> sizes;
        if (desktop_ != nullptr) {
            for (const auto& s : screens_) {
                sizes.push_back(s.size);
            }
        } else {
            for (const auto& pattern : patterns_) {
                sizes.emplace_back(pattern.width(), pattern.height());
            }
        }
        return sizes;
    }

    void add_desktop_fds(std::vector<pollfd>& fds) const
    {
        if (desktop_ == nullptr) {
            return;
        }
        for (std::size_t i = 0; i < screens_.size(); ++i) {
            const auto* cursor = desktop_->screen_cursor(i);
            for (const int fd : {desktop_->screen_frames(i).wake_fd(), cursor != nullptr ? cursor->wake_fd() : -1}) {
                if (fd >= 0) {
                    fds.push_back(pollfd{fd, POLLIN, 0});
                }
            }
        }
        for (const int fd : desktop_->dispatch_fds()) {
            if (fd >= 0) {
                fds.push_back(pollfd{fd, POLLIN, 0});
            }
        }
    }

    /// Takes what the desktop has: its events, each screen's newest frame,
    /// cursor changes.
    void service_desktop(std::span<const pollfd> fds)
    {
        const bool readable = std::ranges::any_of(
            fds, [](const pollfd& pfd) { return (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0; });
        if (!readable && desktop_->frames().wake_fd() >= 0) {
            return;
        }
        desktop_->dispatch();
        sync_screen_count();
        if (desktop_->closed()) {
            log::info(log_component, "{}: the shared desktop went away", peer_);
            if (connection_) {
                connection_->disconnect(proto::errinfo::none);
                pump();
            }
            running_ = false;
            return;
        }
        for (std::size_t i = 0; i < screens_.size(); ++i) {
            if (auto frame = desktop_->screen_frames(i).take_frame()) {
                latency_.frame_taken(Clock::now(), frame->captured);
                on_frame(i, std::move(*frame));
            }
        }
        for (std::size_t i = 0; i < screens_.size(); ++i) {
            if (auto* source = desktop_->screen_cursor(i)) {
                if (auto update = source->take_cursor()) {
                    on_cursor(i, std::move(*update));
                }
            }
        }
        if (screens_resized_) {
            screens_resized_ = false;
            on_screens_resized();
        }
    }

    /// Keeps a screen's newest frame (valid until the next take_frame()) and
    /// collects its damage for the dmabuf path, which cannot diff.
    void on_frame(std::size_t index, platform::Frame frame)
    {
        Screen& s = screens_[index];
        s.image.reset();
        if (!frame.image.data.empty()) {
            s.image = frame.image;
        }
        if (frame.dmabuf) {
            if (s.dmabuf_generation && *s.dmabuf_generation != frame.dmabuf->generation) {
                if (const auto surface = surface_of(index)) {
                    log::debug(log_component, "{}: the capture of screen {} replaced its buffers", peer_, index);
                    gfx_->forget_dmabufs(*surface);
                }
            }
            s.dmabuf_generation = frame.dmabuf->generation;
        }
        s.dmabuf = frame.dmabuf;
        if (frame.damage.empty()) {
            s.damage_full = true;
        } else if (!s.damage_full) {
            s.damage.insert(s.damage.end(), frame.damage.begin(), frame.damage.end());
            s.damage_full = s.damage.size() > max_damage_rects;
        }
        if (s.damage_full) {
            s.damage.clear();
        }
        s.dirty = true;
        if (const auto size = frame_size(frame); size.first != 0 && size != s.size) {
            s.size = size;
            screens_resized_ = true;
        }
        if (scheduler_) {
            scheduler_->damage();
        }
    }

    /// A desktop whose screens follow the client's monitors added or removed
    /// some (Desktop::screens_follow_monitors()): the removed ones are
    /// forgotten before their frames could be used again, new ones start
    /// from their source's size, and the output is laid out again.
    void sync_screen_count()
    {
        const std::size_t count = desktop_->screen_count();
        if (count == screens_.size()) {
            return;
        }
        log::info(log_component, "{}: the desktop has {} screen{} now", peer_, count, count == 1 ? "" : "s");
        const std::size_t old_count = screens_.size();
        screens_.resize(count);
        for (std::size_t i = old_count; i < count; ++i) {
            screens_[i].size = desktop_->screen_frames(i).size();
        }
        if (cursor_screen_ && *cursor_screen_ >= count) {
            cursor_screen_.reset();
        }
        screens_resized_ = false;
        on_screens_resized();
    }

    /// A screen changed its size (a virtual monitor followed the client, a
    /// monitor changed its mode): its placement, or the whole layout when the
    /// screens dictate it, changes too.
    void on_screens_resized()
    {
        if (!layout_ || !active()) {
            return;
        }
        if (!client_layout_) {
            apply_layout(server::DisplayLayout::row(screen_sizes(), display_limits_));
            return;
        }
        update_output();
    }

    /// Every screen's frame goes out again, all of it (after the output was
    /// laid out again or the client asked for a refresh).
    void redraw_desktop()
    {
        for (auto& s : screens_) {
            s.dirty = s.image.has_value() || s.dmabuf.has_value();
        }
        if (scheduler_) {
            scheduler_->damage();
        }
    }

    /// The damage of a screen's frames taken since the last one went out,
    /// clipped to `width` x `height`; one rectangle of all of it when a frame
    /// had none.
    static std::vector<server::PixelRect> take_damage(Screen& s, std::uint32_t width, std::uint32_t height)
    {
        std::vector<server::PixelRect> damage;
        if (s.damage_full) {
            damage.push_back({0, 0, width, height});
        }
        for (const platform::Rect& r : s.damage) {
            const std::int64_t left = std::max<std::int64_t>(r.x, 0);
            const std::int64_t top = std::max<std::int64_t>(r.y, 0);
            const std::int64_t right = std::min<std::int64_t>(std::int64_t{r.x} + r.width, width);
            const std::int64_t bottom = std::min<std::int64_t>(std::int64_t{r.y} + r.height, height);
            if (right > left && bottom > top) {
                damage.push_back({static_cast<std::uint32_t>(left), static_cast<std::uint32_t>(top),
                                  static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top)});
            }
        }
        s.damage.clear();
        s.damage_full = false;
        return damage;
    }

    /// The GFX surface that shows screen `index`, once the pipeline is ready.
    [[nodiscard]] std::optional<std::size_t> surface_of(std::size_t index) const
    {
        if (!gfx_ready()) {
            return std::nullopt;
        }
        const auto& placed = gfx_->layout().screens;
        for (std::size_t i = 0; i < std::min(placed.size(), gfx_->screen_count()); ++i) {
            if (placed[i].screen == index) {
                return i;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] const server::ScreenPlacement* placement_of(std::size_t index) const
    {
        const auto found = std::ranges::find(output_.screens, index, &server::ScreenPlacement::screen);
        return found != output_.screens.end() ? &*found : nullptr;
    }

    /// Frames go to the encoder as dmabufs only for AVC420 surfaces with an
    /// encoder that takes them and a picture the client gets at its size;
    /// Progressive, planar, AVC444 (whose 4:4:4 split runs on the CPU),
    /// scaled pictures and bitmap updates need the pixels. Logs the path
    /// when it changes.
    void choose_frame_access()
    {
        if (desktop_ == nullptr) {
            return;
        }
        for (std::size_t i = 0; i < screens_.size(); ++i) {
            Screen& s = screens_[i];
            const auto surface = surface_of(i);
            const bool same_size = surface && gfx_->surface_size(*surface) == s.size;
            const bool dmabuf =
                options_.zero_copy && !dmabuf_refused_ && surface && gfx_->accepts_dmabuf(*surface) && same_size;
            const auto access = dmabuf ? platform::FrameAccess::dmabuf : platform::FrameAccess::cpu;
            if (s.access == access) {
                continue;
            }
            s.access = access;
            desktop_->screen_frames(i).set_access(access);
            const std::string which = screens_.size() > 1 ? std::format(" of screen {}", i) : std::string();
            if (dmabuf) {
                log::info(log_component, "{}: frames{} go to the H.264 encoder as dmabufs, without a CPU copy", peer_,
                          which);
                continue;
            }
            const char* why = "bitmap updates";
            if (!options_.zero_copy) {
                why = "--no-zero-copy";
            } else if (dmabuf_refused_) {
                why = "the H.264 encoder cannot import the capture's dmabufs";
            } else if (surface && gfx_->codec() == server::TileCodec::avc420 && gfx_->accepts_dmabuf(*surface)) {
                why = "the picture is scaled to fit the client's monitor";
            } else if (gfx_ready()) {
                why = gfx_->codec() == server::TileCodec::avc420 ? "the H.264 encoder takes no dmabufs"
                                                                 : "the codec encodes from CPU memory";
            }
            log::info(log_component, "{}: frames{} are read into CPU memory ({})", peer_, which, why);
        }
    }

    /// The zero-copy path for GFX surface `surface` showing screen `index`:
    /// the frame goes to the encoder as the dmabuf it came in, with the
    /// capture's damage as regions. False when the frame has no dmabuf (or
    /// pixels already), the client needs it scaled, or the encoder refused
    /// the buffer; then its pixels go the CPU way.
    bool add_screen_dmabuf(std::size_t surface, std::size_t index)
    {
        Screen& s = screens_[index];
        if (s.access != platform::FrameAccess::dmabuf || !s.dirty || !s.dmabuf || s.image ||
            !gfx_->accepts_dmabuf(surface)) {
            return false;
        }
        const auto [width, height] = gfx_->surface_size(surface);
        const platform::Dmabuf& dmabuf = *s.dmabuf;
        if (dmabuf.width != width || dmabuf.height != height) {
            return false;
        }
        const auto damage = take_damage(s, width, height);
        const auto sent = gfx_->add_dmabuf_frame(surface, to_dmabuf_frame(dmabuf), damage);
        if (!sent) {
            // Errc::unsupported: this frame goes the CPU way; after a few in a
            // row, every frame does.
            log::debug(log_component, "{}: the encoder refused a dmabuf: {}", peer_, sent.error().message());
            s.damage_full = true;
            if (++dmabuf_refusals_ >= max_dmabuf_refusals) {
                dmabuf_refused_ = true;
                choose_frame_access();
            }
            return false;
        }
        dmabuf_refusals_ = 0;
        s.dirty = false;
        if (*sent && !s.logged_dmabuf_frame) {
            s.logged_dmabuf_frame = true;
            log::info(log_component, "{}: first frame from a dmabuf: {}x{}, format {:#010x}, modifier {:#x}, {} planes",
                      peer_, dmabuf.width, dmabuf.height, dmabuf.drm_format, dmabuf.modifier, dmabuf.plane_count);
        }
        return true;
    }

    /// Sends a cursor change, or keeps it until the connection is active.
    /// Each screen reports the cursor in its own pixels; it goes to the
    /// client from the screen that shows it, mapped onto that screen's place.
    void on_cursor(std::size_t index, platform::CursorUpdate update)
    {
        const bool laid_out = output_.width != 0;
        const auto* placement = placement_of(index);
        if (laid_out && placement == nullptr) {
            return;  // a screen that shows nowhere
        }
        if (update.visible) {
            cursor_screen_ = index;
        } else if (cursor_screen_ && *cursor_screen_ != index) {
            return;  // hidden here because it is on another screen
        }
        if (placement != nullptr && update.position) {
            const auto& t = placement->target;
            const auto [x, y] = *update.position;
            update.position = std::pair{
                static_cast<std::int32_t>(t.x + ((std::int64_t{x} * t.width) / std::max(placement->width, 1U))),
                static_cast<std::int32_t>(t.y + ((std::int64_t{y} * t.height) / std::max(placement->height, 1U)))};
        }
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

    [[nodiscard]] bool has_picture() const { return desktop_ != nullptr || !patterns_.empty(); }

    /// Screen `index`'s newest frame when it changed, read now if it came as
    /// a dmabuf.
    [[nodiscard]] std::optional<codec::ImageView> screen_image(std::size_t index)
    {
        Screen& s = screens_[index];
        if (!s.dirty) {
            return std::nullopt;
        }
        if (!s.image && s.dmabuf) {
            s.image = desktop_->screen_frames(index).map_frame();
            if (!s.image) {
                s.dirty = false;  // lost; the next frame brings everything the diff finds
                return std::nullopt;
            }
        }
        if (!s.image) {
            return std::nullopt;
        }
        latency_.frame_read(Clock::now());
        s.dirty = false;
        s.damage.clear();  // the CPU paths diff the pixels themselves
        s.damage_full = false;
        return s.image;
    }

    /// The whole desktop for bitmap updates when anything changed: the
    /// screens composed onto the layout.
    [[nodiscard]] std::optional<codec::ImageView> next_desktop_picture()
    {
        if (encoder_->width() != output_.width || encoder_->height() != output_.height) {
            return std::nullopt;  // a reactivation to the new size is on its way
        }
        std::vector<std::optional<codec::ImageView>> pictures(screen_count());
        bool any = false;
        for (const auto& placement : output_.screens) {
            if (desktop_ == nullptr) {
                pictures[placement.screen] = patterns_[placement.screen].render(frame_);
            } else {
                pictures[placement.screen] = screen_image(placement.screen);
            }
            any = any || pictures[placement.screen].has_value();
        }
        ++frame_;
        if (!any) {
            return std::nullopt;
        }
        return compositor_.compose(output_, pictures);
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

    [[nodiscard]] bool gfx_ready() const { return gfx_ && gfx_->ready() && scheduler_; }

    /// Opens the touch and pen input channel ([MS-RDPEI]); clients without
    /// a digitizer refuse it.
    void start_touch()
    {
        if (!touch_) {
            touch_.emplace(*dvc_);
        }
    }

    /// Injects touch and pen frames into the desktop, or shows them on the
    /// test pattern.
    void poll_touch()
    {
        bool closed = false;
        while (auto event = touch_->poll_event()) {
            std::visit(
                [this, &closed](const auto& e) {
                    using T = std::decay_t<decltype(e)>;
                    if constexpr (std::is_same_v<T, channels::rdpei::event::Ready>) {
                        log::info(log_component, "{}: touch input ready (RDPEI version {:#010x}, {} contacts{})", peer_,
                                  e.client.protocol_version, e.max_touch_contacts, e.pen ? ", pen" : "");
                    } else if constexpr (std::is_same_v<T, channels::rdpei::event::Frame>) {
                        note_input();
                        if (translator_) {
                            translator_->translate(e.contacts);
                            return;
                        }
                        for (const auto& contact : e.contacts) {
                            apply_to_patterns(contact);
                            log::debug(log_component, "{}: input {}", peer_, server::TestPattern::describe(contact));
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

    /// Opens the Graphics Pipeline when the client can run it.
    void start_graphics()
    {
        const auto& session = connection_->session();
        if (gfx_ || !session.supports_gfx() || output_.width == 0) {
            return;
        }
        channels::rdpgfx::GfxServerConfig config;
        const bool avc444 = options_.gfx_codec == server::TileCodec::avc444;
        const bool progressive = options_.gfx_codec == server::TileCodec::progressive;
        // AVC444 falls back to AVC420 for clients without it (8.1). A
        // Progressive surface takes AVC420 too, for the tiles that keep
        // changing (server::PipelineOptions::video_regions).
        config.avc420 =
            options_.gfx_codec == server::TileCodec::avc420 || avc444 || (progressive && options_.video_regions);
        config.avc444 = config.avc444v2 = avc444;
        server::H264Factory make_h264;
        if (config.avc420) {
            video::BackendOptions backend_options{.openh264_library = options_.openh264_library,
                                                  .render_node = options_.render_node};
            make_h264 = [backend_options, forced = options_.h264_backend,
                         fps = options_.frames_per_second](const video::EncoderConfig& requested) {
                auto encoder_config = requested;
                encoder_config.fps = fps;
                if (forced) {
                    return video::create_encoder(std::span(&*forced, 1), encoder_config, backend_options);
                }
                return video::create_encoder(video::compiled_backends(), encoder_config, backend_options);
            };
        }
        gfx_.emplace(*dvc_, output_, config, options_.gfx_codec, std::move(make_h264),
                     server::PipelineOptions{.clearcodec = options_.clearcodec,
                                             .refine = options_.refine,
                                             .video_regions = options_.video_regions,
                                             .lossless_still = options_.lossless_still});
        // The ladder starts where connect-time auto-detect says the link
        // is, not at the best tier: the first frame is the whole screen and
        // is the one a person watches arrive. Its encoder settings go in
        // before the encoders exist.
        server::QualityController::Config quality;
        quality.width = output_.width;
        quality.height = output_.height;
        quality.fps = options_.frames_per_second;
        // AVC444 puts two H.264 pictures on the wire for every frame, so the
        // encoder's per-second cap has to be half the session's budget.
        quality.pictures_per_frame = avc444 ? 2U : 1U;
        quality.min_bitrate_kbps = options_.h264_min_bitrate_kbps;
        quality.max_bitrate_kbps = options_.h264_max_bitrate_kbps;
        quality.target_bitrate_kbps = options_.h264_bitrate_kbps;
        quality_.emplace(quality, connection_->network().bandwidth_kbps);
        apply_tier(quality_->tier());
    }

    /// Opens the Display Control channel, over which the client resizes the
    /// desktop and changes its monitors.
    void start_display_control()
    {
        if (display_ || !layout_) {
            return;
        }
        server::DisplayControl::Config config;
        config.limits = display_limits_;
        display_.emplace(*dvc_, *layout_, config);
    }

    /// Applies the layout the client asked for, once it settled.
    void service_display()
    {
        if (!display_ || !active()) {
            return;
        }
        if (auto layout = display_->poll_layout(Clock::now())) {
            client_layout_ = true;
            apply_layout(std::move(*layout));
        }
    }

    /// A new layout. GFX lays out its surfaces again; bitmap updates take the
    /// desktop size from the capability exchange, so the connection is
    /// reactivated when that changes ([MS-RDPBCGR] 1.3.1.3).
    void apply_layout(server::DisplayLayout layout)
    {
        layout_ = std::move(layout);
        if (display_) {
            display_->set_current(*layout_);
        }
        const auto& session = connection_->session();
        if (!gfx_ && (layout_->width() != session.desktop_width || layout_->height() != session.desktop_height)) {
            log::info(log_component, "{}: reactivating at {}x{}", peer_, layout_->width(), layout_->height());
            connection_->reactivate(static_cast<std::uint16_t>(layout_->width()),
                                    static_cast<std::uint16_t>(layout_->height()));
            pump();
            return;
        }
        show_layout();
    }

    /// Shows layout_: resizable screens follow the client's monitors, test
    /// patterns take their sizes, and the output is laid out again.
    void show_layout()
    {
        if (!layout_) {
            return;
        }
        const std::size_t count =
            desktop_ != nullptr && !desktop_->screens_follow_monitors() ? screens_.size() : layout_->monitors().size();
        std::vector<Size> wanted;
        for (const std::size_t monitor : layout_->screen_monitors(count)) {
            const auto& rect = layout_->monitors()[monitor].rect;
            wanted.emplace_back(rect.width, rect.height);
        }
        if (desktop_ != nullptr) {
            if (desktop_->resizable() && client_layout_) {
                desktop_->request_screen_sizes(wanted);
                sync_screen_count();
            }
        } else {
            std::vector<server::TestPattern> patterns;
            for (std::size_t i = 0; i < wanted.size(); ++i) {
                if (i < patterns_.size()) {
                    patterns_[i].resize(wanted[i].first, wanted[i].second);
                    patterns.push_back(std::move(patterns_[i]));
                } else {
                    patterns.emplace_back(wanted[i].first, wanted[i].second);
                }
            }
            patterns_ = std::move(patterns);
        }
        update_output();
    }

    /// Places the screens on layout_ and, when that changed, lays out the
    /// output and points input at the new places.
    void update_output()
    {
        if (!layout_) {
            return;
        }
        auto output = layout_->place(screen_sizes());
        if (!client_layout_) {
            // The screens side by side in one client window: one monitor.
            output.monitors = server::DisplayLayout::single(output.width, output.height).gfx_monitors();
        }
        if (output == output_) {
            return;
        }
        output_ = std::move(output);
        for (const auto& p : output_.screens) {
            log::info(log_component, "{}: screen {} ({}x{}) shows at {}x{}+{}+{}{}", peer_, p.screen, p.width, p.height,
                      p.target.width, p.target.height, p.target.x, p.target.y, p.scaled() ? ", scaled" : "");
        }
        if (gfx_) {
            gfx_->set_layout(output_);
        }
        set_input_targets();
        if (encoder_) {
            encoder_->invalidate_all();
        }
        redraw_desktop();
        choose_frame_access();
    }

    /// Absolute pointer positions come in client desktop pixels. A desktop
    /// that knows where its screens show takes them as they are; otherwise
    /// they are mapped onto the first screen.
    void set_input_targets()
    {
        if (!translator_ || desktop_ == nullptr) {
            return;
        }
        std::vector<std::optional<platform::Rect>> targets(screens_.size());
        for (const auto& p : output_.screens) {
            targets[p.screen] =
                platform::Rect{static_cast<std::int32_t>(p.target.x), static_cast<std::int32_t>(p.target.y),
                               static_cast<std::int32_t>(p.target.width), static_cast<std::int32_t>(p.target.height)};
        }
        if (desktop_->set_screen_targets(targets)) {
            translator_->set_geometry(0, 0, 0, 0);  // positions pass through
            return;
        }
        const auto* first = placement_of(0);
        if (first == nullptr) {
            translator_->set_geometry(0, 0, 0, 0);
            return;
        }
        // The linear map that takes the first screen's target onto its pixels.
        const auto& t = first->target;
        const double sx = static_cast<double>(first->width) / t.width;
        const double sy = static_cast<double>(first->height) / t.height;
        translator_->set_geometry(
            output_.width, output_.height, static_cast<std::uint32_t>(std::lround(output_.width * sx)),
            static_cast<std::uint32_t>(std::lround(output_.height * sy)),
            -static_cast<std::int32_t>(std::lround(t.x * sx)), -static_cast<std::int32_t>(std::lround(t.y * sy)));
    }

    /// Frame rate and encoder settings of a quality tier.
    void apply_tier(const server::QualityTier& tier)
    {
        frame_interval_ = interval_for(tier.fps);
        if (scheduler_) {
            scheduler_->set_max_fps(tier.fps * 2);  // the frame clock sets the pace, see poll_pipeline()
        }
        if (gfx_) {
            gfx_->set_quality(tier.progressive_quant, tier.h264, tier.defer_chroma);
        }
        // What the encoder was actually told, so a configured bitrate can be
        // seen to have taken rather than inferred from the output.
        log::info(log_component, "{}: tier {} ({}), {} fps, H.264 {}", peer_, tier.level, tier.name, tier.fps,
                  tier.h264.mode == video::RateControl::Mode::bitrate
                      ? std::format("{} kbit/s average, cap {}", tier.h264.bitrate_kbps, tier.h264.max_bitrate_kbps)
                      : std::format("quality {}, cap {} kbit/s", tier.h264.quality, tier.h264.max_bitrate_kbps));
    }

    /// Feeds the quality controller and applies the tier it picks.
    void update_quality(Clock::time_point now)
    {
        if (!quality_ || !scheduler_) {
            return;
        }
        const server::QualityController::Observation observation{
            .network = connection_->network(),
            .frames_in_flight = scheduler_->frames_in_flight(),
            .max_frames_in_flight = max_frames_in_flight,
            .queue_depth = queue_depth_,
            .ack_round_trip = scheduler_->round_trip(),
            .bytes_sent = bytes_sent_,
        };
        if (auto change = quality_->update(observation, now)) {
            const auto& tier = change->tier;
            log::info(log_component, "{}: quality tier {} ({}): {} fps, H.264 CRF {} capped at {} kbit/s; {}", peer_,
                      tier.level, tier.name, tier.fps, tier.h264.quality, tier.h264.max_bitrate_kbps, change->reason);
            apply_tier(tier);
        }
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
                        const unsigned fps = quality_ ? quality_->tier().fps : options_.frames_per_second;
                        scheduler_.emplace(server::FrameScheduler::Config{
                            .max_fps = fps * 2,
                            .max_frames_in_flight = max_frames_in_flight,
                            .ack_timeout = std::chrono::seconds(1),
                            .acknowledgements = true,
                        });
                        scheduler_->damage();
                        next_frame_ = Clock::now();
                        if (desktop_ != nullptr) {
                            redraw_desktop();  // the new surfaces are empty
                        }
                        log::info(log_component, "{}: graphics pipeline active, {}", peer_, [&] {
                            switch (gfx_->codec()) {
                            case server::TileCodec::avc420:
                                return "H.264 (AVC420)";
                            case server::TileCodec::avc444:
                                return "H.264 4:4:4 (AVC444)";
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
                                const auto acknowledged = Clock::now();
                                scheduler_->frame_acknowledged(e.frame_id, acknowledged);
                                latency_.frame_acknowledged(e.frame_id, acknowledged);
                                queue_depth_ = e.queue_depth;
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
            quality_.reset();
            frame_interval_ = interval_for(options_.frames_per_second);
            if (encoder_) {
                encoder_->invalidate_all();
            }
            redraw_desktop();
            if (layout_) {
                apply_layout(*layout_);  // bitmap updates may need another desktop size
            }
        }
        choose_frame_access();
    }

    /// Every few seconds: frame rate, acknowledgements and round trip, what
    /// auto-detect measured and the quality tier.
    /// Where the time went, once, when the session ends. It answers the
    /// M4 exit criterion as far as a server can (docs/ROADMAP.md), so it is
    /// worth a few lines of log rather than a number nobody can break down.
    void report_latency()
    {
        const auto summary = latency_.summary();
        if (summary.frames == 0) {
            return;
        }
        log::info(log_component, "{}: latency, {}", peer_, server::LatencyTracker::describe(summary));
    }

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
        const auto& network = connection_->network();
        std::string link;
        if (connection_->session().autodetect) {
            link = std::format(", RTT {} (base {}, jitter {}), bandwidth {}", milliseconds(network.rtt),
                               milliseconds(network.base_rtt), milliseconds(network.jitter),
                               network.bandwidth_kbps ? std::format("{} kbit/s", *network.bandwidth_kbps)
                                                      : std::string("unknown"));
        }
        const auto latency = latency_.summary();
        const auto server_ms = std::chrono::duration<double, std::milli>(latency.server_p95).count();
        log::info(log_component,
                  "{}: GFX {:.1f} fps, {} in flight, round trip {}{}, sending {} kbit/s, tier {}, server {:.1f} ms p95",
                  peer_, static_cast<double>(gfx_frames_) / seconds, scheduler_->frames_in_flight(),
                  milliseconds(scheduler_->round_trip()), link, quality_ ? quality_->send_kbps() : 0,
                  quality_ ? std::format("{} ({})", quality_->tier().level, quality_->tier().name) : "none", server_ms);
        gfx_frames_ = 0;
        gfx_statistics_since_ = now;
    }

    /// Screen `surface`'s picture into the open GFX frame, if it changed.
    void add_screen_to_frame(std::size_t surface)
    {
        const auto& placement = gfx_->layout().screens[surface];
        const auto [width, height] = gfx_->surface_size(surface);
        if (desktop_ == nullptr) {
            if (placement.screen < patterns_.size()) {
                const auto image = patterns_[placement.screen].render(frame_);
                if (image.width == width && image.height == height) {
                    gfx_->add_frame(surface, image);
                }
            }
            return;
        }
        if (add_screen_dmabuf(surface, placement.screen)) {
            return;
        }
        const auto image = screen_image(placement.screen);
        if (!image || image->width != placement.width || image->height != placement.height) {
            return;  // unchanged, or of a size a new layout is on its way for
        }
        gfx_->add_frame(surface, screens_[placement.screen].scaled.scale(*image, width, height));
    }

    void send_gfx_frame_if_due()
    {
        const auto now = Clock::now();
        update_quality(now);
        if (suppressed_ || !has_picture()) {
            return;
        }
        if (now < next_frame_) {
            return;
        }
        next_frame_ += frame_interval_;
        if (next_frame_ < now) {
            next_frame_ = now + frame_interval_;
        }
        if (desktop_ == nullptr || gfx_->has_pending_refinement()) {
            // The test pattern moves in every frame; held-back AVC444
            // chroma and Progressive refinement go out even when nothing
            // changed.
            scheduler_->damage();
        }
        if (!scheduler_->due(now)) {
            return;  // the client is behind, or nothing changed; the next frame covers everything
        }
        gfx_->begin_frame();
        for (std::size_t surface = 0; surface < gfx_->screen_count() && gfx_->ready(); ++surface) {
            add_screen_to_frame(surface);
        }
        ++frame_;
        if (const auto frame_id = gfx_->end_frame()) {
            const auto sent = Clock::now();
            latency_.frame_encoded(sent);
            latency_.frame_sent(*frame_id, sent);
            scheduler_->frame_sent(*frame_id, now);
            ++gfx_frames_;
            count_frame();
        }
        log_gfx_statistics(now);
        pump();
        poll_pipeline();  // an encoder failure closes the pipeline
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
        const auto codec = session.bits_per_pixel == 32 ? options_.codec : server::BitmapCodec::uncompressed;
        if (!layout_ || layout_->width() != session.desktop_width || layout_->height() != session.desktop_height) {
            // The connection chose the size (connect_layout_), or clamped it.
            layout_ = connect_layout_ && connect_layout_->width() == session.desktop_width &&
                              connect_layout_->height() == session.desktop_height
                          ? *connect_layout_
                          : server::DisplayLayout::single(session.desktop_width, session.desktop_height);
        }
        encoder_.emplace(session.desktop_width, session.desktop_height, session.bits_per_pixel, codec,
                         session.no_bitmap_compression_header);
        if (desktop_ != nullptr) {
            start_desktop_session(session);
        }
        show_layout();
        next_frame_ = Clock::now();
        log::info(log_component, "{}: {} {}x{} in {} monitor{}, {} bitmaps", peer_,
                  e.reactivation ? "reactivated" : "active", session.desktop_width, session.desktop_height,
                  layout_->monitors().size(), layout_->monitors().size() == 1 ? "" : "s",
                  codec == server::BitmapCodec::planar ? "planar" : "uncompressed");
        // A client that advertised GFX gets a moment to open it before the
        // bitmap path floods the link (gfx_grace).
        if (!gfx_ && session.supports_gfx()) {
            gfx_wait_until_ = Clock::now() + gfx_grace;
        }
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
    /// and the desktop has a clipboard. The test pattern has a loopback one.
    void start_clipboard()
    {
        const auto id = connection_->session().static_channel_id(channels::cliprdr::channel_name);
        if (clipboard_ || !id || !options_.clipboard) {
            return;
        }
        clipboard_source_ = desktop_ != nullptr ? desktop_->clipboard() : &loopback_clipboard_.emplace();
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

    /// On (re)activation with a shared desktop: the cursor (re)starts and
    /// input goes to the desktop.
    void start_desktop_session(const server::Session& session)
    {
        redraw_desktop();
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
    }

    void on_event(server::event::Input& e)
    {
        note_input();
        if (translator_) {
            translator_->translate(e.events);
            return;
        }
        for (const auto& input : e.events) {
            apply_to_patterns(input);
            log::debug(log_component, "{}: input {}", peer_, server::TestPattern::describe(input));
        }
    }

    /// Pointer events go to the test pattern under the pointer, in its
    /// pixels; keys go to all of them.
    void apply_to_patterns(const proto::InputEvent& input)
    {
        const auto* mouse = std::get_if<proto::MouseEvent>(&input);
        if (mouse == nullptr) {
            for (auto& pattern : patterns_) {
                pattern.apply(input);
            }
            return;
        }
        for (const auto& p : output_.screens) {
            const auto& t = p.target;
            if (mouse->x < t.x || mouse->x >= t.x + t.width || mouse->y < t.y || mouse->y >= t.y + t.height ||
                p.screen >= patterns_.size()) {
                continue;
            }
            auto local = *mouse;
            local.x = static_cast<std::uint16_t>(((mouse->x - t.x) * std::uint64_t{p.width}) / t.width);
            local.y = static_cast<std::uint16_t>(((mouse->y - t.y) * std::uint64_t{p.height}) / t.height);
            patterns_[p.screen].apply(proto::InputEvent{local});
            return;
        }
    }

    /// Touch and pen contacts go to the test pattern under them, in its
    /// pixels; the other patterns drop the contact if they had it.
    void apply_to_patterns(const channels::rdpei::Contact& contact)
    {
        std::size_t hit = patterns_.size();
        auto local = contact;
        for (const auto& p : output_.screens) {
            const auto& t = p.target;
            const std::int64_t x = contact.x;
            const std::int64_t y = contact.y;
            if (x < t.x || x >= std::int64_t{t.x} + t.width || y < t.y || y >= std::int64_t{t.y} + t.height ||
                p.screen >= patterns_.size()) {
                continue;
            }
            local.x = static_cast<decltype(local.x)>(((x - t.x) * std::int64_t{p.width}) / t.width);
            local.y = static_cast<decltype(local.y)>(((y - t.y) * std::int64_t{p.height}) / t.height);
            hit = p.screen;
            break;
        }
        auto gone = contact;
        gone.action = channels::rdpei::ContactAction::up;
        for (std::size_t i = 0; i < patterns_.size(); ++i) {
            patterns_[i].apply(i == hit ? local : gone);
        }
    }

    void on_event(server::event::RefreshRequested& e)
    {
        if (gfx_ready()) {
            gfx_->invalidate_all();
            redraw_desktop();
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
            redraw_desktop();
        }
        if (!suppressed_ && encoder_) {
            encoder_->invalidate_all();
        }
    }

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

    void send_frame_if_due()
    {
        if (gfx_ready()) {
            gfx_wait_until_.reset();
            send_gfx_frame_if_due();
            return;
        }
        if (!active() || suppressed_ || !encoder_ || !has_picture() || output_.width == 0) {
            return;
        }
        const auto now = Clock::now();
        if (gfx_wait_until_) {
            if (now < *gfx_wait_until_) {
                return;  // the Graphics Pipeline is still opening; do not flood the link
            }
            gfx_wait_until_.reset();
            log::info(log_component, "{}: the client did not open the graphics pipeline within {} s; painting with "
                                     "bitmap updates",
                      peer_, gfx_grace.count());
        }
        if (now < next_frame_) {
            return;
        }
        if (const auto image = next_desktop_picture()) {
            for (const auto& update : encoder_->encode(*image, connection_->max_update_size())) {
                connection_->send_bitmap_update(update);
            }
            count_frame();
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
    std::optional<server::GraphicsPipeline> gfx_;                  // after dvc_, which it uses
    /// Set while a GFX-capable client is still opening the pipeline: until it
    /// passes, the bitmap path stays quiet so the negotiation is not stuck
    /// behind a full-desktop update (gfx_grace).
    std::optional<Clock::time_point> gfx_wait_until_;
    std::optional<server::TouchInput> touch_;                      // after dvc_, which it uses
    std::optional<server::LoopbackClipboard> loopback_clipboard_;  // before clipboard_, which uses it
    platform::Clipboard* clipboard_source_ = nullptr;
    std::optional<server::ClipboardBridge> clipboard_;
    std::uint16_t clipboard_channel_ = 0;
    std::optional<server::FrameScheduler> scheduler_;
    /// Where the time goes from the compositor to the client's
    /// acknowledgement (docs/ROADMAP.md M4). Costs one timestamp per stage
    /// and nothing else; reported when the session ends.
    server::LatencyTracker latency_;
    std::optional<server::QualityController> quality_;
    std::uint32_t queue_depth_ = 0;  ///< from the client's last frame acknowledgement
    std::uint64_t bytes_sent_ = 0;
    std::uint64_t gfx_frames_ = 0;
    Clock::time_point gfx_statistics_since_;
    bool running_ = true;
    bool suppressed_ = false;
    /// One per client monitor when there is no shared desktop.
    std::vector<server::TestPattern> patterns_;
    std::vector<Screen> screens_;  ///< the shared desktop's
    bool screens_resized_ = false;
    unsigned dmabuf_refusals_ = 0;  ///< refused by the encoder in a row
    bool dmabuf_refused_ = false;   ///< too many: frames are read into CPU memory
    std::optional<server::CursorEncoder> cursor_;
    std::optional<platform::CursorUpdate> pending_cursor_;
    /// The screen that shows the cursor.
    std::optional<std::size_t> cursor_screen_;
    std::optional<platform::InputTranslator> translator_;
    std::optional<server::FrameEncoder> encoder_;
    std::uint64_t frame_ = 0;
    Clock::time_point next_frame_;
    server::DisplayLimits display_limits_;
    /// What the connection's desktop size was chosen from.
    std::optional<server::DisplayLayout> connect_layout_;
    /// The monitors the screens are placed on: the client's, or (while
    /// client_layout_ is false) the shared desktop's screens side by side.
    std::optional<server::DisplayLayout> layout_;
    bool client_layout_ = true;
    /// Where the screens show; what GFX lays out and bitmap updates compose.
    server::OutputLayout output_;
    server::Compositor compositor_;
    std::optional<server::DisplayControl> display_;  // after dvc_, which it uses
    std::optional<SessionCamera> camera_;            // uses dvc_, so before it
    std::optional<SessionAudio> audio_;              // last: it uses dvc_
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
