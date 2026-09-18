// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/platform/backend.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct wl_output;

namespace farland::platform::wayland {
class Connection;
}

namespace farland::platform::wlroots {

/// One output (monitor) of the compositor.
struct Output {
    std::uint32_t global = 0;
    wl_output* output = nullptr;
    std::string name;
    /// The current mode, in pixels (what a capture delivers).
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    /// Where it lies in the compositor's layout, in logical pixels.
    Rect logical;
    bool enabled = true;
    /// False once the compositor removed it.
    bool present = true;
};

/// Laid out left to right, then top to bottom: the order screens go in.
[[nodiscard]] std::vector<std::size_t> screen_order(const std::vector<Output>& outputs);
/// The bounding box of the enabled outputs in the layout.
[[nodiscard]] Rect layout_box(const std::vector<Output>& outputs);

/// The compositor's outputs: every wl_output with its name and mode, and,
/// through zwlr_output_manager_v1 where the compositor has it (sway, labwc
/// and cage all do), their place in the layout and new sizes. A headless
/// output takes any custom mode, so resizing a screen is one configuration
/// with that mode, which also lines the resized outputs up left to right.
class Outputs {
public:
    /// Binds the outputs and the output manager and waits for their state.
    [[nodiscard]] static Result<std::unique_ptr<Outputs>> create(wayland::Connection& connection);
    Outputs(const Outputs&) = delete;
    Outputs& operator=(const Outputs&) = delete;
    Outputs(Outputs&&) = delete;
    Outputs& operator=(Outputs&&) = delete;
    ~Outputs();

    [[nodiscard]] const std::vector<Output>& list() const noexcept;
    /// Changes whenever an output's mode, place or presence changed.
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] bool can_resize() const noexcept;

    /// Sizes for the outputs named, which go side by side in this order.
    /// Sent as soon as the compositor's state is current; a later call
    /// replaces one that was not sent yet.
    void request_sizes(std::vector<std::pair<std::string, std::pair<std::uint32_t, std::uint32_t>>> sizes);
    /// A configuration is waiting or on its way.
    [[nodiscard]] bool resizing() const noexcept;

    struct Impl;

private:
    explicit Outputs(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace farland::platform::wlroots
