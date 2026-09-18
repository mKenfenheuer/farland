// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/platform/kwin/wayland_connection.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
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

    /// True when the output named `name` takes custom modes.
    [[nodiscard]] bool resizable(std::string_view name) const;
    /// The size of the output's current mode; nullopt for an unknown output.
    [[nodiscard]] std::optional<std::pair<std::int32_t, std::int32_t>> size(std::string_view name) const;
    /// Resizes the output named `name` (asynchronously; later calls replace
    /// the size asked for). Unknown or fixed outputs are left alone.
    void request_size(std::string_view name, std::int32_t width, std::int32_t height);
    /// True while a resize is under way.
    [[nodiscard]] bool busy() const;
    /// Gives up on resizes KWin never finished; call it now and then.
    void check_timeouts();

    struct Mode;
    struct Device;
    struct Listeners;

private:
    OutputManagement(WaylandConnection& connection, kde_output_management_v2* manager) noexcept;
    void add_device(const WaylandGlobal& global);
    void remove_device(const WaylandGlobal& global);
    [[nodiscard]] Device* find(std::string_view name) const;
    /// Takes the next step of `device`'s resize.
    void advance(Device& device);
    void configuration_done(Device& device, bool applied);

    WaylandConnection& connection_;
    kde_output_management_v2* manager_ = nullptr;
    std::vector<std::unique_ptr<Device>> devices_;
};

}  // namespace farland::platform::kwin
