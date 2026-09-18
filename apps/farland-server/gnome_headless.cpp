// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "gnome_headless.hpp"

#include <farland/base/log.hpp>
#include <farland/platform/logind/seat.hpp>
#include <farland/platform/mutter/headless_shell.hpp>
#include <farland/platform/mutter/mutter_clipboard.hpp>
#include <farland/platform/mutter/mutter_session.hpp>
#include <farland/platform/portal/ei_input.hpp>
#include <farland/platform/portal/pipewire_capture.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <poll.h>
#include <thread>

namespace farland::app {

namespace {

namespace logind = platform::logind;
namespace mutter = platform::mutter;
namespace portal = platform::portal;
using Clock = std::chrono::steady_clock;
using Size = std::pair<std::uint32_t, std::uint32_t>;
constexpr std::string_view log_component = "app.gnome";
/// RDP clients show at most 16 monitors ([MS-RDPBCGR] 2.2.1.3.6).
constexpr std::size_t max_screens = 16;

class GnomeHeadlessDesktop final : public Desktop {
public:
    GnomeHeadlessDesktop() = default;
    GnomeHeadlessDesktop(const GnomeHeadlessDesktop&) = delete;
    GnomeHeadlessDesktop& operator=(const GnomeHeadlessDesktop&) = delete;
    GnomeHeadlessDesktop(GnomeHeadlessDesktop&&) = delete;
    GnomeHeadlessDesktop& operator=(GnomeHeadlessDesktop&&) = delete;
    ~GnomeHeadlessDesktop() override
    {
        if (session_) {
            attach_back();
            log::info(log_component, "ending the Mutter remote desktop session");
        }
    }

    [[nodiscard]] Result<void> start(const HeadlessOptions& options);

    [[nodiscard]] platform::FrameSource& frames() override { return screens_.front().capture->frames(); }
    [[nodiscard]] platform::CursorSource* cursor() override { return &screens_.front().capture->cursor(); }
    [[nodiscard]] platform::InputSink& input() override { return *ei_; }
    [[nodiscard]] std::vector<int> dispatch_fds() const override
    {
        std::vector<int> fds{session_->fd(), ei_->fd()};
        if (seat_watch_) {
            fds.push_back(seat_watch_->fd());
        }
        // The next screen to come wakes the session with its first frame.
        if (!pending_.empty() && pending_.front().capture) {
            fds.push_back(pending_.front().capture->frames().wake_fd());
        }
        return fds;
    }
    void dispatch() override
    {
        session_->process();
        ei_->dispatch();
        if (seat_watch_) {
            seat_watch_->process();
        }
        service_pending();
    }
    [[nodiscard]] bool closed() const override
    {
        // A session that keeps being drawn off its seat lets the seat show a
        // login screen while a client holds it, so losing the seat is what
        // this connection asked for; it ends when somebody at the machine
        // logs in and the session goes back to the seat. A session that is
        // drawn only on its seat ends the moment it loses it.
        const char* reason = nullptr;
        if (session_->closed()) {
            reason = "Mutter closed the remote desktop session";
        } else if (seat_watch_ && keeps_rendering() && seat_watch_->returned_to_the_seat()) {
            reason = "somebody logged in at the machine and took the session back";
        } else if (seat_watch_ && !keeps_rendering() && seat_watch_->left_the_seat()) {
            reason = "something else took the seat the session is on";
        } else if (screens_.empty()) {
            reason = "the session has no screens left";
        } else if (std::ranges::any_of(screens_, [](const Screen& s) { return s.capture->closed(); })) {
            reason = "a screen's stream closed";
        } else if (ei_->closed()) {
            reason = "the input connection closed";
        }
        if (reason != nullptr && !said_why_closed_) {
            said_why_closed_ = true;
            log::info(log_component, "the desktop ends: {}", reason);
        }
        return reason != nullptr;
    }
    [[nodiscard]] platform::Clipboard* clipboard() override { return clipboard_.get(); }

    [[nodiscard]] std::size_t screen_count() const override { return screens_.size(); }
    [[nodiscard]] platform::FrameSource& screen_frames(std::size_t index) override
    {
        return screens_.at(index).capture->frames();
    }
    [[nodiscard]] platform::CursorSource* screen_cursor(std::size_t index) override
    {
        return &screens_.at(index).capture->cursor();
    }
    [[nodiscard]] bool resizable() const override { return true; }
    [[nodiscard]] bool screens_follow_monitors() const override { return true; }
    void request_screen_sizes(std::span<const Size> sizes) override;
    bool set_screen_targets(std::span<const std::optional<platform::Rect>> targets) override;
    void set_held(bool held) override;
    [[nodiscard]] bool keep_when_released() const override { return !options_.attach; }

private:
    /// A virtual monitor: its stream, the size asked for, and the capture
    /// once Mutter announced the PipeWire node.
    struct Screen {
        mutter::StreamId stream = 0;
        std::string mapping_id;
        Size size{0, 0};
        std::unique_ptr<portal::PipeWireCapture> capture;
    };

    [[nodiscard]] Result<void> connect(const HeadlessOptions& options);
    /// Makes the session one this client can see: on its seat, and watched.
    [[nodiscard]] Result<void> take_the_session();
    [[nodiscard]] bool add_screen(Size size);
    /// Connects the captures of new streams and moves screens whose first
    /// frame came to screens_, in the order they were added.
    void service_pending();
    [[nodiscard]] Result<void> wait_for_first_screen();
    void start_input_and_keymap();
    void apply_targets();
    /// Takes the session over: it shows exactly our virtual monitors, in the
    /// client's order, with the client's first monitor primary.
    void ensure_monitor_layout();
    /// True while this desktop is attached to a session on a seat that
    /// Mutter keeps drawing off that seat (ScreenCast 5): the seat can show
    /// a login screen while a client holds the session.
    [[nodiscard]] bool keeps_rendering() const
    {
        return options_.attach && session_ && session_->capabilities().keep_rendering;
    }
    /// Hands the seat a GDM login screen while a client holds the session,
    /// so that the screen at the machine shows neither the session nor what
    /// was last on it, and whoever is there takes it back by logging in.
    void hand_the_seat_a_greeter();
    /// Gives the session back to what was attached before (the seat's
    /// screen), before our monitors go with us.
    void attach_back();

    HeadlessOptions options_;
    // Destroyed in reverse order: the clipboard and input before the
    // captures, those before the session, and the session before the shell.
    std::unique_ptr<mutter::HeadlessShell> shell_;
    std::unique_ptr<mutter::MutterSession> session_;
    std::vector<Screen> screens_;
    /// Recorded, but without a first frame yet; they come after screens_.
    std::vector<Screen> pending_;
    std::unique_ptr<portal::EiInput> ei_;
    std::unique_ptr<mutter::MutterClipboard> clipboard_;
    std::vector<std::optional<platform::Rect>> targets_;
    /// The screens ensure_monitor_layout() last laid the session out for.
    std::string monitor_layout_;
    /// Set while this connection took the session from a seat: it ends when
    /// the seat takes it back.
    std::unique_ptr<logind::SeatWatch> seat_watch_;
    /// A client holds the desktop (set_held()); until the first one does,
    /// the session stays where it is.
    bool held_ = false;
    /// closed() said why, once.
    mutable bool said_why_closed_ = false;
    /// The monitors that were attached before we took the session over (the
    /// seat's screen), to put back when this desktop goes.
    std::vector<mutter::LogicalMonitor> detached_;
    bool warned_monitor_layout_ = false;
};

Result<void> GnomeHeadlessDesktop::start(const HeadlessOptions& options)
{
    options_ = options;
    FARLAND_TRY_VOID(connect(options));
    if (options.attach) {
        FARLAND_TRY_VOID(take_the_session());
    }
    if (!add_screen({options.width, options.height})) {
        return fail(Errc::io, "Mutter did not create a virtual monitor");
    }
    if (auto started = session_->start(); !started) {
        log::error(log_component, "{}", started.error().message);
        return fail(Errc::io, "cannot start the Mutter remote desktop session");
    }
    FARLAND_TRY_VOID(wait_for_first_screen());

    auto eis = session_->connect_to_eis(mutter::device_keyboard | mutter::device_pointer | mutter::device_touchscreen);
    if (!eis) {
        log::error(log_component, "ConnectToEIS: {}", eis.error().message);
        return fail(Errc::io, "cannot connect to Mutter's input (libei)");
    }
    auto ei = portal::EiInput::connect_fd(eis->release());
    if (!ei) {
        log::error(log_component, "libei: {}", ei.error().message());
        return fail(Errc::io, "cannot connect to Mutter's input (libei)");
    }
    ei_ = std::move(*ei);
    start_input_and_keymap();

    auto clipboard = mutter::MutterClipboard::create(*session_);
    if (clipboard) {
        clipboard_ = std::move(*clipboard);
    } else {
        log::warn(log_component, "no clipboard: {}", clipboard.error().message);
    }

    // Until a session places the screens: side by side at their sizes.
    std::vector<std::optional<platform::Rect>> targets;
    std::int32_t x = 0;
    for (const auto& screen : screens_) {
        const auto [width, height] = screen.capture->frames().size();
        targets.emplace_back(platform::Rect{x, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)});
        x += static_cast<std::int32_t>(width);
    }
    static_cast<void>(set_screen_targets(targets));
    set_held(true);
    log::info(log_component, "headless GNOME desktop ready: {}x{}, input through libei{}", screens_.front().size.first,
              screens_.front().size.second, clipboard_ ? ", clipboard through Mutter" : "");
    return {};
}

Result<void> GnomeHeadlessDesktop::take_the_session()
{
    // Where Mutter draws the session only on its seat, a session someone
    // switched away from shows nothing: bring it to its seat first. Where
    // Mutter keeps our monitors going off the seat, the seat is free to show
    // something else, and the session is ours wherever it is.
    if (!keeps_rendering()) {
        if (auto active = logind::activate_user_session(); !active) {
            log::error(log_component, "{}", active.error().message);
            return fail(Errc::io,
                        "the GNOME session is not on its seat, so it draws nothing: switch to it on the machine first");
        }
    }
    // From here on, something else taking the seat ends this connection: the
    // session is attached to one place at a time.
    if (auto watch = logind::SeatWatch::create()) {
        seat_watch_ = std::move(*watch);
    } else {
        log::debug(log_component, "cannot watch the seat: {}", watch.error().message);
    }
    return {};
}

Result<void> GnomeHeadlessDesktop::connect(const HeadlessOptions& options)
{
    mutter::MutterOptions session_options;
    session_options.timeout = options.timeout;
    // A session of our own is drawn wherever it runs; one we attach to lives
    // on a seat, and only Mutter keeping it drawn off the seat lets the seat
    // show a login screen while a client holds it.
    session_options.keep_rendering_when_inactive = options.attach;
    if (!options.attach) {
        auto shell = mutter::HeadlessShell::launch({});
        if (!shell) {
            log::error(log_component, "{}", shell.error().message);
            return fail(Errc::io, "cannot start a headless GNOME Shell");
        }
        shell_ = std::move(*shell);
        session_options.bus_address = shell_->bus_address();
        session_options.keep_waiting = [this] { return shell_->running(); };
    }
    // The session bus can be restarting: GNOME restarts the user's D-Bus
    // when a session of theirs ends (gnome-session-restart-dbus), so a
    // desktop started right after one closed finds a connection that dies
    // under it. Keep trying until the timeout.
    const auto deadline = Clock::now() + options.timeout;
    bool waited = false;
    for (;;) {
        auto attempt = session_options;
        attempt.timeout = std::min<std::chrono::milliseconds>(
            std::chrono::seconds(5), std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
        auto session = mutter::MutterSession::create(attempt);
        if (session) {
            session_ = std::move(*session);
            return {};
        }
        if (Clock::now() >= deadline || (shell_ && !shell_->running())) {
            log::error(log_component, "{}", session.error().message);
            return fail(Errc::io, "Mutter's remote desktop API is not available");
        }
        if (!waited) {
            waited = true;
            log::info(log_component, "waiting for the GNOME session's bus: {}", session.error().message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

bool GnomeHeadlessDesktop::add_screen(Size size)
{
    auto id = session_->record_virtual();
    if (!id) {
        log::error(log_component, "RecordVirtual: {}", id.error().message);
        return false;
    }
    const auto* stream = session_->stream(*id);
    pending_.push_back(Screen{*id, stream != nullptr ? stream->mapping_id : std::string(), size, nullptr});
    service_pending();
    return true;
}

void GnomeHeadlessDesktop::service_pending()
{
    for (auto it = pending_.begin(); it != pending_.end();) {
        Screen& s = *it;
        if (!s.capture) {
            const auto* stream = session_->stream(s.stream);
            if (stream == nullptr || !stream->node_id) {
                ++it;
                continue;
            }
            portal::PipeWireCaptureOptions capture_options;
            capture_options.render_node = options_.render_node;
            capture_options.stream_name = std::format("farland-gnome-{}", s.stream);
            capture_options.size = s.size;
            auto capture = portal::PipeWireCapture::create(-1, *stream->node_id, capture_options);
            if (!capture) {
                log::error(log_component, "PipeWire capture of node {}: {}", *stream->node_id,
                           capture.error().message());
                session_->stop_stream(s.stream);
                it = pending_.erase(it);
                continue;
            }
            s.capture = std::move(*capture);
        }
        if (s.capture->closed()) {
            log::warn(log_component, "a new virtual monitor's stream closed: {}", s.capture->error());
            session_->stop_stream(s.stream);
            it = pending_.erase(it);
            continue;
        }
        ++it;
    }
    bool added = false;
    while (!pending_.empty() && pending_.front().capture && pending_.front().capture->frames().size().first != 0) {
        Screen& s = pending_.front();
        const auto [width, height] = s.capture->frames().size();
        log::info(log_component, "screen {}: a virtual monitor of {}x{} (PipeWire node {}, mapping {})",
                  screens_.size(), width, height, s.capture->node_id(), s.mapping_id);
        screens_.push_back(std::move(s));
        pending_.erase(pending_.begin());
        added = true;
    }
    if (added) {
        apply_targets();
    }
    ensure_monitor_layout();
}

void GnomeHeadlessDesktop::ensure_monitor_layout()
{
    if (screens_.empty() || session_->closed()) {
        return;
    }
    if (options_.attach && !held_) {
        return;  // nobody holds it: the session is the seat's, laid out for it
    }
    // A session is attached to one place at a time: ours are its monitors,
    // and whatever else was attached (the seat's screen, the monitors of an
    // earlier connection) is switched off, so that the panel, the dash and
    // the overview are where the client looks. Mutter lays the session out
    // again when our monitors go away, which brings the local screen back.
    std::string layout;
    for (const auto& screen : screens_) {
        const auto [width, height] = screen.capture->frames().size();
        layout += std::format("{}x{};", width, height);
    }
    if (layout == monitor_layout_) {
        return;
    }
    auto state = session_->monitors();
    if (!state) {
        if (!warned_monitor_layout_) {
            warned_monitor_layout_ = true;
            log::warn(log_component,
                      "cannot read the session's monitors ({}); the desktop may show the panel on "
                      "another monitor",
                      state.error().message);
        }
        return;
    }
    const auto ours = state->virtual_connectors();
    if (ours.size() != screens_.size()) {
        return;  // Mutter has not caught up with the monitors we asked for
    }
    std::int32_t x = 0;
    bool correct = true;
    for (const auto& monitor : state->monitors) {
        const auto found = std::ranges::find(ours, monitor.connector);
        if (found == ours.end()) {
            correct = correct && !monitor.active;  // something else, and off
            continue;
        }
        const bool first = found == ours.begin();
        correct = correct && monitor.active && monitor.primary == first;
    }
    for (const auto& connector : ours) {
        const auto found = std::ranges::find(state->monitors, connector, &mutter::Monitor::connector);
        if (found == state->monitors.end()) {
            return;
        }
        correct = correct && found->x == x && found->y == 0;
        x += static_cast<std::int32_t>(found->width);
    }
    if (correct) {
        monitor_layout_ = layout;
        return;
    }
    if (detached_.empty()) {
        // What the session showed before we took it over, to put back when
        // this desktop goes: our monitors disappear with it, and a session
        // that briefly has none at all can end.
        for (const auto& entry : state->active_layout()) {
            if (std::ranges::find(ours, entry.connector) == ours.end()) {
                detached_.push_back(entry);
            }
        }
    }
    if (auto applied = session_->set_monitors(state->serial, state->side_by_side(ours)); !applied) {
        if (!warned_monitor_layout_) {
            warned_monitor_layout_ = true;
            log::warn(log_component,
                      "cannot lay the session's monitors out ({}); the desktop may show the panel on "
                      "another monitor",
                      applied.error().message);
        }
        return;
    }
    monitor_layout_ = layout;
    const auto others = state->monitors.size() - ours.size();
    log::info(
        log_component, "the session shows {} monitor{} of this connection{}", ours.size(), ours.size() == 1 ? "" : "s",
        others == 0 ? "" : std::format(", and {} other monitor{} of it went off", others, others == 1 ? "" : "s"));
}

void GnomeHeadlessDesktop::set_held(bool held)
{
    if (held_ == held || !options_.attach || session_ == nullptr || session_->closed()) {
        held_ = held;
        return;
    }
    held_ = held;
    if (held) {
        // A client took the session again: it shows that client's monitors,
        // and the seat gets a login screen back.
        monitor_layout_.clear();
        ensure_monitor_layout();
        hand_the_seat_a_greeter();
        return;
    }
    // Nobody holds it any more. The session goes back to the screen at the
    // machine, so that whoever is there sees the session and not the last
    // picture a client left behind. Where the seat has a login screen, it
    // keeps it: logging in there brings the session back with its windows.
    attach_back();
    monitor_layout_.clear();
    detached_.clear();
    log::info(log_component, "no client holds the session: it is the seat's again");
}

void GnomeHeadlessDesktop::hand_the_seat_a_greeter()
{
    if (!keeps_rendering() || seat_watch_ == nullptr) {
        return;
    }
    if (!seat_watch_->active()) {
        // Something else is on the seat already (a login screen from an
        // earlier connection, or another session): nothing to hand over.
        log::info(log_component, "the seat is not showing this session, so it keeps what it has");
        return;
    }
    if (auto handed = logind::switch_seat_to_greeter(); !handed) {
        // The seat keeps showing the session; the client still has it.
        log::warn(log_component, "cannot put a login screen on the seat ({}); the screen at the machine keeps showing "
                                 "the session",
                  handed.error().message);
        return;
    }
    log::info(log_component, "the screen at the machine shows a login screen; logging in there takes the session back");
}

void GnomeHeadlessDesktop::attach_back()
{
    if (detached_.empty() || session_->closed()) {
        return;
    }
    if (seat_watch_ && !seat_watch_->active() && !keeps_rendering()) {
        // Something else is on the seat; its screen is not this session's
        // to switch on, and Mutter lays the session out itself when it
        // comes back. A session Mutter keeps drawing off its seat does need
        // its screen back now: it is what whoever logs in there will see.
        return;
    }
    auto state = session_->monitors();
    if (!state) {
        return;
    }
    // The monitors that were there before come back beside ours, not
    // instead of them: switching the seat's screens on and taking ours away
    // in one step makes Mutter 50 fall over its cursor plane (the session
    // dies with it). Ours go when this session stops, a moment later, and
    // Mutter lays the rest out by itself.
    std::vector<mutter::LogicalMonitor> layout = detached_;
    std::int32_t x = 0;
    for (const auto& entry : layout) {
        const auto found = std::ranges::find(state->monitors, entry.connector, &mutter::Monitor::connector);
        const auto width = found != state->monitors.end() ? static_cast<std::int32_t>(found->width) : 0;
        x = std::max(x, entry.x + width);
    }
    for (const auto& connector : state->virtual_connectors()) {
        const auto found = std::ranges::find(state->monitors, connector, &mutter::Monitor::connector);
        if (found == state->monitors.end()) {
            continue;
        }
        layout.push_back(mutter::LogicalMonitor{x, 0, 1.0, 0, false, found->connector, found->mode});
        x += static_cast<std::int32_t>(found->width);
    }
    if (auto applied = session_->set_monitors(state->serial, layout); !applied) {
        // The screen may be gone by now; Mutter lays the session out itself
        // once our monitors disappear.
        log::debug(log_component, "cannot attach the session back: {}", applied.error().message);
        return;
    }
    log::info(log_component, "the session goes back to the {} monitor{} it had before", detached_.size(),
              detached_.size() == 1 ? "" : "s");
}

Result<void> GnomeHeadlessDesktop::wait_for_first_screen()
{
    const auto deadline = Clock::now() + options_.timeout;
    while (screens_.empty()) {
        if (session_->closed()) {
            return fail(Errc::io, "the Mutter session closed before the first frame");
        }
        if (pending_.empty()) {
            return fail(Errc::io, "the virtual monitor's stream failed");
        }
        if (seat_watch_ && seat_watch_->left_the_seat()) {
            return fail(Errc::io, "the GNOME session left its seat, so it stopped drawing");
        }
        if (Clock::now() > deadline) {
            return fail(Errc::io, seat_watch_ && !seat_watch_->active()
                                      ? "no frame: the GNOME session is not on its seat, so GNOME does not draw it"
                                      : "no frame from the virtual monitor");
        }
        std::array<pollfd, 2> fds{{{session_->fd(), session_->events(), 0}, {-1, POLLIN, 0}}};
        if (pending_.front().capture) {
            fds[1].fd = pending_.front().capture->frames().wake_fd();
        }
        ::poll(fds.data(), fds.size(), 100);
        session_->process();
        service_pending();
    }
    return {};
}

void GnomeHeadlessDesktop::start_input_and_keymap()
{
    if (options_.keymap_layout.empty()) {
        return;
    }
    const auto keymap = mutter::xkb_keymap_for_layout(options_.keymap_layout);
    if (!keymap) {
        log::warn(log_component,
                  "cannot compile an XKB keymap for layout '{}' (unknown, or farland was built "
                  "without libxkbcommon); the desktop keeps its own",
                  options_.keymap_layout);
        return;
    }
    if (auto set = session_->set_keymap(*keymap); !set) {
        log::warn(log_component, "SetKeymap: {}; the desktop keeps its keymap", set.error().message);
        return;
    }
    log::info(log_component, "keyboard layout {}", options_.keymap_layout);
}

void GnomeHeadlessDesktop::request_screen_sizes(std::span<const Size> sizes)
{
    const std::size_t wanted = std::clamp<std::size_t>(sizes.size(), 1, max_screens);
    const auto size_at = [&](std::size_t i) -> Size {
        if (i < sizes.size() && sizes[i].first > 0 && sizes[i].second > 0) {
            return sizes[i];
        }
        return {options_.width, options_.height};
    };
    // Fewer monitors: the newest screens go, those still coming first.
    bool removed = false;
    while (screens_.size() + pending_.size() > wanted) {
        auto& from = pending_.empty() ? screens_ : pending_;
        session_->stop_stream(from.back().stream);
        from.pop_back();
        removed = true;
    }
    for (std::size_t i = 0; i < screens_.size() + pending_.size(); ++i) {
        Screen& s = i < screens_.size() ? screens_[i] : pending_[i - screens_.size()];
        const Size size = size_at(i);
        if (s.size != size) {
            s.size = size;
            if (s.capture) {
                s.capture->request_size(size.first, size.second);
            }
        }
    }
    // More: new virtual monitors, which show up in dispatch() once their
    // first frame came.
    while (screens_.size() + pending_.size() < wanted) {
        const std::size_t index = screens_.size() + pending_.size();
        if (!add_screen(size_at(index))) {
            break;
        }
        log::info(log_component, "adding a virtual monitor of {}x{} for client monitor {}", size_at(index).first,
                  size_at(index).second, index);
    }
    if (removed) {
        apply_targets();
    }
    ensure_monitor_layout();
}

bool GnomeHeadlessDesktop::set_screen_targets(std::span<const std::optional<platform::Rect>> targets)
{
    targets_.assign(targets.begin(), targets.end());
    apply_targets();
    return true;
}

void GnomeHeadlessDesktop::apply_targets()
{
    if (!ei_) {
        return;
    }
    std::vector<portal::EiInput::Output> outputs;
    for (std::size_t i = 0; i < std::min(targets_.size(), screens_.size()); ++i) {
        if (targets_[i]) {
            outputs.push_back(portal::EiInput::Output{*targets_[i], screens_[i].mapping_id});
        }
    }
    ei_->set_outputs(std::move(outputs));
}

}  // namespace

Result<std::unique_ptr<Desktop>> start_gnome_headless(const HeadlessOptions& options)
{
    auto desktop = std::make_unique<GnomeHeadlessDesktop>();
    FARLAND_TRY_VOID(desktop->start(options));
    return std::unique_ptr<Desktop>(std::move(desktop));
}

}  // namespace farland::app
