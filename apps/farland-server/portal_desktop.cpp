// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "portal_desktop.hpp"

#include <farland/base/log.hpp>
#include <farland/platform/portal/ei_input.hpp>
#include <farland/platform/portal/pipewire_capture.hpp>
#include <farland/platform/portal/portal_input.hpp>
#include <farland/platform/portal/portal_session.hpp>

#include <poll.h>

namespace farland::app {

namespace {

namespace portal = platform::portal;
using Clock = std::chrono::steady_clock;
constexpr std::string_view log_component = "app.portal";
/// How long the compositor may take to deliver the first frame.
constexpr auto first_frame_timeout = std::chrono::seconds(10);

/// A permission for existing monitors does not restore a virtual one and the
/// other way round, so each mode keeps its own token.
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

class PortalDesktop final : public Desktop {
public:
    [[nodiscard]] Result<void> start(const PortalDesktopOptions& options);

    [[nodiscard]] platform::FrameSource& frames() override { return capture_->frames(); }
    [[nodiscard]] platform::CursorSource* cursor() override { return &capture_->cursor(); }
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
        return session_->closed() || !capture_ || capture_->closed() || (ei_ && ei_->closed());
    }

private:
    [[nodiscard]] Result<void> start_session(portal::PortalOptions& options);
    [[nodiscard]] Result<void> connect_input();
    [[nodiscard]] Result<void> wait_for_first_frame();

    // Declared first, destroyed last: the capture and the input need the session.
    std::unique_ptr<portal::PortalSession> session_ = std::make_unique<portal::PortalSession>();
    std::unique_ptr<portal::PipeWireCapture> capture_;
    std::unique_ptr<portal::EiInput> ei_;
    std::unique_ptr<portal::PortalNotifyInput> notify_;
};

Result<void> PortalDesktop::start(const PortalDesktopOptions& options)
{
    portal::PortalOptions portal_options;
    portal_options.virtual_monitor = options.virtual_monitor;
    portal_options.monitors = !options.virtual_monitor;
    portal_options.multiple = false;  // one monitor; multi-monitor comes with the disp channel (M6)
    portal_options.timeout = options.timeout;

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

    const auto& streams = session_->streams();  // start() fails without any
    const auto& stream = streams.front();
    if (streams.size() > 1) {
        log::info(log_component, "the portal offers {} streams; sharing the first", streams.size());
    }
    auto remote = session_->open_pipewire_remote();
    if (!remote) {
        log::error(log_component, "OpenPipeWireRemote: {}", remote.error().message);
        return fail(Errc::io, "the desktop portal gave no PipeWire connection");
    }
    auto capture = portal::PipeWireCapture::create(remote->get(), stream.node_id);
    if (!capture) {
        log::error(log_component, "PipeWire capture of node {}: {}", stream.node_id, capture.error().message());
        return fail(Errc::io, "cannot capture the screen cast stream");
    }
    capture_ = std::move(*capture);
    FARLAND_TRY_VOID(connect_input());
    FARLAND_TRY_VOID(wait_for_first_frame());

    const auto [width, height] = capture_->frames().size();
    if (ei_) {
        ei_->set_outputs({{platform::Rect{0, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)},
                           stream.mapping_id}});
    }
    log::info(log_component, "sharing {}x{} (PipeWire node {}), input through {}", width, height, stream.node_id,
              ei_ ? "libei" : "the portal");
    return {};
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

Result<void> PortalDesktop::wait_for_first_frame()
{
    const auto deadline = Clock::now() + first_frame_timeout;
    while (capture_->frames().size().first == 0) {
        if (closed()) {
            log::error(log_component, "the screen cast ended before its first frame: {}", capture_->error());
            return fail(Errc::io, "the screen cast stream closed");
        }
        if (Clock::now() > deadline) {
            return fail(Errc::io, "no frame from the screen cast stream");
        }
        pollfd pfd{capture_->frames().wake_fd(), POLLIN, 0};
        ::poll(&pfd, 1, 100);
        dispatch();
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
