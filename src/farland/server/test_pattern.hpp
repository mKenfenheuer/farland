// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/channels/rdpei_server.hpp>
#include <farland/codec/image.hpp>
#include <farland/proto/input.hpp>

#include <cstdint>
#include <string>
#include <vector>

/// The `test` platform backend (docs/PLAN.md §3.3): a synthetic desktop for
/// development and interop tests. It draws color bars, a gradient and a
/// bouncing square, and reflects input: a crosshair follows the pointer, the
/// crosshair turns red while a button is held, a strip of cells shows the
/// last keys pressed, and touch and pen contacts show as squares (filled
/// while they touch, small while they hover).
namespace farland::server {

class TestPattern {
public:
    TestPattern(std::uint32_t width, std::uint32_t height);

    void resize(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    /// Draws frame number `frame` and returns a view of it (valid until the next call).
    [[nodiscard]] codec::ImageView render(std::uint64_t frame);
    void apply(const proto::InputEvent& event);
    void apply(const channels::rdpei::Contact& contact);

    /// Human-readable summary of an input event, for the input log.
    [[nodiscard]] static std::string describe(const proto::InputEvent& event);
    [[nodiscard]] static std::string describe(const channels::rdpei::Contact& contact);

private:
    struct Point {
        channels::rdpei::ContactKind kind{};
        std::uint8_t id = 0;
        std::uint32_t x = 0;
        std::uint32_t y = 0;
        bool engaged = false;
    };

    void fill(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h, std::uint32_t rgb);
    /// A square of `size` centred on (x, y), clipped to the picture.
    void square(std::uint32_t x, std::uint32_t y, std::uint32_t size, std::uint32_t rgb);

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<std::byte> pixels_;
    std::uint32_t pointer_x_ = 0;
    std::uint32_t pointer_y_ = 0;
    bool button_down_ = false;
    std::vector<std::uint16_t> recent_keys_;
    std::vector<Point> contacts_;  ///< active (hovering or engaged)
};

}  // namespace farland::server
