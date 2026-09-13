// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/codec/image.hpp>
#include <farland/proto/input.hpp>

#include <cstdint>
#include <string>
#include <vector>

/// The `test` platform backend (docs/PLAN.md §3.3): a synthetic desktop for
/// development and interop tests. It draws color bars, a gradient and a
/// bouncing square, and reflects input: a crosshair follows the pointer, the
/// crosshair turns red while a button is held, and a strip of cells shows the
/// last keys pressed.
namespace farland::server {

class TestPattern {
public:
    TestPattern(std::uint32_t width, std::uint32_t height);

    void resize(std::uint32_t width, std::uint32_t height);
    /// Draws frame number `frame` and returns a view of it (valid until the next call).
    [[nodiscard]] codec::ImageView render(std::uint64_t frame);
    void apply(const proto::InputEvent& event);

    /// Human-readable summary of an input event, for the input log.
    [[nodiscard]] static std::string describe(const proto::InputEvent& event);

private:
    void fill(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h, std::uint32_t rgb);

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<std::byte> pixels_;
    std::uint32_t pointer_x_ = 0;
    std::uint32_t pointer_y_ = 0;
    bool button_down_ = false;
    std::vector<std::uint16_t> recent_keys_;
};

}  // namespace farland::server
