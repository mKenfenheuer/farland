// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

/// Pointer updates: the server tells the client which mouse pointer to draw,
/// [MS-RDPBCGR] 2.2.9.1.1.4 (slow-path TS_POINTER_PDU) and 2.2.9.1.2.1.4 -
/// 2.2.9.1.2.1.11 (fast-path pointer updates). The cursor is never part of
/// the desktop image (docs/PLAN.md §3.3).
namespace farland::proto::pointer {

/// TS_POINTER_PDU messageType, [MS-RDPBCGR] 2.2.9.1.1.4.
namespace message_type {
inline constexpr std::uint16_t system = 0x0001;
inline constexpr std::uint16_t position = 0x0003;
inline constexpr std::uint16_t color = 0x0006;
inline constexpr std::uint16_t cached = 0x0007;
inline constexpr std::uint16_t pointer = 0x0008;
}  // namespace message_type

/// TS_SYSTEMPOINTERATTRIBUTE systemPointerType, [MS-RDPBCGR] 2.2.9.1.1.4.3.
namespace system_pointer {
inline constexpr std::uint32_t null = 0x00000000;
inline constexpr std::uint32_t default_pointer = 0x00007F00;
}  // namespace system_pointer

/// Largest Color and New Pointer shapes: 32 x 32, or 96 x 96 once the client
/// set LARGE_POINTER_FLAG_96x96 ([MS-RDPBCGR] 2.2.9.1.1.4.4). Decoders accept
/// up to 96; which limit applies is a matter of negotiation.
inline constexpr std::uint16_t max_legacy_size = 32;
inline constexpr std::uint16_t max_size = 96;
/// Largest Fast-Path Large Pointer shape, [MS-RDPBCGR] 2.2.9.1.2.1.11.
inline constexpr std::uint16_t max_large_size = 384;

/// A pointer image as it travels: the fields of TS_COLORPOINTERATTRIBUTE
/// (2.2.9.1.1.4.4), plus the xorBpp of TS_POINTERATTRIBUTE (2.2.9.1.1.4.5) and
/// TS_FP_LARGEPOINTERATTRIBUTE (2.2.9.1.2.1.11).
///
/// Both masks are bottom-up with every scan line padded to two bytes. The XOR
/// mask has `xor_bpp` bits per pixel (32 bpp is BGRA with straight alpha);
/// the AND mask has one bit per pixel, most significant bit first.
struct Shape {
    std::uint16_t xor_bpp = 32;
    std::uint16_t cache_index = 0;
    /// Kept as sent. Some servers send hotspots outside the image (FreeRDP
    /// resets those to 0 when drawing); decoders do not reject them.
    std::uint16_t hotspot_x = 0;
    std::uint16_t hotspot_y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::vector<std::byte> xor_mask;
    std::vector<std::byte> and_mask;

    friend bool operator==(const Shape&, const Shape&) = default;
};

/// PTR_NULL / SYSPTR_NULL: hide the pointer (2.2.9.1.2.1.5, 2.2.9.1.1.4.3).
struct Hidden {
    friend bool operator==(const Hidden&, const Hidden&) = default;
};
/// PTR_DEFAULT / SYSPTR_DEFAULT: the client's default pointer (2.2.9.1.2.1.6).
struct Default {
    friend bool operator==(const Default&, const Default&) = default;
};
/// TS_POINTERPOSATTRIBUTE: move the pointer's hotspot here (2.2.9.1.1.4.2).
struct Position {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    friend bool operator==(const Position&, const Position&) = default;
};
/// TS_COLORPOINTERATTRIBUTE: a 24 bpp pointer, stored in the color pointer
/// cache (2.2.9.1.1.4.4). `shape.xor_bpp` is 24.
struct ColorPointer {
    Shape shape;
    friend bool operator==(const ColorPointer&, const ColorPointer&) = default;
};
/// TS_POINTERATTRIBUTE: a pointer at any depth, stored in the pointer cache
/// (2.2.9.1.1.4.5).
struct NewPointer {
    Shape shape;
    friend bool operator==(const NewPointer&, const NewPointer&) = default;
};
/// TS_FP_LARGEPOINTERATTRIBUTE: up to 384 x 384, stored in the pointer cache
/// (2.2.9.1.2.1.11). Fast-path only.
struct LargePointer {
    Shape shape;
    friend bool operator==(const LargePointer&, const LargePointer&) = default;
};
/// TS_CACHEDPOINTERATTRIBUTE: show a pointer cached earlier (2.2.9.1.1.4.6).
struct CachedPointer {
    std::uint16_t cache_index = 0;
    friend bool operator==(const CachedPointer&, const CachedPointer&) = default;
};

using Update = std::variant<Hidden, Default, Position, ColorPointer, NewPointer, LargePointer, CachedPointer>;

/// Bytes per XOR mask scan line: `width` pixels at `xor_bpp`, padded to two bytes.
[[nodiscard]] std::size_t xor_stride(std::uint16_t width, std::uint16_t xor_bpp) noexcept;
/// Bytes per AND mask scan line: `width` bits, padded to two bytes.
[[nodiscard]] std::size_t and_stride(std::uint16_t width) noexcept;

/// Checks the invariants of a shape: size within `max_dimension`, a depth
/// RDP defines, and mask lengths that match width, height and depth.
[[nodiscard]] Result<void> validate(const Shape& shape, std::uint16_t max_dimension);

/// The fast-path updateCode for `update` ([MS-RDPBCGR] 2.2.9.1.2.1).
[[nodiscard]] std::uint8_t fastpath_code(const Update& update);
/// Bytes `encode_fastpath` writes for `update`.
[[nodiscard]] std::size_t fastpath_size(const Update& update);
/// Writes the data of the fast-path update (what follows the size field of
/// TS_FP_UPDATE) and returns its updateCode. Shapes must be valid.
std::uint8_t encode_fastpath(Writer& w, const Update& update);
/// Decodes the data of one reassembled fast-path pointer update; `data`
/// must hold exactly that update.
[[nodiscard]] Result<Update> decode_fastpath(std::uint8_t code, Reader& data);

/// Writes TS_POINTER_PDU from messageType on (the payload after the Share
/// Data Header). Large pointers have no slow-path form and must not be passed.
void encode_slow_path(Writer& w, const Update& update);
/// Decodes the payload of a Pointer Update PDU (pduType2 PDUTYPE2_POINTER).
[[nodiscard]] Result<Update> decode_slow_path(Reader& payload);

/// Builds the masks of a 24 or 32 bpp shape from straight-alpha BGRA,
/// top-down, stride width * 4. Hotspot and cache index are left at 0.
///
/// 32 bpp: the XOR mask carries the pixels with their alpha, and fully
/// transparent pixels become XOR 0 with the AND bit set. Clients that honour
/// alpha (mstsc, FreeRDP) draw exactly the input; clients that apply the
/// masks the classic way see those pixels as transparent too.
/// 24 bpp: pixels with alpha >= 128 are opaque, the rest transparent.
[[nodiscard]] Shape shape_from_bgra(std::span<const std::byte> bgra, std::uint16_t width, std::uint16_t height,
                                    std::uint16_t xor_bpp);

/// Renders a shape to straight-alpha BGRA, top-down, the way FreeRDP's client
/// does (libfreerdp/codec/color.c). Where the AND bit is set, opaque black
/// becomes transparent and opaque white, which Windows draws by inverting
/// the screen, becomes FreeRDP's black and white checkerboard. FreeRDP reads
/// 1 bpp masks top-down; farland does the same. 4 and 8 bpp need a palette
/// and are not supported.
[[nodiscard]] Result<std::vector<std::byte>> shape_to_bgra(const Shape& shape);

}  // namespace farland::proto::pointer

namespace farland::proto {
using PointerUpdate = pointer::Update;
}  // namespace farland::proto
