// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

struct wl_buffer;
struct wl_shm;

namespace farland::platform::wayland {

/// wl_shm format codes (wayland.xml), the ones farland reads: both are
/// B, G, R, X/A in memory.
namespace shm_format {
inline constexpr std::uint32_t argb8888 = 0;
inline constexpr std::uint32_t xrgb8888 = 1;
}  // namespace shm_format

/// A wl_buffer in shared memory (a sealed memfd), mapped for reading and
/// writing.
class ShmBuffer {
public:
    [[nodiscard]] static Result<std::unique_ptr<ShmBuffer>>
    create(wl_shm* shm, std::uint32_t width, std::uint32_t height, std::uint32_t stride, std::uint32_t format);
    ShmBuffer(const ShmBuffer&) = delete;
    ShmBuffer& operator=(const ShmBuffer&) = delete;
    ShmBuffer(ShmBuffer&&) = delete;
    ShmBuffer& operator=(ShmBuffer&&) = delete;
    ~ShmBuffer();

    [[nodiscard]] wl_buffer* buffer() const noexcept { return buffer_; }
    [[nodiscard]] std::span<std::byte> data() noexcept { return data_; }
    [[nodiscard]] std::span<const std::byte> data() const noexcept { return data_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint32_t stride() const noexcept { return stride_; }
    [[nodiscard]] std::uint32_t format() const noexcept { return format_; }

private:
    ShmBuffer(wl_buffer* buffer, std::span<std::byte> data, std::uint32_t width, std::uint32_t height,
              std::uint32_t stride, std::uint32_t format) noexcept;

    wl_buffer* buffer_;
    std::span<std::byte> data_;
    std::uint32_t width_;
    std::uint32_t height_;
    std::uint32_t stride_;
    std::uint32_t format_;
};

}  // namespace farland::platform::wayland
