// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "wlroots_headless.hpp"

#include <farland/base/log.hpp>
#include <farland/platform/wlroots/compositor.hpp>
#include <farland/platform/wlroots/output_capture.hpp>
#include <farland/platform/wlroots/outputs.hpp>
#include <farland/platform/wlroots/virtual_input.hpp>
#include <farland/platform/wlroots/wayland/connection.hpp>
#include <farland/platform/wlroots/wayland/data_control.hpp>

#include <algorithm>
#include <ext-image-capture-source-v1-client-protocol.h>
#include <ext-image-copy-capture-v1-client-protocol.h>
#include <format>
#include <wayland-client.h>
#include <wlr-screencopy-unstable-v1-client-protocol.h>

namespace farland::app {

namespace {

namespace wlroots = platform::wlroots;
namespace wayland = platform::wayland;
using Clock = std::chrono::steady_clock;
constexpr std::string_view log_component = "app.wlroots";
constexpr auto resize_timeout = std::chrono::seconds(5);

class WlrootsDesktop final : public Desktop {
public:
    WlrootsDesktop() = default;
    WlrootsDesktop(const WlrootsDesktop&) = delete;
    WlrootsDesktop& operator=(const WlrootsDesktop&) = delete;
    WlrootsDesktop(WlrootsDesktop&&) = delete;
    WlrootsDesktop& operator=(WlrootsDesktop&&) = delete;
    ~WlrootsDesktop() override
    {
        // Wayland objects before the connection, the connection before the compositor.
        clipboard_.reset();
        screens_.clear();
        input_.reset();
        if (pointer_ != nullptr) {
            wl_pointer_release(pointer_);
        }
        if (seat_ != nullptr) {
            wl_seat_release(seat_);
        }
        destroy_globals();
        outputs_.reset();
        connection_.reset();
        process_.reset();
    }

    [[nodiscard]] Result<void> start(const HeadlessOptions& options);

    [[nodiscard]] platform::FrameSource& frames() override { return screens_.front().capture->frames(); }
    [[nodiscard]] platform::CursorSource* cursor() override { return screens_.front().capture->cursor(); }
    [[nodiscard]] platform::InputSink& input() override { return *input_; }
    [[nodiscard]] std::vector<int> dispatch_fds() const override { return {connection_->fd()}; }
    void dispatch() override
    {
        connection_->dispatch();
        if (outputs_->generation() != outputs_generation_) {
            update_input_layout();
        }
    }
    [[nodiscard]] bool closed() const override
    {
        return connection_->closed() || (process_ && process_->exited()) ||
               std::ranges::any_of(screens_, [](const Screen& s) { return s.capture->closed(); });
    }
    [[nodiscard]] std::size_t screen_count() const override { return screens_.size(); }
    [[nodiscard]] platform::FrameSource& screen_frames(std::size_t index) override
    {
        return screens_.at(index).capture->frames();
    }
    [[nodiscard]] platform::CursorSource* screen_cursor(std::size_t index) override
    {
        return screens_.at(index).capture->cursor();
    }
    [[nodiscard]] bool resizable() const override { return process_ != nullptr && outputs_->can_resize(); }
    void request_screen_sizes(std::span<const std::pair<std::uint32_t, std::uint32_t>> sizes) override
    {
        if (!resizable()) {
            return;
        }
        std::vector<std::pair<std::string, std::pair<std::uint32_t, std::uint32_t>>> wanted;
        for (std::size_t i = 0; i < std::min(sizes.size(), screens_.size()); ++i) {
            if (sizes[i].first > 0 && sizes[i].second > 0) {
                wanted.emplace_back(screens_[i].name, sizes[i]);
            }
        }
        outputs_->request_sizes(std::move(wanted));
    }
    bool set_screen_targets(std::span<const std::optional<platform::Rect>> targets) override
    {
        targets_.assign(targets.begin(), targets.end());
        update_input_layout();
        return true;
    }
    [[nodiscard]] platform::Clipboard* clipboard() override { return clipboard_.get(); }

private:
    struct Screen {
        std::string name;
        std::unique_ptr<wlroots::OutputCapture> capture;
    };

    [[nodiscard]] Result<void> connect(const HeadlessOptions& options);
    [[nodiscard]] Result<void> start_captures(const HeadlessOptions& options);
    void destroy_globals();
    void update_input_layout();

    std::unique_ptr<wlroots::CompositorProcess> process_;
    std::unique_ptr<wayland::Connection> connection_;
    std::unique_ptr<wlroots::Outputs> outputs_;
    std::uint64_t outputs_generation_ = 0;
    wl_seat* seat_ = nullptr;
    wl_pointer* pointer_ = nullptr;
    wlroots::OutputCapture::Protocols protocols_;
    std::unique_ptr<wlroots::VirtualInput> input_;
    std::vector<Screen> screens_;
    std::vector<std::optional<platform::Rect>> targets_;
    std::unique_ptr<platform::Clipboard> clipboard_;
};

wlroots::CompositorKind compositor_kind(HeadlessKind kind)
{
    switch (kind) {
    case HeadlessKind::labwc:
        return wlroots::CompositorKind::labwc;
    case HeadlessKind::cage:
        return wlroots::CompositorKind::cage;
    default:
        return wlroots::CompositorKind::sway;
    }
}

Result<void> WlrootsDesktop::connect(const HeadlessOptions& options)
{
    const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(options.timeout);
    if (!options.attach) {
        FARLAND_TRY(process_, wlroots::CompositorProcess::launch(wlroots::CompositorLaunch{
                                  .kind = compositor_kind(options.kind),
                                  .command = options.cage_command,
                                  .outputs = 1,
                                  .render_node = options.render_node,
                                  .timeout = timeout,
                              }));
    }
    FARLAND_TRY(connection_, wayland::Connection::connect(process_ ? process_->socket_path() : std::string(), timeout));
    const auto* seat = connection_->find("wl_seat");
    const auto* shm = connection_->find("wl_shm");
    if (seat == nullptr || shm == nullptr) {
        return fail(Errc::unsupported, "the compositor has no seat or no shared memory");
    }
    seat_ = connection_->bind<wl_seat>(*seat, &wl_seat_interface, 5);
    protocols_.shm = connection_->bind<wl_shm>(*shm, &wl_shm_interface, 1);
    const auto* image_copy = connection_->find("ext_image_copy_capture_manager_v1");
    const auto* sources = connection_->find("ext_output_image_capture_source_manager_v1");
    if (image_copy != nullptr && sources != nullptr) {
        protocols_.image_copy = connection_->bind<ext_image_copy_capture_manager_v1>(
            *image_copy, &ext_image_copy_capture_manager_v1_interface, 1);
        protocols_.output_sources = connection_->bind<ext_output_image_capture_source_manager_v1>(
            *sources, &ext_output_image_capture_source_manager_v1_interface, 1);
    } else if (const auto* screencopy = connection_->find("zwlr_screencopy_manager_v1")) {
        protocols_.screencopy =
            connection_->bind<zwlr_screencopy_manager_v1>(*screencopy, &zwlr_screencopy_manager_v1_interface, 3);
    } else {
        return fail(Errc::unsupported, "the compositor has no screen capture protocol");
    }
    FARLAND_TRY(outputs_, wlroots::Outputs::create(*connection_));
    FARLAND_TRY(input_, wlroots::VirtualInput::create(*connection_, seat_, options.keymap_layout));
    return {};
}

Result<void> WlrootsDesktop::start_captures(const HeadlessOptions& options)
{
    if (protocols_.image_copy != nullptr) {
        // The virtual pointer made the seat a pointer seat; its wl_pointer
        // names the cursor to capture.
        if (!connection_->roundtrip(std::chrono::seconds(5))) {
            return fail(Errc::io, "the compositor stopped answering");
        }
        pointer_ = wl_seat_get_pointer(seat_);
    }
    const auto& outputs = outputs_->list();
    for (const std::size_t index : wlroots::screen_order(outputs)) {
        const auto& output = outputs[index];
        FARLAND_TRY(auto capture,
                    wlroots::OutputCapture::create(*connection_, output.output, pointer_, protocols_, output.name));
        screens_.push_back(Screen{output.name, std::move(capture)});
    }
    if (screens_.empty()) {
        return fail(Errc::unsupported, "the compositor has no enabled output");
    }
    const auto deadline = Clock::now() + options.timeout;
    const bool framed = connection_->wait_until(
        [this] {
            return closed() ||
                   std::ranges::all_of(screens_, [](const Screen& s) { return s.capture->frames().size().first != 0; });
        },
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()));
    if (!framed || closed()) {
        for (const auto& s : screens_) {
            if (s.capture->closed()) {
                log::error(log_component, "capture of {}: {}", s.name, s.capture->error());
            }
        }
        if (process_) {
            log::error(log_component, "the compositor's log:\n{}", process_->log_tail());
        }
        return fail(Errc::io, "no frame from the compositor");
    }
    return {};
}

Result<void> WlrootsDesktop::start(const HeadlessOptions& options)
{
    FARLAND_TRY_VOID(connect(options));
    if (process_ && options.width > 0 && options.height > 0) {
        const auto order = wlroots::screen_order(outputs_->list());
        if (!outputs_->can_resize()) {
            log::warn(log_component, "the compositor has no output management: its output keeps its size");
        } else if (!order.empty()) {
            const auto& first = outputs_->list()[order.front()];
            outputs_->request_sizes({{first.name, {options.width, options.height}}});
            const std::string name = first.name;
            if (!connection_->wait_until(
                    [&] {
                        const auto& list = outputs_->list();
                        const auto it = std::ranges::find(list, name, &wlroots::Output::name);
                        return !outputs_->resizing() && it != list.end() &&
                               std::pair(it->width, it->height) == std::pair(options.width, options.height);
                    },
                    resize_timeout)) {
                log::warn(log_component, "the compositor did not resize {} to {}x{}", name, options.width,
                          options.height);
            }
        }
    }
    FARLAND_TRY_VOID(start_captures(options));
    clipboard_ = wayland::create_data_control_clipboard(*connection_, seat_);
    update_input_layout();

    const auto kind = wlroots::to_string(compositor_kind(options.kind));
    for (const auto& s : screens_) {
        const auto [width, height] = s.capture->frames().size();
        log::info(log_component, "{} {}: screen {} {}x{} through {}{}", options.attach ? "attached to" : "headless",
                  kind, s.name, width, height, s.capture->protocol(),
                  s.capture->cursor() != nullptr ? "" : ", cursor in the picture");
    }
    if (!clipboard_) {
        log::warn(log_component, "the compositor has no data-control protocol: no clipboard");
    }
    return {};
}

void WlrootsDesktop::destroy_globals()
{
    if (protocols_.image_copy != nullptr) {
        ext_image_copy_capture_manager_v1_destroy(protocols_.image_copy);
    }
    if (protocols_.output_sources != nullptr) {
        ext_output_image_capture_source_manager_v1_destroy(protocols_.output_sources);
    }
    if (protocols_.screencopy != nullptr) {
        zwlr_screencopy_manager_v1_destroy(protocols_.screencopy);
    }
    if (protocols_.shm != nullptr) {
        wl_shm_destroy(protocols_.shm);
    }
    protocols_ = {};
}

/// Each screen's target on the client's desktop onto its output in the
/// layout. Screens without a target take none; before the session placed
/// them, positions are layout pixels.
void WlrootsDesktop::update_input_layout()
{
    outputs_generation_ = outputs_->generation();
    const auto& outputs = outputs_->list();
    std::vector<wlroots::ScreenMapping> mappings;
    for (std::size_t i = 0; i < screens_.size(); ++i) {
        const auto output = std::ranges::find(outputs, screens_[i].name, &wlroots::Output::name);
        if (output == outputs.end()) {
            continue;
        }
        if (targets_.empty()) {
            mappings.push_back(wlroots::ScreenMapping{output->logical, output->logical});
        } else if (i < targets_.size() && targets_[i]) {
            mappings.push_back(wlroots::ScreenMapping{*targets_[i], output->logical});
        }
    }
    input_->set_layout(std::move(mappings), wlroots::layout_box(outputs));
}

}  // namespace

Result<std::unique_ptr<Desktop>> start_wlroots_headless(const HeadlessOptions& options)
{
    if (options.kind != HeadlessKind::sway && options.kind != HeadlessKind::labwc &&
        options.kind != HeadlessKind::cage) {
        return fail(Errc::invalid_value, "not a wlroots compositor");
    }
    auto desktop = std::make_unique<WlrootsDesktop>();
    FARLAND_TRY_VOID(desktop->start(options));
    return std::unique_ptr<Desktop>(std::move(desktop));
}

}  // namespace farland::app
