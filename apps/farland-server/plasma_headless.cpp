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
#include <farland/platform/portal/ei_input.hpp>
#include <farland/platform/portal/pipewire_capture.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <poll.h>
#include <unistd.h>

namespace farland::app {

namespace {

namespace kwin = platform::kwin;
namespace portal = platform::portal;
using Clock = std::chrono::steady_clock;
constexpr std::string_view log_component = "app.plasma";
/// The largest monitor RDP describes (MS-RDPEDISP 2.2.2.2.1).
constexpr std::int32_t max_size = 8192;
constexpr std::int32_t min_size = 200;

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
    [[nodiscard]] Result<void> start(const HeadlessOptions& options);

    [[nodiscard]] platform::FrameSource& frames() override { return screens_.front().capture->frames(); }
    [[nodiscard]] platform::CursorSource* cursor() override { return &screens_.front().capture->cursor(); }
    [[nodiscard]] platform::InputSink& input() override { return *ei_; }
    [[nodiscard]] std::vector<int> dispatch_fds() const override { return {wayland_->fd(), ei_->fd(), eis_->bus_fd()}; }
    void dispatch() override
    {
        wayland_->dispatch();
        eis_->dispatch();
        ei_->dispatch();
        if (outputs_) {
            outputs_->check_timeouts();
        }
    }
    [[nodiscard]] bool closed() const override
    {
        return wayland_->broken() || (processes_ && processes_->exited()) || ei_->closed() ||
               std::ranges::any_of(screens_, [](const Screen& s) {
                   return s.capture->closed() || s.stream->state() != kwin::ScreencastStream::State::created;
               });
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
    void request_screen_sizes(std::span<const std::pair<std::uint32_t, std::uint32_t>> sizes) override;
    bool set_screen_targets(std::span<const std::optional<platform::Rect>> targets) override;

private:
    struct Screen {
        std::string output;
        std::unique_ptr<kwin::ScreencastStream> stream;
        std::unique_ptr<portal::PipeWireCapture> capture;
    };

    [[nodiscard]] Result<void> launch(const HeadlessOptions& options, Clock::time_point deadline);
    [[nodiscard]] Result<void> wait_for_outputs(std::size_t count, Clock::time_point deadline);
    [[nodiscard]] Result<void> start_streams(const HeadlessOptions& options, Clock::time_point deadline);
    [[nodiscard]] Result<void> wait_for_first_frames(Clock::time_point deadline);
    void start_clipboard();

    // Destroyed in reverse: everything that talks to KWin before KWin ends.
    std::unique_ptr<kwin::PlasmaProcesses> processes_;
    std::unique_ptr<kwin::WaylandConnection> wayland_;
    std::unique_ptr<kwin::Screencast> screencast_;
    std::unique_ptr<kwin::OutputManagement> outputs_;
    std::vector<Screen> screens_;
    std::unique_ptr<kwin::KWinEis> eis_;
    std::unique_ptr<portal::EiInput> ei_;
    std::unique_ptr<kwin::DataControlClipboard> clipboard_;
    bool resizable_ = false;
};

Result<void> PlasmaHeadlessDesktop::start(const HeadlessOptions& options)
{
    if (options.kind != HeadlessKind::plasma) {
        return fail(Errc::invalid_value, "start_plasma_headless starts Plasma only");
    }
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
    }
    FARLAND_TRY_VOID(wait_for_outputs(1, deadline));

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
    FARLAND_TRY_VOID(start_streams(options, deadline));

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
    log::info(log_component, "{} screen{}{}", screens_.size(), screens_.size() == 1 ? "" : "s",
              resizable_ ? ", resizable" : "");
    return {};
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
        screens_.push_back(Screen{
            output->name, screencast_->stream_output(output->proxy, kwin::Screencast::Cursor::metadata), nullptr});
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

void PlasmaHeadlessDesktop::request_screen_sizes(std::span<const std::pair<std::uint32_t, std::uint32_t>> sizes)
{
    if (!resizable_) {
        return;
    }
    for (std::size_t i = 0; i < std::min(sizes.size(), screens_.size()); ++i) {
        const auto [width, height] = sizes[i];
        if (width > 0 && height > 0) {
            outputs_->request_size(screens_[i].output,
                                   static_cast<std::int32_t>(std::min<std::uint32_t>(width, max_size)),
                                   static_cast<std::int32_t>(std::min<std::uint32_t>(height, max_size)));
        }
    }
    wayland_->flush();
}

bool PlasmaHeadlessDesktop::set_screen_targets(std::span<const std::optional<platform::Rect>> targets)
{
    // KWin names each EIS region after its output.
    std::vector<portal::EiInput::Output> outputs;
    for (std::size_t i = 0; i < std::min(targets.size(), screens_.size()); ++i) {
        if (targets[i]) {
            outputs.push_back(portal::EiInput::Output{*targets[i], screens_[i].output});
        }
    }
    ei_->set_outputs(std::move(outputs));
    return true;
}

}  // namespace

Result<std::unique_ptr<Desktop>> start_plasma_headless(const HeadlessOptions& options)
{
    auto desktop = std::make_unique<PlasmaHeadlessDesktop>();
    FARLAND_TRY_VOID(desktop->start(options));
    return std::unique_ptr<Desktop>(std::move(desktop));
}

}  // namespace farland::app
