// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct gbm_bo;

/// CPU access to dmabufs whose modifier is not LINEAR (LINEAR ones are simply
/// mmapped by the capture). Uses GBM to import and map, and EGL to ask the
/// driver which modifiers it can import. Both are optional build dependencies:
/// without them DmabufReader::open() returns nullptr and the capture offers
/// only LINEAR dmabufs and shared memory.
namespace farland::platform::portal {

/// One dmabuf image as PipeWire describes it (spa_data per plane).
struct DmabufImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t drm_format = 0;
    std::uint64_t modifier = 0;
    std::uint32_t planes = 0;
    std::array<int, 4> fds{-1, -1, -1, -1};
    std::array<std::uint32_t, 4> strides{};
    std::array<std::uint32_t, 4> offsets{};
};

class DmabufReader {
public:
    /// A mapped image; unmapped when destroyed.
    class Mapping {
    public:
        Mapping(gbm_bo* bo, void* map_data, std::span<const std::byte> pixels, std::size_t stride) noexcept;
        Mapping(Mapping&& other) noexcept;
        Mapping& operator=(Mapping&&) = delete;
        Mapping(const Mapping&) = delete;
        Mapping& operator=(const Mapping&) = delete;
        ~Mapping();

        /// The pixels, starting at (0, 0).
        [[nodiscard]] std::span<const std::byte> pixels() const noexcept { return pixels_; }
        [[nodiscard]] std::size_t stride() const noexcept { return stride_; }

    private:
        gbm_bo* bo_;
        void* map_data_;
        std::span<const std::byte> pixels_;
        std::size_t stride_;
    };

    /// Opens `render_node` (empty: the first /dev/dri/renderD* that works).
    /// nullptr if farland was built without GBM and EGL or no GPU is usable.
    [[nodiscard]] static std::unique_ptr<DmabufReader> open(const std::string& render_node);

    DmabufReader(const DmabufReader&) = delete;
    DmabufReader& operator=(const DmabufReader&) = delete;
    DmabufReader(DmabufReader&&) = delete;
    DmabufReader& operator=(DmabufReader&&) = delete;
    ~DmabufReader();

    /// The modifiers the GPU imports for `drm_format` (not external-only),
    /// LINEAR included if the driver lists it.
    [[nodiscard]] std::vector<std::uint64_t> modifiers(std::uint32_t drm_format) const;
    /// Imports and maps `image` for reading; nullopt if the driver refuses.
    [[nodiscard]] std::optional<Mapping> map(const DmabufImage& image) const;

private:
    struct State;
    explicit DmabufReader(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

}  // namespace farland::platform::portal
