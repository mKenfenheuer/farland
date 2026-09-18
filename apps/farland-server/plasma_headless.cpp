// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "plasma_headless.hpp"

#include <farland/base/log.hpp>
#include <farland/platform/kwin/data_control_clipboard.hpp>
#include <farland/platform/kwin/kwin_eis.hpp>
#include <farland/platform/kwin/kwin_launcher.hpp>
#include <farland/platform/kwin/output_management.hpp>
#include <farland/platform/kwin/screencast.hpp>
#include <farland/platform/kwin/wayland_connection.hpp>
#include <farland/platform/logind/seat.hpp>
#include <farland/platform/portal/ei_input.hpp>
#include <farland/platform/portal/pipewire_capture.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <poll.h>
#include <unistd.h>

namespace farland::app {

namespace {

namespace kwin = platform::kwin;
namespace logind = platform::logind;
namespace portal = platform::portal;
using Clock = std::chrono::steady_clock;
using Size = std::pair<std::uint32_t, std::uint32_t>;
constexpr std::string_view log_component = "app.plasma";
/// The largest monitor RDP describes (MS-RDPEDISP 2.2.2.2.1).
constexpr std::int32_t max_size = 8192;
constexpr std::int32_t min_size = 200;
/// RDP clients show at most 16 monitors ([MS-RDPBCGR] 2.2.1.3.6).
constexpr std::size_t max_screens = 16;

/// How long after a refused login at the machine ([policy] seat_takeover) the
/// seat coming back is that refusal's doing, and not somebody taking the
/// session: the display manager gives the login screen up as soon as it is
/// told, and a later return is a new login farlandd asked about again.
constexpr auto seat_refusal_grace = std::chrono::seconds(10);

/// How long after the login screen has the seat back the client's screen
/// sizes and layout are asked for again: long enough for KWin to have
/// described what it did to the outputs while it had the seat, or asking for
/// a size it already believes we want does nothing.
constexpr auto seat_return_catch_up = std::chrono::seconds(2);

std::chrono::milliseconds left(Clock::time_point deadline)
{
    return std::max(std::chrono::milliseconds(1),
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
}

/// The Wayland sockets to try when attaching, $WAYLAND_DISPLAY first: a user
/// service manager keeps the value the session imported into it, which is the
/// socket of the session that ran *then* — Plasma restarted since, on
/// wayland-0 while the manager still says wayland-1, and the agent inherits
/// that. The sockets in $XDG_RUNTIME_DIR say what is running now.
std::vector<std::string> attach_candidates()
{
    std::vector<std::string> names;
    if (const char* display = std::getenv("WAYLAND_DISPLAY"); display != nullptr && *display != '\0') {
        names.emplace_back(display);
    }
    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    if (runtime_dir == nullptr || *runtime_dir == '\0') {
        return names;
    }
    std::error_code ec;
    std::vector<std::string> found;
    for (const auto& entry : std::filesystem::directory_iterator(runtime_dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with("wayland-") && !name.ends_with(".lock") &&
            std::ranges::find(names, name) == names.end()) {
            found.push_back(name);
        }
    }
    std::ranges::sort(found);
    names.insert(names.end(), found.begin(), found.end());
    return names;
}

class PlasmaHeadlessDesktop final : public Desktop {
public:
    PlasmaHeadlessDesktop() = default;
    PlasmaHeadlessDesktop(const PlasmaHeadlessDesktop&) = delete;
    PlasmaHeadlessDesktop& operator=(const PlasmaHeadlessDesktop&) = delete;
    PlasmaHeadlessDesktop(PlasmaHeadlessDesktop&&) = delete;
    PlasmaHeadlessDesktop& operator=(PlasmaHeadlessDesktop&&) = delete;
    ~PlasmaHeadlessDesktop() override
    {
        // The seat's screens come back before ours go: KWin is not to be left
        // with a session that has no screen at all, and whoever is at the
        // machine finds their screens as they were.
        if (outputs_ && wayland_ && !wayland_->broken()) {
            outputs_->restore_layout(std::chrono::seconds(2));
        }
    }

    [[nodiscard]] Result<void> start(const HeadlessOptions& options);

    [[nodiscard]] platform::FrameSource& frames() override { return screens_.front().capture->frames(); }
    [[nodiscard]] platform::CursorSource* cursor() override { return &screens_.front().capture->cursor(); }
    [[nodiscard]] platform::InputSink& input() override { return *ei_; }
    [[nodiscard]] std::vector<int> dispatch_fds() const override
    {
        std::vector<int> fds{wayland_->fd(), ei_->fd(), eis_->bus_fd()};
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
        wayland_->dispatch();
        eis_->dispatch();
        ei_->dispatch();
        if (outputs_) {
            outputs_->check_timeouts();
        }
        if (seat_watch_) {
            seat_watch_->process();
            keep_the_seat_after_a_refusal();
            catch_up_after_a_refusal();
        }
        service_pending();
    }
    [[nodiscard]] bool closed() const override
    {
        // A session whose virtual outputs KWin keeps drawing off the seat
        // lets the seat show a login screen while a client holds it, so
        // losing the seat is what this connection asked for; it ends when
        // somebody at the machine logs in and the session goes back to the
        // seat. One that is drawn only on its seat ends the moment it loses
        // it.
        const char* reason = nullptr;
        if (wayland_->broken()) {
            reason = "the connection to KWin broke";
        } else if (processes_ && processes_->exited()) {
            reason = "the KWin this desktop started ended";
        } else if (ei_->closed()) {
            reason = "the input connection closed";
        } else if (seat_watch_ && keeps_rendering() && seat_watch_->returned_to_the_seat()) {
            reason = "somebody logged in at the machine and took the session back";
        } else if (seat_watch_ && !keeps_rendering() && seat_watch_->left_the_seat()) {
            reason = "something else took the seat the session is on";
        } else if (screens_.empty()) {
            reason = "the session has no screens left";
        } else if (std::ranges::any_of(screens_, [](const Screen& s) {
                       return s.capture->closed() || s.stream->state() != kwin::ScreencastStream::State::created;
                   })) {
            reason = "a screen's stream closed";
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
    [[nodiscard]] bool resizable() const override { return resizable_; }
    [[nodiscard]] bool screens_follow_monitors() const override { return virtual_screens_; }
    void request_screen_sizes(std::span<const Size> sizes) override;
    bool set_screen_targets(std::span<const std::optional<platform::Rect>> targets) override;
    void set_held(bool held) override;
    void seat_takeover_decided(bool allowed) override;
    [[nodiscard]] bool keep_when_released() const override { return !options_.attach; }

private:
    struct Screen {
        /// The KWin output this screen shows: a real one when the desktop
        /// mirrors the seat's screen, else a virtual one of ours. It is the
        /// name KWin gives the output, which is what names the libei region
        /// as well, and not always the name that was asked for.
        std::string output;
        /// The size asked for; what the frames have once KWin followed.
        Size size{0, 0};
        std::unique_ptr<kwin::ScreencastStream> stream;
        std::unique_ptr<portal::PipeWireCapture> capture;
    };

    [[nodiscard]] Result<void> launch(const HeadlessOptions& options, Clock::time_point deadline);
    [[nodiscard]] Result<void> wait_for_outputs(std::size_t count, Clock::time_point deadline);
    [[nodiscard]] Result<void> start_streams(const HeadlessOptions& options, Clock::time_point deadline);
    [[nodiscard]] Result<void> wait_for_first_frames(Clock::time_point deadline);
    void start_clipboard();

    /// Starts watching the seat, so that this connection ends when the
    /// session goes back to it.
    [[nodiscard]] Result<void> take_the_session();
    /// One virtual output of `size`, streamed; it becomes a screen once its
    /// first frame arrived (service_pending()).
    [[nodiscard]] bool add_screen(Size size);
    /// Connects the capture of a screen KWin answered for, and gives it the
    /// name KWin ended up using. False when the stream failed: it is no
    /// screen and never will be.
    [[nodiscard]] bool service_screen(Screen& screen);
    /// The screen has a picture to show.
    [[nodiscard]] static bool has_first_frame(const Screen& screen)
    {
        return screen.capture && screen.capture->frames().size().first != 0;
    }
    /// Connects the captures of new streams and moves screens whose first
    /// frame came to screens_, in the order they were added.
    void service_pending();
    [[nodiscard]] Result<void> wait_for_first_screen(Clock::time_point deadline);
    /// Puts the client's targets on the input sink, for the screens there are.
    void apply_targets();
    /// True while this desktop is attached to a session on a seat whose
    /// screens KWin keeps drawing off that seat, so that the seat can show a
    /// login screen while a client holds the session. Measured on KWin 6.6.6:
    /// a virtual output made through zkde_screencast_unstable_v1 goes on
    /// giving frames at its full rate while the session is not active on its
    /// seat, and only the seat's own outputs stop (DrmGpu::setActive inhibits
    /// their render loops alone).
    [[nodiscard]] bool keeps_rendering() const { return options_.attach && virtual_screens_; }
    /// Hands the seat the display manager's login screen while a client holds
    /// the session, so that the screen at the machine shows neither the
    /// session nor what was last on it, and whoever is there takes it back by
    /// logging in. False when the seat kept the session, so that the screen
    /// there still shows it.
    [[nodiscard]] bool hand_the_seat_a_greeter();
    /// [policy] seat_takeover: the seat came back right after a login at the
    /// machine was refused, because the display manager gave up the login
    /// screen it was showing. The seat gets one back and the client keeps
    /// the session; without this the return would end the connection, which
    /// is exactly what the refusal said must not happen.
    void keep_the_seat_after_a_refusal();
    /// Asks KWin for the size every screen already wanted. Nothing changed
    /// for the client, but KWin resizes a virtual output back to its own
    /// default while the session is on its seat, and only takes a new size
    /// while it is there.
    void ask_for_the_screen_sizes_again();
    /// Puts the client's screen sizes and layout back a moment after the
    /// seat gave the session up again (keep_the_seat_after_a_refusal()).
    void catch_up_after_a_refusal();
    /// Lays the session out around this connection's screens, so that the
    /// panel, new windows and the overview are where the client looks.
    void ensure_layout();
    /// Waits until KWin has laid the session out, at most `timeout`.
    void wait_for_layout(std::chrono::milliseconds timeout);

    HeadlessOptions options_;
    // Destroyed in reverse: everything that talks to KWin before KWin ends.
    std::unique_ptr<kwin::PlasmaProcesses> processes_;
    std::unique_ptr<kwin::WaylandConnection> wayland_;
    std::unique_ptr<kwin::Screencast> screencast_;
    std::unique_ptr<kwin::OutputManagement> outputs_;
    std::vector<Screen> screens_;
    /// Streamed, but without a first frame yet; they come after screens_.
    std::vector<Screen> pending_;
    std::unique_ptr<kwin::KWinEis> eis_;
    std::unique_ptr<portal::EiInput> ei_;
    std::unique_ptr<kwin::DataControlClipboard> clipboard_;
    std::vector<std::optional<platform::Rect>> targets_;
    /// Set while this connection took the session from a seat: it ends when
    /// the seat takes it back.
    std::unique_ptr<logind::SeatWatch> seat_watch_;
    /// The screens are virtual outputs of ours, one per client monitor, and
    /// not the seat's own screen mirrored.
    bool virtual_screens_ = false;
    bool resizable_ = false;
    /// A client holds the desktop (set_held()).
    bool held_ = false;
    /// A login at the machine was refused just now, and the seat coming back
    /// until then is the display manager giving its login screen up, not
    /// somebody taking the session (seat_takeover_decided()).
    Clock::time_point seat_refused_until_{};
    /// When to put the client's sizes and layout back after such a return;
    /// unset when there is nothing to put back.
    Clock::time_point catch_up_at_{};
    /// The virtual outputs created so far, for names that stay unique.
    std::uint64_t next_virtual_ = 0;
    /// closed() said why, once.
    mutable bool said_why_closed_ = false;
    bool warned_resize_ = false;
};

Result<void> PlasmaHeadlessDesktop::start(const HeadlessOptions& options)
{
    if (options.kind != HeadlessKind::plasma) {
        return fail(Errc::invalid_value, "start_plasma_headless starts Plasma only");
    }
    options_ = options;
    const auto deadline = Clock::now() + options.timeout;
    std::string bus_address;  // the session bus when attached
    if (options.attach) {
        log::info(log_component, "attaching to the running KWin");
        const auto candidates = attach_candidates();
        std::string tried;
        for (const auto& name : candidates) {
            auto connection = kwin::WaylandConnection::connect(name, left(deadline));
            if (connection) {
                if (!tried.empty()) {
                    log::info(log_component, "$WAYLAND_DISPLAY names no running compositor; attached to {}", name);
                }
                wayland_ = std::move(*connection);
                break;
            }
            log::debug(log_component, "cannot attach to {}: {}", name, connection.error().message());
            tried += tried.empty() ? name : ", " + name;
        }
        if (!wayland_) {
            return fail(Errc::io, tried.empty() ? "no Wayland socket to attach to: neither $WAYLAND_DISPLAY nor "
                                                  "$XDG_RUNTIME_DIR names one"
                                                : std::format("cannot connect to the running KWin (tried {})", tried));
        }
    } else {
        FARLAND_TRY_VOID(launch(options, deadline));
        bus_address = processes_->bus_address();
        FARLAND_TRY_VOID(wait_for_outputs(1, deadline));
    }

    auto screencast = kwin::Screencast::create(*wayland_);
    if (!screencast) {
        std::error_code ec;
        const auto self = std::filesystem::canonical("/proc/self/exe", ec).string();
        log::error(log_component,
                   "KWin does not grant screen casting to {}: it needs a desktop file with Exec={} and "
                   "X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1 in the applications directory of "
                   "KWin's XDG_DATA_DIRS or ~/.local/share/applications (see data/org.farland.server.desktop.in)",
                   self, self);
        return std::unexpected(screencast.error());
    }
    screencast_ = std::move(*screencast);
    if (auto outputs = kwin::OutputManagement::create(*wayland_)) {
        outputs_ = std::move(*outputs);
    } else {
        log::warn(log_component, "{}: the screens keep their size", outputs.error().what);
    }
    // A session of our own is drawn wherever it runs, on outputs KWin made
    // for it. One we attach to lives on a seat, and mirroring its screen
    // would show the client what whoever walks past the machine sees: there
    // the client gets virtual outputs of its own, so that the seat is free to
    // show a login screen while the client holds the session.
    virtual_screens_ = options.attach && screencast_->has_virtual_outputs();
    if (options.attach && !virtual_screens_) {
        log::warn(log_component,
                  "this KWin has no virtual outputs in zkde_screencast_unstable_v1 (version {}); the client sees the "
                  "screen at the machine, and the screen at the machine keeps showing the session",
                  screencast_->version());
    }
    if (virtual_screens_) {
        FARLAND_TRY_VOID(take_the_session());
        if (!add_screen({options.width, options.height})) {
            return fail(Errc::io, "KWin did not create a virtual output");
        }
        FARLAND_TRY_VOID(wait_for_first_screen(deadline));
        resizable_ = true;
    } else {
        if (options.attach) {
            FARLAND_TRY_VOID(wait_for_outputs(1, deadline));
        }
        FARLAND_TRY_VOID(start_streams(options, deadline));
    }

    auto eis = kwin::KWinEis::connect(
        bus_address, kwin::KWinEis::keyboard | kwin::KWinEis::pointer | kwin::KWinEis::touch, left(deadline));
    if (!eis) {
        return std::unexpected(eis.error());
    }
    eis_ = std::move(*eis);
    auto ei = portal::EiInput::connect_fd(eis_->release_socket());
    if (!ei) {
        log::error(log_component, "libei: {}", ei.error().message());
        return fail(Errc::io, "cannot connect to KWin's input (libei)");
    }
    ei_ = std::move(*ei);
    start_clipboard();
    FARLAND_TRY_VOID(wait_for_first_frames(deadline));

    std::vector<std::optional<platform::Rect>> targets;
    std::int32_t x = 0;
    for (const auto& screen : screens_) {
        const auto [width, height] = screen.capture->frames().size();
        targets.emplace_back(platform::Rect{x, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)});
        x += static_cast<std::int32_t>(width);
        log::info(log_component, "screen {}x{}: KWin output {}", width, height, screen.output);
    }
    static_cast<void>(set_screen_targets(targets));
    // The seat keeps the session until a client actually holds the desktop:
    // the agent says so (set_held()) as soon as a connection starts, and only
    // then does the screen at the machine get a login screen.
    log::info(log_component, "{} screen{}{}{}", screens_.size(), screens_.size() == 1 ? "" : "s",
              resizable_ ? ", resizable" : "", virtual_screens_ ? ", on virtual outputs of this connection" : "");
    return {};
}

Result<void> PlasmaHeadlessDesktop::take_the_session()
{
    // From here on the seat taking the session back ends this connection.
    if (auto watch = logind::SeatWatch::create()) {
        seat_watch_ = std::move(*watch);
    } else {
        log::debug(log_component, "cannot watch the seat: {}", watch.error().message);
    }
    // Nothing brings the session to its seat first: KWin keeps drawing a
    // virtual output while the session is not active there, so the session is
    // this client's wherever it is. Where it is off its seat — an earlier
    // connection left a login screen on it — a KWin without farland's fix
    // refuses to *make* the output; wait_for_first_screen() says so.
    return {};
}

bool PlasmaHeadlessDesktop::add_screen(Size size)
{
    const auto width = static_cast<std::int32_t>(std::clamp<std::uint32_t>(size.first, min_size, max_size));
    const auto height = static_cast<std::int32_t>(std::clamp<std::uint32_t>(size.second, min_size, max_size));
    const std::size_t index = screens_.size() + pending_.size();
    // KWin makes an output of its own from this name (it prefixes its own
    // "Virtual-"), and names the libei region after the output; the
    // description is what the display settings show.
    const std::string name = std::format("farland-{}", next_virtual_++);
    auto stream = screencast_->stream_virtual_output(name, std::format("farland screen {}", index + 1), width, height,
                                                     1.0, kwin::Screencast::Cursor::metadata);
    if (!stream) {
        return false;
    }
    Screen screen;
    screen.output = name;
    screen.size = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
    screen.stream = std::move(stream);
    pending_.push_back(std::move(screen));
    service_pending();
    return true;
}

bool PlasmaHeadlessDesktop::service_screen(Screen& s)
{
    if (s.stream->state() == kwin::ScreencastStream::State::pending) {
        return true;
    }
    if (s.stream->state() != kwin::ScreencastStream::State::created) {
        log::error(log_component, "KWin cannot cast the virtual output {}: {}", s.output, s.stream->error());
        return false;
    }
    if (!s.capture) {
        portal::PipeWireCaptureOptions capture_options;
        capture_options.render_node = options_.render_node;
        capture_options.stream_name = std::format("farland-kwin-{}", s.output);
        // KWin's streams are on the user's PipeWire daemon.
        auto capture = portal::PipeWireCapture::create(-1, s.stream->node(), capture_options);
        if (!capture) {
            log::error(log_component, "PipeWire capture of node {}: {}", s.stream->node(), capture.error().message());
            return false;
        }
        s.capture = std::move(*capture);
    }
    // KWin puts its own prefix in front of the name it was given, and the
    // libei region carries the name KWin ended up with. The output global may
    // arrive after the stream, so this keeps trying until it is there.
    if (wayland_->find_output(s.output) == nullptr) {
        const std::string prefixed = "Virtual-" + s.output;
        if (wayland_->find_output(prefixed) != nullptr) {
            s.output = prefixed;
            apply_targets();
        }
    }
    if (s.capture->closed()) {
        log::warn(log_component, "a new virtual output's stream closed: {}", s.capture->error());
        return false;
    }
    return true;
}

void PlasmaHeadlessDesktop::service_pending()
{
    bool changed = false;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (service_screen(*it)) {
            ++it;
        } else {
            it = pending_.erase(it);
        }
    }
    while (!pending_.empty() && has_first_frame(pending_.front())) {
        Screen& s = pending_.front();
        const auto [width, height] = s.capture->frames().size();
        log::info(log_component, "screen {}: a virtual output of {}x{} (KWin output {}, PipeWire node {})",
                  screens_.size(), width, height, s.output, s.capture->node_id());
        screens_.push_back(std::move(s));
        pending_.erase(pending_.begin());
        changed = true;
    }
    if (changed) {
        apply_targets();
        ensure_layout();
    }
}

Result<void> PlasmaHeadlessDesktop::wait_for_first_screen(Clock::time_point deadline)
{
    while (screens_.empty()) {
        if (wayland_->broken()) {
            return fail(Errc::io, "the connection to KWin broke before the first frame");
        }
        // KWin goes on drawing a virtual output while the session is not
        // active on its seat, but a KWin without farland's fix will not make
        // one there: it refuses every output configuration off the seat, so
        // the output it made never becomes one it shows ("Could not find
        // output"), and there is nothing to wait for.
        const bool off_the_seat = seat_watch_ != nullptr && !seat_watch_->active();
        const auto not_made = [] {
            return fail(Errc::io,
                        "this KWin will not make a screen for a client while the session is off its seat; somebody "
                        "has to log in at the machine first, or the machine needs the KWin from "
                        "packaging/make-kwin-deb.sh");
        };
        if (pending_.empty()) {
            return off_the_seat ? not_made() : fail(Errc::io, "the virtual output's stream failed");
        }
        if (Clock::now() > deadline) {
            return off_the_seat ? not_made() : fail(Errc::io, "no frame from the virtual output");
        }
        std::array<pollfd, 2> fds{{{wayland_->fd(), POLLIN, 0}, {-1, POLLIN, 0}}};
        if (pending_.front().capture) {
            fds[1].fd = pending_.front().capture->frames().wake_fd();
        }
        ::poll(fds.data(), fds.size(), 100);
        wayland_->dispatch();
        if (seat_watch_) {
            seat_watch_->process();
        }
        service_pending();
    }
    return {};
}

void PlasmaHeadlessDesktop::set_held(bool held)
{
    if (held_ == held || !virtual_screens_) {
        held_ = held;
        return;
    }
    held_ = held;
    if (held) {
        // The layout first, and finished before the seat goes: KWin refuses
        // every output configuration once the session is off its seat
        // ("Atomic modeset test failed! Permission denied"), so a layout
        // still on its way when the login screen arrives never happens.
        ensure_layout();
        wait_for_layout(std::chrono::seconds(3));
        static_cast<void>(hand_the_seat_a_greeter());
        return;
    }
    if (outputs_) {
        outputs_->restore_layout(std::chrono::seconds(2));
    }
    // Nobody holds it any more. keep_when_released() is false for an attached
    // desktop, so the agent takes the whole desktop down: the virtual outputs
    // go with it and KWin lays the session out for the seat's screen again.
    // Where the seat has a login screen, it keeps it, and logging in there
    // brings the session back with its windows.
    log::info(log_component, "no client holds the session: it is the seat's again");
}

void PlasmaHeadlessDesktop::ask_for_the_screen_sizes_again()
{
    if (!virtual_screens_ || outputs_ == nullptr) {
        return;
    }
    for (const auto& screen : screens_) {
        if (screen.size.first == 0 || screen.size.second == 0 || !outputs_->resizable(screen.output)) {
            continue;
        }
        outputs_->request_size(screen.output,
                               static_cast<std::int32_t>(std::min<std::uint32_t>(screen.size.first, max_size)),
                               static_cast<std::int32_t>(std::min<std::uint32_t>(screen.size.second, max_size)));
    }
    wayland_->flush();
}

void PlasmaHeadlessDesktop::seat_takeover_decided(bool allowed)
{
    // Every answer settles the last one: a login that is allowed to take the
    // session over must not be bounced back by an earlier refusal.
    seat_refused_until_ = allowed ? Clock::time_point{} : Clock::now() + seat_refusal_grace;
}

void PlasmaHeadlessDesktop::keep_the_seat_after_a_refusal()
{
    if (!keeps_rendering() || !seat_watch_->returned_to_the_seat() || Clock::now() >= seat_refused_until_) {
        return;
    }
    seat_refused_until_ = {};  // one return belongs to one refusal
    if (!hand_the_seat_a_greeter()) {
        // The screen at the machine would go on showing the session to
        // whoever was refused, which is worse than the connection ending:
        // the return stands, and the desktop closes as it did before.
        return;
    }
    seat_watch_->forget_return();
    // KWin resized the virtual output to its own default and laid the
    // session out for the seat's screen while it had the seat back, and the
    // sizes and the layout have to go back. Not now, though: a configuration
    // in the moment of the return is refused whole ("Atomic modeset test
    // failed! Permission denied", KWin back on the seat without DRM master
    // yet), and what KWin did to the outputs has not even been described to
    // us yet, so asking for a size it already believes we want does nothing.
    // It waits for the login screen to have the seat and for KWin's own
    // word on the outputs; off the seat a configuration reaches the virtual
    // outputs alone (packaging/kwin).
    catch_up_at_ = Clock::now() + seat_return_catch_up;
    log::info(log_component, "the login at the machine was refused, so the seat has a login screen again and the "
                             "client keeps the session");
}

void PlasmaHeadlessDesktop::catch_up_after_a_refusal()
{
    if (catch_up_at_ == Clock::time_point{} || Clock::now() < catch_up_at_) {
        return;
    }
    catch_up_at_ = {};
    ask_for_the_screen_sizes_again();
    ensure_layout();
    log::info(log_component, "the session is laid out for the client's screens again after the seat gave it back");
}

bool PlasmaHeadlessDesktop::hand_the_seat_a_greeter()
{
    if (!keeps_rendering() || seat_watch_ == nullptr) {
        return false;
    }
    if (!seat_watch_->active()) {
        // Something else is on the seat already (a login screen from an
        // earlier connection, or another session): nothing to hand over.
        log::info(log_component, "the seat is not showing this session, so it keeps what it has");
        return true;
    }
    if (auto handed = logind::switch_seat_to_greeter(); !handed) {
        // The seat keeps showing the session; the client still has it.
        log::warn(log_component,
                  "cannot put a login screen on the seat ({}); the screen at the machine keeps showing the session",
                  handed.error().message);
        return false;
    }
    log::info(log_component, "the screen at the machine shows a login screen; logging in there takes the session back");
    return true;
}

void PlasmaHeadlessDesktop::wait_for_layout(std::chrono::milliseconds timeout)
{
    if (outputs_ == nullptr) {
        return;
    }
    const auto deadline = Clock::now() + timeout;
    while (outputs_->busy() && Clock::now() < deadline) {
        static_cast<void>(wayland_->roundtrip(std::chrono::milliseconds(100)));
        outputs_->check_timeouts();
    }
}

void PlasmaHeadlessDesktop::ensure_layout()
{
    if (!virtual_screens_ || !held_ || outputs_ == nullptr || screens_.empty()) {
        return;
    }
    std::vector<std::string> names;
    names.reserve(screens_.size());
    for (const auto& screen : screens_) {
        names.push_back(screen.output);
    }
    // The client's first screen becomes the primary one, so the panel, new
    // windows and the overview are where the client looks. The seat's own
    // screen stays on beside them: the login screen is what keeps it from
    // showing the session, and KWin would not let us switch it on again
    // afterwards (it refuses configurations off the seat).
    outputs_->request_layout(names);
}

Result<void> PlasmaHeadlessDesktop::launch(const HeadlessOptions& options, Clock::time_point deadline)
{
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");  // NOLINT(concurrency-mt-unsafe): read before threads
    if (runtime == nullptr || *runtime == '\0') {
        return fail(Errc::io, "XDG_RUNTIME_DIR is not set");
    }
    std::error_code ec;
    kwin::PlasmaLaunchOptions launch;
    launch.width = std::clamp<std::uint32_t>(options.width, min_size, max_size);
    launch.height = std::clamp<std::uint32_t>(options.height, min_size, max_size);
    launch.keymap_layout = options.keymap_layout;
    launch.runtime_dir = std::filesystem::path(runtime) / "farland" / std::format("plasma-{}", ::getpid());
    launch.data_dir = std::filesystem::path(runtime) / "farland" / "kwin-data";
    launch.client_executable = std::filesystem::canonical("/proc/self/exe", ec);
    if (ec) {
        return fail(Errc::io, "cannot find this program's executable");
    }
    const auto plan = kwin::plan_plasma_launch(launch, [](const char* name) -> std::optional<std::string> {
        const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read before threads
        return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
    });
    log::info(log_component, "starting a headless Plasma session of {}x{}", launch.width, launch.height);
    auto processes = kwin::PlasmaProcesses::start(plan, left(deadline));
    if (!processes) {
        return std::unexpected(processes.error());
    }
    processes_ = std::move(*processes);
    // KWin takes its socket before it listens on it.
    for (;;) {
        auto connection = kwin::WaylandConnection::connect(processes_->wayland_display(), left(deadline));
        if (connection) {
            wayland_ = std::move(*connection);
            break;
        }
        if (processes_->exited() || Clock::now() > deadline) {
            processes_->log_output_tail();
            return fail(Errc::io, "cannot connect to the launched KWin");
        }
        pollfd none{-1, 0, 0};
        ::poll(&none, 1, 100);
    }
    log::info(log_component, "KWin is up on {}", processes_->wayland_display());
    return {};
}

Result<void> PlasmaHeadlessDesktop::wait_for_outputs(std::size_t count, Clock::time_point deadline)
{
    const auto ready = [this] {
        return static_cast<std::size_t>(std::ranges::count_if(
            wayland_->outputs(), [](const auto& o) { return o->ready && !o->name.empty() && o->width > 0; }));
    };
    while (ready() < count) {
        if (!wayland_->roundtrip(left(deadline)) || Clock::now() > deadline) {
            return fail(Errc::io, "KWin announced no output");
        }
    }
    return {};
}

Result<void> PlasmaHeadlessDesktop::start_streams(const HeadlessOptions& options, Clock::time_point deadline)
{
    std::vector<const kwin::WaylandOutput*> outputs;
    for (const auto& output : wayland_->outputs()) {
        if (output->ready && !output->name.empty()) {
            outputs.push_back(output.get());
        }
    }
    std::ranges::sort(outputs,
                      [](const auto* a, const auto* b) { return std::pair{a->x, a->y} < std::pair{b->x, b->y}; });

    // A launched KWin's outputs are all virtual; an attached KWin's are
    // resized only when they are virtual ones too, never real monitors.
    resizable_ = outputs_ != nullptr && std::ranges::all_of(outputs, [this, &options](const auto* o) {
                     return outputs_->resizable(o->name) && (!options.attach || o->name.starts_with("Virtual-"));
                 });
    if (resizable_ && !options.attach) {
        // KWin may have restored another size for the output from an
        // earlier session.
        const auto width = static_cast<std::int32_t>(std::clamp<std::uint32_t>(options.width, min_size, max_size));
        const auto height = static_cast<std::int32_t>(std::clamp<std::uint32_t>(options.height, min_size, max_size));
        outputs_->request_size(outputs.front()->name, width, height);
        while (outputs_->busy() && Clock::now() < deadline) {
            static_cast<void>(wayland_->roundtrip(std::chrono::milliseconds(100)));
            outputs_->check_timeouts();
        }
    }

    for (const auto* output : outputs) {
        Screen screen;
        screen.output = output->name;
        screen.size = {static_cast<std::uint32_t>(output->width), static_cast<std::uint32_t>(output->height)};
        screen.stream = screencast_->stream_output(output->proxy, kwin::Screencast::Cursor::metadata);
        screens_.push_back(std::move(screen));
    }
    for (auto& screen : screens_) {
        while (screen.stream->state() == kwin::ScreencastStream::State::pending) {
            if (!wayland_->roundtrip(left(deadline)) || Clock::now() > deadline) {
                return fail(Errc::io, "KWin did not start the screen cast");
            }
        }
        if (screen.stream->state() != kwin::ScreencastStream::State::created) {
            log::error(log_component, "KWin cannot cast output {}: {}", screen.output, screen.stream->error());
            return fail(Errc::io, "KWin refused the screen cast (it needs OpenGL compositing)");
        }
        portal::PipeWireCaptureOptions capture_options;
        capture_options.render_node = options.render_node;
        capture_options.stream_name = std::format("farland-kwin-{}", screen.output);
        // KWin's streams are on the user's PipeWire daemon.
        auto capture = portal::PipeWireCapture::create(-1, screen.stream->node(), capture_options);
        if (!capture) {
            log::error(log_component, "PipeWire capture of node {}: {}", screen.stream->node(),
                       capture.error().message());
            return fail(Errc::io, "cannot capture KWin's screen cast stream");
        }
        screen.capture = std::move(*capture);
    }
    return {};
}

void PlasmaHeadlessDesktop::start_clipboard()
{
    auto clipboard = kwin::DataControlClipboard::create(*wayland_);
    if (!clipboard) {
        log::warn(log_component, "no clipboard: {}", clipboard.error().what);
        return;
    }
    clipboard_ = std::move(*clipboard);
}

Result<void> PlasmaHeadlessDesktop::wait_for_first_frames(Clock::time_point deadline)
{
    for (const auto& screen : screens_) {
        while (screen.capture->frames().size().first == 0) {
            if (closed()) {
                log::error(log_component, "the screen cast ended before its first frame: {}", screen.capture->error());
                return fail(Errc::io, "KWin's screen cast stream closed");
            }
            if (Clock::now() > deadline) {
                return fail(Errc::io, "no frame from KWin's screen cast stream");
            }
            pollfd pfd{screen.capture->frames().wake_fd(), POLLIN, 0};
            ::poll(&pfd, 1, 100);
            dispatch();
        }
    }
    return {};
}

void PlasmaHeadlessDesktop::request_screen_sizes(std::span<const Size> sizes)
{
    if (!resizable_) {
        return;
    }
    if (!virtual_screens_) {
        for (std::size_t i = 0; i < std::min(sizes.size(), screens_.size()); ++i) {
            const auto [width, height] = sizes[i];
            if (width > 0 && height > 0) {
                outputs_->request_size(screens_[i].output,
                                       static_cast<std::int32_t>(std::min<std::uint32_t>(width, max_size)),
                                       static_cast<std::int32_t>(std::min<std::uint32_t>(height, max_size)));
            }
        }
        wayland_->flush();
        return;
    }
    const std::size_t wanted = std::clamp<std::size_t>(sizes.size(), 1, max_screens);
    const auto size_at = [&](std::size_t i) -> Size {
        if (i < sizes.size() && sizes[i].first > 0 && sizes[i].second > 0) {
            return sizes[i];
        }
        return {options_.width, options_.height};
    };
    // Fewer monitors: the newest screens go, those still coming first. The
    // virtual output goes away when its stream closes.
    bool removed = false;
    while (screens_.size() + pending_.size() > wanted) {
        auto& from = pending_.empty() ? screens_ : pending_;
        from.pop_back();
        removed = true;
    }
    for (std::size_t i = 0; i < screens_.size() + pending_.size(); ++i) {
        Screen& s = i < screens_.size() ? screens_[i] : pending_[i - screens_.size()];
        const Size size = size_at(i);
        if (s.size == size) {
            continue;
        }
        s.size = size;
        if (outputs_ != nullptr && outputs_->resizable(s.output)) {
            outputs_->request_size(s.output, static_cast<std::int32_t>(std::min<std::uint32_t>(size.first, max_size)),
                                   static_cast<std::int32_t>(std::min<std::uint32_t>(size.second, max_size)));
        } else if (!warned_resize_) {
            // KWin gives a virtual output custom modes; another compositor,
            // or an output KWin has not described yet, may not.
            warned_resize_ = true;
            log::warn(log_component, "KWin does not resize the output {}; the client's picture is scaled to {}x{}",
                      s.output, size.first, size.second);
        }
    }
    // More: new virtual outputs, which show up in dispatch() once their first
    // frame came.
    while (screens_.size() + pending_.size() < wanted) {
        const std::size_t index = screens_.size() + pending_.size();
        if (!add_screen(size_at(index))) {
            break;
        }
        log::info(log_component, "adding a virtual output of {}x{} for client monitor {}", size_at(index).first,
                  size_at(index).second, index);
    }
    if (removed) {
        apply_targets();
    }
    wayland_->flush();
}

bool PlasmaHeadlessDesktop::set_screen_targets(std::span<const std::optional<platform::Rect>> targets)
{
    targets_.assign(targets.begin(), targets.end());
    apply_targets();
    return true;
}

void PlasmaHeadlessDesktop::apply_targets()
{
    if (!ei_) {
        return;
    }
    // KWin names each EIS region after its output.
    std::vector<portal::EiInput::Output> outputs;
    for (std::size_t i = 0; i < std::min(targets_.size(), screens_.size()); ++i) {
        if (targets_[i]) {
            outputs.push_back(portal::EiInput::Output{*targets_[i], screens_[i].output});
        }
    }
    ei_->set_outputs(std::move(outputs));
}

}  // namespace

Result<std::unique_ptr<Desktop>> start_plasma_headless(const HeadlessOptions& options)
{
    auto desktop = std::make_unique<PlasmaHeadlessDesktop>();
    FARLAND_TRY_VOID(desktop->start(options));
    return std::unique_ptr<Desktop>(std::move(desktop));
}

}  // namespace farland::app
