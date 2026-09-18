// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/platform/kwin/wayland_connection.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct kde_mode_list_v2;
struct kde_output_configuration_v2;
struct kde_output_device_mode_v2;
struct kde_output_device_v2;
struct kde_output_management_v2;

/// Output sizes through KWin's output management (kde_output_device_v2 and
/// kde_output_management_v2, plasma-wayland-protocols): an output that
/// takes custom modes (KWin's virtual outputs do) gets a mode of the size
/// asked for, then switches to it. KWin then renegotiates its screen cast
/// streams to the new size.
namespace farland::platform::kwin {

class OutputManagement {
public:
    /// kde_output_configuration_v2.set_custom_modes arrived in version 18.
    static constexpr std::uint32_t custom_modes_version = 18;

    /// Binds the manager and every output device. Fails when KWin offers
    /// no kde_output_management_v2 of version 18 or later.
    [[nodiscard]] static Result<std::unique_ptr<OutputManagement>> create(WaylandConnection& connection);

    OutputManagement(const OutputManagement&) = delete;
    OutputManagement& operator=(const OutputManagement&) = delete;
    OutputManagement(OutputManagement&&) = delete;
    OutputManagement& operator=(OutputManagement&&) = delete;
    ~OutputManagement();

    /// One output as a layout wants it.
    struct LayoutEntry {
        std::string name;
        bool enabled = false;
        std::int32_t x = 0;
        std::int32_t y = 0;
        /// 1 for the primary output, then upwards; 0 for a disabled one.
        std::uint32_t priority = 0;
    };

    /// True when the output named `name` takes custom modes.
    [[nodiscard]] bool resizable(std::string_view name) const;
    /// True when the output named `name` is one KWin knows and shows.
    [[nodiscard]] bool enabled(std::string_view name) const;
    /// The size of the output's current mode; nullopt for an unknown output.
    [[nodiscard]] std::optional<std::pair<std::int32_t, std::int32_t>> size(std::string_view name) const;
    /// Resizes the output named `name` (asynchronously; later calls replace
    /// the size asked for). Unknown or fixed outputs are left alone.
    void request_size(std::string_view name, std::int32_t width, std::int32_t height);
    /// Makes `outputs` the screens the session is laid out around, in that
    /// order: each is enabled and the first is the primary one, which is the
    /// screen the panel and new windows go to; they sit side by side, beside
    /// the outputs this does not name. Asynchronous, like request_size();
    /// asking again replaces what was asked for, and KWin is asked again
    /// whenever the outputs change until the layout is what was asked for.
    ///
    /// The outputs not named are left switched on, although a client holding
    /// the session sees nothing of them: KWin refuses every output
    /// configuration while the session is not active on its seat ("Atomic
    /// modeset test failed! Permission denied"), so a screen switched off on
    /// the way out could not be switched on again from here, and KWin
    /// remembers the configurations it applies. The screen at the machine is
    /// kept from showing the session by the display manager's login screen
    /// instead.
    ///
    /// The layout that was there when this is first called is remembered, so
    /// that restore_layout() can put it back.
    void request_layout(std::span<const std::string> outputs);
    /// Puts back the layout that was there before the first request_layout(),
    /// and forgets it. Blocks until KWin applied it or `timeout` passed,
    /// because it is what a desktop does on its way out.
    void restore_layout(std::chrono::milliseconds timeout);
    /// True while a resize or a layout is under way.
    [[nodiscard]] bool busy() const;
    /// Gives up on resizes KWin never finished, and takes the next step of a
    /// layout; call it now and then.
    void check_timeouts();

    struct Mode;
    struct Device;
    struct Layout;
    struct Listeners;

private:
    OutputManagement(WaylandConnection& connection, kde_output_management_v2* manager) noexcept;
    void add_device(const WaylandGlobal& global);
    void remove_device(const WaylandGlobal& global);
    [[nodiscard]] Device* find(std::string_view name) const;
    /// Takes the next step of `device`'s resize.
    void advance(Device& device);
    void configuration_done(Device& device, bool applied);
    /// Every output as the layout wants it, empty while KWin has not
    /// announced all of them yet.
    [[nodiscard]] std::vector<LayoutEntry> layout_target() const;
    /// True when the outputs already are as the layout asks.
    [[nodiscard]] bool layout_reached() const;
    /// Sends the layout KWin is not showing yet.
    void apply_layout();
    void layout_done(bool applied);

    WaylandConnection& connection_;
    kde_output_management_v2* manager_ = nullptr;
    std::vector<std::unique_ptr<Device>> devices_;
    std::unique_ptr<Layout> layout_;
};

}  // namespace farland::platform::kwin
