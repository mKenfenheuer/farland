// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>
#include <farland/base/unique_fd.hpp>
#include <farland/platform/wlroots/wayland/shm.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

namespace farland::platform::wayland {

namespace {

constexpr std::string_view log_component = "platform.wayland";
/// 16384 x 16384 pixels of 4 bytes: beyond anything a compositor offers.
constexpr std::size_t max_buffer_size = std::size_t{1} << 30;

}  // namespace

Result<std::unique_ptr<ShmBuffer>> ShmBuffer::create(wl_shm* shm, std::uint32_t width, std::uint32_t height,
                                                     std::uint32_t stride, std::uint32_t format)
{
    const std::size_t size = std::size_t{stride} * height;
    if (width == 0 || height == 0 || stride < std::size_t{width} * 4 || size > max_buffer_size) {
        return fail(Errc::invalid_value, "invalid shared-memory buffer size");
    }
    const UniqueFd fd(::memfd_create("farland-wayland-buffer", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (!fd.valid() || ::ftruncate(fd.get(), static_cast<off_t>(size)) != 0) {
        log::error(log_component, "cannot create a {}-byte buffer: {}", size, std::strerror(errno));
        return fail(Errc::io, "cannot create a shared-memory buffer");
    }
    // The size is fixed from now on; the compositor may rely on that.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    ::fcntl(fd.get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
    void* map = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
    if (map == MAP_FAILED) {
        log::error(log_component, "cannot map a {}-byte buffer: {}", size, std::strerror(errno));
        return fail(Errc::io, "cannot map a shared-memory buffer");
    }
    // libwayland duplicates the descriptor when it queues the request.
    wl_shm_pool* pool = wl_shm_create_pool(shm, fd.get(), static_cast<std::int32_t>(size));
    wl_buffer* buffer =
        wl_shm_pool_create_buffer(pool, 0, static_cast<std::int32_t>(width), static_cast<std::int32_t>(height),
                                  static_cast<std::int32_t>(stride), format);
    wl_shm_pool_destroy(pool);
    return std::unique_ptr<ShmBuffer>(
        new ShmBuffer(buffer, std::span(static_cast<std::byte*>(map), size), width, height, stride, format));
}

ShmBuffer::ShmBuffer(wl_buffer* buffer, std::span<std::byte> data, std::uint32_t width, std::uint32_t height,
                     std::uint32_t stride, std::uint32_t format) noexcept
    : buffer_(buffer), data_(data), width_(width), height_(height), stride_(stride), format_(format)
{
}

ShmBuffer::~ShmBuffer()
{
    wl_buffer_destroy(buffer_);
    ::munmap(data_.data(), data_.size());
}

}  // namespace farland::platform::wayland
