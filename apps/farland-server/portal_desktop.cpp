// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "portal_desktop.hpp"

#include <farland/base/log.hpp>
#include <farland/platform/portal/ei_input.hpp>
#include <farland/platform/portal/pipewire_capture.hpp>
#include <farland/platform/portal/portal_clipboard.hpp>
#include <farland/platform/portal/portal_input.hpp>
#include <farland/platform/portal/portal_session.hpp>

#include <algorithm>
#include <format>
#include <poll.h>

namespace farland::app {

namespace {

namespace portal = platform::portal;
using Clock = std::chrono::steady_clock;
constexpr std::string_view log_component = "app.portal";
/// How long the compositor may take to deliver the first frame.
constexpr auto first_frame_timeout = std::chrono::seconds(10);

/// A permission for existing monitors does not restore a virtual one and the
/// other way round, so each mode keeps its own token. The token covers every
/// stream the user picked.
std::optional<std::filesystem::path> restore_token_path(const PortalDesktopOptions& options)
{
    if (options.restore_token_file) {
        return options.restore_token_file;
    }
    auto path = portal::default_restore_token_path();
    if (path && options.virtual_monitor) {
        path->replace_filename("portal-restore-token-virtual");
    }
    return path;
}

/// Streams left to right as the compositor arranges them (then top to
/// bottom); streams without a position (virtual monitors, windows) after
/// them, in the portal's order.
std::vector<portal::PortalStream> in_screen_order(std::vector<portal::PortalStream> streams)
{
    std::ranges::stable_sort(streams, [](const portal::PortalStream& a, const portal::PortalStream& b) {
        if (a.position.has_value() != b.position.has_value()) {
            return a.position.has_value();
        }
        return a.position && b.position && *a.position < *b.position;
    });
    return streams;
}

class PortalDesktop final : public Desktop {
public:
    [[nodiscard]] Result<void> start(const PortalDesktopOptions& options);

    [[nodiscard]] platform::FrameSource& frames() override { return screens_.front().capture->frames(); }
    [[nodiscard]] platform::CursorSource* cursor() override { return &screens_.front().capture->cursor(); }
    [[nodiscard]] platform::InputSink& input() override
    {
        if (ei_) {
            return *ei_;
        }
        return *notify_;
    }
    [[nodiscard]] std::vector<int> dispatch_fds() const override
    {
        std::vector<int> fds{session_->fd()};
        if (ei_) {
            fds.push_back(ei_->fd());
        }
        return fds;
    }
    void dispatch() override
    {
        session_->process();
        if (ei_) {
            ei_->dispatch();
        }
    }
    [[nodiscard]] bool closed() const override
    {
        return session_->closed() || screens_.empty() ||
               std::ranges::any_of(screens_, [](const Screen& s) { return !s.capture || s.capture->closed(); }) ||
               (ei_ && ei_->closed());
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
    [[nodiscard]] bool resizable() const override { return virtual_monitor_; }
    void request_screen_sizes(std::span<const std::pair<std::uint32_t, std::uint32_t>> sizes) override;
    bool set_screen_targets(std::span<const std::optional<platform::Rect>> targets) override;

private:
    struct Screen {
        portal::PortalStream stream;
        std::unique_ptr<portal::PipeWireCapture> capture;
    };

    void start_clipboard();
    [[nodiscard]] Result<void> start_session(portal::PortalOptions& options);
    [[nodiscard]] Result<void> connect_input();
    [[nodiscard]] Result<void> wait_for_first_frames();

    // Declared first, destroyed last: the captures and the input need the session.
    std::unique_ptr<portal::PortalSession> session_ = std::make_unique<portal::PortalSession>();
    std::vector<Screen> screens_;
    std::unique_ptr<portal::EiInput> ei_;
    std::unique_ptr<portal::PortalNotifyInput> notify_;
    bool virtual_monitor_ = false;
    std::unique_ptr<portal::PortalClipboard> clipboard_;
};

Result<void> PortalDesktop::start(const PortalDesktopOptions& options)
{
    portal::PortalOptions portal_options;
    portal_options.virtual_monitor = options.virtual_monitor;
    portal_options.monitors = !options.virtual_monitor;
    // Every monitor the user picks; a portal session has one virtual monitor
    // at most (xdg-desktop-portal-gnome and -kde both).
    portal_options.multiple = !options.virtual_monitor;
    portal_options.timeout = options.timeout;
    virtual_monitor_ = options.virtual_monitor;
    portal_options.clipboard = options.clipboard;

    const auto token_file = restore_token_path(options);
    if (token_file) {
        if (const auto token = portal::load_restore_token(*token_file); token && *token) {
            portal_options.restore_token = **token;
        }
    }
    FARLAND_TRY_VOID(start_session(portal_options));
    if (token_file) {
        const auto& token = session_->restore_token();
        const auto stored =
            token ? portal::save_restore_token(*token_file, *token) : portal::remove_restore_token(*token_file);
        if (!stored) {
            log::warn(log_component, "cannot store the portal restore token in {}", token_file->string());
        }
    }

    if (options.clipboard) {
        start_clipboard();
    }

    auto remote = session_->open_pipewire_remote();
    if (!remote) {
        log::error(log_component, "OpenPipeWireRemote: {}", remote.error().message);
        return fail(Errc::io, "the desktop portal gave no PipeWire connection");
    }
    // start() fails without any stream.
    for (auto& stream : in_screen_order(session_->streams())) {
        portal::PipeWireCaptureOptions capture_options;
        capture_options.render_node = options.render_node;
        capture_options.stream_name = std::format("farland-capture-{}", screens_.size());
        auto capture = portal::PipeWireCapture::create(remote->get(), stream.node_id, capture_options);
        if (!capture) {
            log::error(log_component, "PipeWire capture of node {}: {}", stream.node_id, capture.error().message());
            return fail(Errc::io, "cannot capture the screen cast stream");
        }
        screens_.push_back(Screen{std::move(stream), std::move(*capture)});
    }
    FARLAND_TRY_VOID(connect_input());
    FARLAND_TRY_VOID(wait_for_first_frames());

    // Until a session places the screens: side by side at their sizes.
    std::vector<std::optional<platform::Rect>> targets;
    std::int32_t x = 0;
    for (const auto& screen : screens_) {
        const auto [width, height] = screen.capture->frames().size();
        targets.emplace_back(platform::Rect{x, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)});
        x += static_cast<std::int32_t>(width);
        std::string where;
        if (screen.stream.position) {
            where = std::format(" at {},{}", screen.stream.position->first, screen.stream.position->second);
        }
        log::info(log_component, "sharing {}x{}{}{} (PipeWire node {})", width, height, where,
                  screen.stream.source_type == portal::source_virtual ? ", a virtual monitor" : "",
                  screen.stream.node_id);
    }
    static_cast<void>(set_screen_targets(targets));
    log::info(log_component, "{} screen{}, input through {}", screens_.size(), screens_.size() == 1 ? "" : "s",
              ei_ ? "libei" : "the portal");
    return {};
}

void PortalDesktop::request_screen_sizes(std::span<const std::pair<std::uint32_t, std::uint32_t>> sizes)
{
    if (!virtual_monitor_) {
        return;
    }
    for (std::size_t i = 0; i < std::min(sizes.size(), screens_.size()); ++i) {
        const auto [width, height] = sizes[i];
        if (width > 0 && height > 0) {
            screens_[i].capture->request_size(width, height);
        }
    }
}

bool PortalDesktop::set_screen_targets(std::span<const std::optional<platform::Rect>> targets)
{
    if (ei_) {
        std::vector<portal::EiInput::Output> outputs;
        for (std::size_t i = 0; i < std::min(targets.size(), screens_.size()); ++i) {
            if (targets[i]) {
                outputs.push_back(portal::EiInput::Output{*targets[i], screens_[i].stream.mapping_id});
            }
        }
        ei_->set_outputs(std::move(outputs));
        return true;
    }
    if (notify_) {
        std::vector<portal::StreamRegion> regions;
        for (std::size_t i = 0; i < std::min(targets.size(), screens_.size()); ++i) {
            if (!targets[i]) {
                continue;
            }
            const auto& stream = screens_[i].stream;
            const auto [width, height] = screens_[i].capture->frames().size();
            portal::StreamRegion region{stream.node_id, *targets[i], static_cast<std::int32_t>(width),
                                        static_cast<std::int32_t>(height)};
            if (stream.size) {
                // NotifyPointerMotionAbsolute takes the stream's logical coordinates.
                region.logical_width = stream.size->first;
                region.logical_height = stream.size->second;
            }
            regions.push_back(region);
        }
        notify_->set_layout(std::move(regions));
        return true;
    }
    return false;
}

Result<void> PortalDesktop::start_session(portal::PortalOptions& options)
{
    log::info(log_component, "{}",
              options.restore_token ? "reconnecting to the desktop portal with the stored permission"
                                    : "asking the desktop portal for screen sharing and remote control: "
                                      "confirm the dialog on the desktop");
    auto started = session_->start(options);
    if (!started && options.restore_token) {
        // A stored permission goes stale when the monitors change, and the
        // portal may then grant nothing: ask again.
        log::warn(log_component, "the desktop portal did not restore the stored permission ({}); asking again",
                  started.error().message);
        options.restore_token.reset();
        session_ = std::make_unique<portal::PortalSession>();
        log::info(log_component, "asking the desktop portal for screen sharing and remote control: "
                                 "confirm the dialog on the desktop");
        started = session_->start(options);
    }
    if (!started) {
        log::error(log_component, "desktop portal: {} ({})", started.error().message,
                   portal::to_string(started.error().code));
        return fail(Errc::io, "the desktop portal did not start screen sharing");
    }
    return {};
}

void PortalDesktop::start_clipboard()
{
    if (!session_->clipboard_enabled()) {
        log::warn(log_component, "{}",
                  session_->capabilities().clipboard_version == 0
                      ? "the desktop portal has no Clipboard interface: no clipboard"
                      : "the desktop portal did not grant clipboard access: no clipboard (a permission stored "
                        "without it is restored that way; remove the restore token to be asked again)");
        return;
    }
    auto clipboard = portal::PortalClipboard::create(*session_);
    if (!clipboard) {
        log::warn(log_component, "no clipboard: {}", clipboard.error().message);
        return;
    }
    clipboard_ = std::move(*clipboard);
    log::info(log_component, "sharing the clipboard through the portal");
}

Result<void> PortalDesktop::connect_input()
{
    auto eis = session_->connect_to_eis();
    if (!eis) {
        // ConnectToEIS failed outright; Notify* still works then.
        log::warn(log_component, "ConnectToEIS: {}; input goes through the portal", eis.error().message);
    } else if (*eis) {
        auto ei = portal::EiInput::connect_fd((*eis)->release());
        if (!ei) {
            // Once ConnectToEIS succeeded the portal refuses Notify*: no input.
            log::error(log_component, "libei: {}", ei.error().message());
            return fail(Errc::io, "cannot connect to the compositor's input (libei)");
        }
        ei_ = std::move(*ei);
        return {};
    }
    notify_ = std::make_unique<portal::PortalNotifyInput>(*session_, portal::default_layout(session_->streams()));
    return {};
}

Result<void> PortalDesktop::wait_for_first_frames()
{
    const auto deadline = Clock::now() + first_frame_timeout;
    for (const auto& screen : screens_) {
        while (screen.capture->frames().size().first == 0) {
            if (closed()) {
                log::error(log_component, "the screen cast ended before its first frame: {}", screen.capture->error());
                return fail(Errc::io, "the screen cast stream closed");
            }
            if (Clock::now() > deadline) {
                return fail(Errc::io, "no frame from the screen cast stream");
            }
            pollfd pfd{screen.capture->frames().wake_fd(), POLLIN, 0};
            ::poll(&pfd, 1, 100);
            dispatch();
        }
    }
    return {};
}

}  // namespace

Result<std::unique_ptr<Desktop>> start_portal_desktop(const PortalDesktopOptions& options)
{
    auto desktop = std::make_unique<PortalDesktop>();
    FARLAND_TRY_VOID(desktop->start(options));
    return std::unique_ptr<Desktop>(std::move(desktop));
}

}  // namespace farland::app
