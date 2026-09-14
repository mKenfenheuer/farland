// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/platform/backend.hpp>
#include <farland/proto/pointer.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace farland::server {

struct Session;

/// Turns the platform's cursor updates into RDP pointer updates
/// (docs/PLAN.md §3.3): hidden becomes PTR_NULL, shapes become 32 bpp New
/// Pointer Updates (Large Pointer Updates above 96 x 96, when negotiated)
/// held in an LRU mirror of the client's pointer cache, so a shape seen
/// before costs a Cached Pointer Update. Sans-IO: the caller sends what comes
/// out, in order, with Connection::send_pointer.
///
/// Shapes larger than the client accepts are scaled down (area average, in
/// premultiplied alpha) to fit, with the hotspot scaled along. Hotspots are
/// clamped into the image.
class CursorEncoder {
public:
    struct Config {
        /// Slots in the client's pointer cache, for New and Large Pointer
        /// Updates. 0: the client cannot take them; Color Pointer Updates
        /// (24 bpp, 1-bit transparency) go to the color pointer cache instead.
        std::uint16_t pointer_cache_size = 0;
        std::uint16_t color_pointer_cache_size = 0;
        /// Largest New or Color Pointer shape: 32, or 96 with
        /// LARGE_POINTER_FLAG_96x96 ([MS-RDPBCGR] 2.2.9.1.1.4.4).
        std::uint16_t max_size = proto::pointer::max_legacy_size;
        /// Fast-Path Large Pointer Updates, up to 384 x 384, are allowed
        /// (LARGE_POINTER_FLAG_384x384 and fast-path output).
        bool large_pointers = false;
        /// The most pointer data one update may carry (Connection::max_update_size()).
        /// Shapes are scaled down further when they would not fit.
        std::size_t max_update_size = 0x3FFF - 64;
        /// Send Pointer Position Updates when the platform reports a new
        /// position. Clients move their pointer themselves, so leave this off
        /// unless something on the server moves the pointer; warp() always sends.
        bool send_positions = false;

        /// The configuration a connection negotiated (`session.pointer`,
        /// `session.fastpath_output`), with Connection::max_update_size().
        [[nodiscard]] static Config negotiated(const Session& session, std::size_t max_update_size);
    };

    explicit CursorEncoder(Config config);

    /// The pointer updates that bring the client in line with `update`,
    /// in the order to send them. Often empty.
    [[nodiscard]] std::vector<proto::PointerUpdate> encode(const platform::CursorUpdate& update);

    /// A server-driven move of the hotspot to desktop position (x, y): always
    /// a Pointer Position Update, whatever `send_positions` says.
    [[nodiscard]] proto::PointerUpdate warp(std::int32_t x, std::int32_t y);

    /// Records where the client put its pointer (from client input), so a
    /// platform position report that only echoes that move is not sent back.
    void client_moved(std::int32_t x, std::int32_t y);

    /// Starts over with a new configuration and an empty client cache, as
    /// after a (re)activation, and returns the updates that restore the
    /// current cursor on the client.
    [[nodiscard]] std::vector<proto::PointerUpdate> reset(Config config);

    [[nodiscard]] const Config& config() const noexcept { return config_; }

private:
    enum class Kind : std::uint8_t { color, new_pointer, large };
    struct Layout {
        Kind kind = Kind::new_pointer;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };
    struct Slot {
        bool used = false;
        std::uint64_t hash = 0;
        std::uint64_t last_used = 0;
        platform::CursorImage image;
    };
    enum class Shown : std::uint8_t { system_default, hidden, slot };

    [[nodiscard]] std::uint16_t cache_size() const noexcept;
    [[nodiscard]] std::optional<Layout> choose_layout(std::uint32_t width, std::uint32_t height) const;
    [[nodiscard]] bool slot_holds(std::size_t index, const platform::CursorImage& shape) const;
    [[nodiscard]] std::size_t take_slot();
    [[nodiscard]] static proto::PointerUpdate build(const platform::CursorImage& source, const Layout& layout,
                                                    std::uint16_t cache_index);
    /// Makes the client show `shape` (the current, normalized shape).
    void show_shape(std::vector<proto::PointerUpdate>& out, const platform::CursorImage& shape);
    /// Makes the client show its default pointer or none.
    void show_system(std::vector<proto::PointerUpdate>& out, Shown target);

    Config config_;
    std::vector<Slot> slots_;
    std::uint64_t tick_ = 0;

    std::optional<platform::CursorImage> shape_;  ///< normalized
    std::uint64_t shape_hash_ = 0;
    bool visible_ = true;
    std::optional<std::pair<std::int32_t, std::int32_t>> position_;

    Shown shown_ = Shown::system_default;  ///< what the client draws
    std::size_t shown_slot_ = 0;
};

}  // namespace farland::server
