// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/proto/fastpath.hpp>
#include <farland/proto/pointer.hpp>

#include <algorithm>
#include <array>

namespace farland::proto::pointer {

namespace {

namespace code = fastpath::update_code;

constexpr std::size_t color_header_size = 14;  // cacheIndex, hotSpot, width, height, lengthAndMask, lengthXorMask
constexpr std::size_t large_header_size = 20;  // xorBpp, cacheIndex, hotSpot, width, height, 2 x 4-byte lengths
constexpr std::size_t pad_size = 1;            // the optional pad, which farland always writes as FreeRDP does

constexpr std::array<std::uint16_t, 7> valid_depths{1, 4, 8, 15, 16, 24, 32};

std::size_t mask_bytes(const Shape& s) noexcept
{
    return s.xor_mask.size() + s.and_mask.size();
}

void write_shape_fields(Writer& w, const Shape& s)
{
    w.u16le(s.hotspot_x);
    w.u16le(s.hotspot_y);
    w.u16le(s.width);
    w.u16le(s.height);
}

/// TS_COLORPOINTERATTRIBUTE, [MS-RDPBCGR] 2.2.9.1.1.4.4.
void encode_color_attribute(Writer& w, const Shape& s)
{
    FARLAND_ASSERT(validate(s, max_size).has_value());
    w.u16le(s.cache_index);
    write_shape_fields(w, s);
    w.u16le(static_cast<std::uint16_t>(s.and_mask.size()));
    w.u16le(static_cast<std::uint16_t>(s.xor_mask.size()));
    w.bytes(s.xor_mask);
    w.bytes(s.and_mask);
    w.u8(0);
}

/// TS_FP_LARGEPOINTERATTRIBUTE after the update header, [MS-RDPBCGR] 2.2.9.1.2.1.11.
void encode_large_attribute(Writer& w, const Shape& s)
{
    FARLAND_ASSERT(validate(s, max_large_size).has_value());
    w.u16le(s.xor_bpp);
    w.u16le(s.cache_index);
    write_shape_fields(w, s);
    w.u32le(static_cast<std::uint32_t>(s.and_mask.size()));
    w.u32le(static_cast<std::uint32_t>(s.xor_mask.size()));
    w.bytes(s.xor_mask);
    w.bytes(s.and_mask);
    w.u8(0);
}

/// Reads both masks once their lengths have been checked against the shape,
/// then the optional pad. The attribute must fill `r`.
Result<void> read_masks(Reader& r, Shape& s, std::size_t and_length, std::size_t xor_length,
                        std::uint16_t max_dimension, std::size_t start)
{
    if (xor_length != xor_stride(s.width, s.xor_bpp) * s.height) {
        return fail(Errc::invalid_length, "pointer XOR mask length does not match its size", start);
    }
    if (and_length != and_stride(s.width) * s.height) {
        return fail(Errc::invalid_length, "pointer AND mask length does not match its size", start);
    }
    FARLAND_TRY(const auto xor_bytes, r.bytes(xor_length));
    FARLAND_TRY(const auto and_bytes, r.bytes(and_length));
    s.xor_mask.assign(xor_bytes.begin(), xor_bytes.end());
    s.and_mask.assign(and_bytes.begin(), and_bytes.end());
    if (r.remaining() == pad_size) {
        FARLAND_TRY_VOID(r.skip(pad_size));
    }
    FARLAND_TRY_VOID(r.expect_end("pointer attribute"));
    return validate(s, max_dimension).transform_error([start](Error e) {
        e.offset = start;
        return e;
    });
}

/// Checks the dimensions before any mask is read, so lengths are bounded.
Result<void> check_header(const Shape& s, std::uint16_t max_dimension, std::size_t start)
{
    if (s.width == 0 || s.height == 0) {
        return fail(Errc::invalid_value, "empty pointer shape", start);
    }
    if (s.width > max_dimension || s.height > max_dimension) {
        return fail(Errc::limit_exceeded, "pointer shape too large", start);
    }
    if (std::ranges::find(valid_depths, s.xor_bpp) == valid_depths.end()) {
        return fail(Errc::invalid_value, "invalid pointer xorBpp", start);
    }
    return {};
}

Result<Shape> decode_color_attribute(Reader& r, std::uint16_t xor_bpp)
{
    const std::size_t start = r.offset();
    Shape s;
    s.xor_bpp = xor_bpp;
    FARLAND_TRY(s.cache_index, r.u16le());
    FARLAND_TRY(s.hotspot_x, r.u16le());
    FARLAND_TRY(s.hotspot_y, r.u16le());
    FARLAND_TRY(s.width, r.u16le());
    FARLAND_TRY(s.height, r.u16le());
    FARLAND_TRY(const std::uint16_t and_length, r.u16le());
    FARLAND_TRY(const std::uint16_t xor_length, r.u16le());
    FARLAND_TRY_VOID(check_header(s, max_size, start));
    FARLAND_TRY_VOID(read_masks(r, s, and_length, xor_length, max_size, start));
    return s;
}

Result<Shape> decode_new_attribute(Reader& r)
{
    FARLAND_TRY(const std::uint16_t xor_bpp, r.u16le());
    return decode_color_attribute(r, xor_bpp);
}

Result<Shape> decode_large_attribute(Reader& r)
{
    const std::size_t start = r.offset();
    Shape s;
    FARLAND_TRY(s.xor_bpp, r.u16le());
    FARLAND_TRY(s.cache_index, r.u16le());
    FARLAND_TRY(s.hotspot_x, r.u16le());
    FARLAND_TRY(s.hotspot_y, r.u16le());
    FARLAND_TRY(s.width, r.u16le());
    FARLAND_TRY(s.height, r.u16le());
    FARLAND_TRY(const std::uint32_t and_length, r.u32le());
    FARLAND_TRY(const std::uint32_t xor_length, r.u32le());
    FARLAND_TRY_VOID(check_header(s, max_large_size, start));
    FARLAND_TRY_VOID(read_masks(r, s, and_length, xor_length, max_large_size, start));
    return s;
}

Result<Position> decode_position(Reader& r)
{
    Position p;
    FARLAND_TRY(p.x, r.u16le());
    FARLAND_TRY(p.y, r.u16le());
    FARLAND_TRY_VOID(r.expect_end("pointer position update"));
    return p;
}

Result<CachedPointer> decode_cached(Reader& r)
{
    CachedPointer c;
    FARLAND_TRY(c.cache_index, r.u16le());
    FARLAND_TRY_VOID(r.expect_end("cached pointer update"));
    return c;
}

template <class... F>
struct Overloaded : F... {
    using F::operator()...;
};

// Rendering ---------------------------------------------------------------------

struct Bgra {
    std::uint8_t b = 0;
    std::uint8_t g = 0;
    std::uint8_t r = 0;
    std::uint8_t a = 0;
    friend bool operator==(const Bgra&, const Bgra&) = default;
};

constexpr Bgra opaque_black{0, 0, 0, 0xFF};
constexpr Bgra opaque_white{0xFF, 0xFF, 0xFF, 0xFF};
constexpr Bgra transparent{};

/// FreeRDP's stand-in for screen-inverting pixels (freerdp_image_inverted_pointer_color).
Bgra inverted(std::size_t x, std::size_t y) noexcept
{
    return ((x + y) & 1U) != 0 ? opaque_black : opaque_white;
}

std::uint8_t expand5(unsigned v) noexcept
{
    return static_cast<std::uint8_t>((v << 3U) | (v >> 2U));
}

/// Reads one XOR pixel of a 15, 16, 24 or 32 bpp mask.
Result<Bgra> read_xor_pixel(Reader& row, std::uint16_t xor_bpp)
{
    Bgra p;
    switch (xor_bpp) {
    case 32: {
        FARLAND_TRY(p.b, row.u8());
        FARLAND_TRY(p.g, row.u8());
        FARLAND_TRY(p.r, row.u8());
        FARLAND_TRY(p.a, row.u8());
        return p;
    }
    case 24: {
        FARLAND_TRY(p.b, row.u8());
        FARLAND_TRY(p.g, row.u8());
        FARLAND_TRY(p.r, row.u8());
        p.a = 0xFF;
        return p;
    }
    default: {  // 15 and 16: RGB555, as FreeRDP reads both
        FARLAND_TRY(const std::uint16_t v, row.u16le());
        p.r = expand5((v >> 10U) & 0x1FU);
        p.g = expand5((v >> 5U) & 0x1FU);
        p.b = expand5(v & 0x1FU);
        p.a = 0xFF;
        return p;
    }
    }
}

/// One bit per pixel, most significant bit first, over one scan line.
class BitReader {
public:
    explicit BitReader(Reader row) : row_(row) {}

    Result<bool> next()
    {
        if (mask_ == 0) {
            FARLAND_TRY(byte_, row_.u8());
            mask_ = 0x80;
        }
        const bool bit = (byte_ & mask_) != 0;
        mask_ = static_cast<std::uint8_t>(mask_ >> 1U);
        return bit;
    }

private:
    Reader row_;
    std::uint8_t byte_ = 0;
    std::uint8_t mask_ = 0;
};

void put(std::vector<std::byte>& out, std::size_t index, Bgra p)
{
    out.at(index) = std::byte{p.b};
    out.at(index + 1) = std::byte{p.g};
    out.at(index + 2) = std::byte{p.r};
    out.at(index + 3) = std::byte{p.a};
}

}  // namespace

std::size_t xor_stride(std::uint16_t width, std::uint16_t xor_bpp) noexcept
{
    const std::size_t bytes = ((std::size_t{width} * xor_bpp) + 7U) / 8U;
    return (bytes + 1U) & ~std::size_t{1};
}

std::size_t and_stride(std::uint16_t width) noexcept
{
    return xor_stride(width, 1);
}

Result<void> validate(const Shape& shape, std::uint16_t max_dimension)
{
    FARLAND_TRY_VOID(check_header(shape, max_dimension, 0));
    if (shape.xor_mask.size() != xor_stride(shape.width, shape.xor_bpp) * shape.height) {
        return fail(Errc::invalid_length, "pointer XOR mask length does not match its size");
    }
    if (shape.and_mask.size() != and_stride(shape.width) * shape.height) {
        return fail(Errc::invalid_length, "pointer AND mask length does not match its size");
    }
    return {};
}

std::uint8_t fastpath_code(const Update& update)
{
    return std::visit(Overloaded{
                          [](const Hidden&) { return code::pointer_hidden; },
                          [](const Default&) { return code::pointer_default; },
                          [](const Position&) { return code::pointer_position; },
                          [](const ColorPointer&) { return code::color_pointer; },
                          [](const NewPointer&) { return code::new_pointer; },
                          [](const LargePointer&) { return code::large_pointer; },
                          [](const CachedPointer&) { return code::cached_pointer; },
                      },
                      update);
}

std::size_t fastpath_size(const Update& update)
{
    return std::visit(Overloaded{
                          [](const Hidden&) -> std::size_t { return 0; },
                          [](const Default&) -> std::size_t { return 0; },
                          [](const Position&) -> std::size_t { return 4; },
                          [](const ColorPointer& p) { return color_header_size + mask_bytes(p.shape) + pad_size; },
                          [](const NewPointer& p) { return 2 + color_header_size + mask_bytes(p.shape) + pad_size; },
                          [](const LargePointer& p) { return large_header_size + mask_bytes(p.shape) + pad_size; },
                          [](const CachedPointer&) -> std::size_t { return 2; },
                      },
                      update);
}

std::uint8_t encode_fastpath(Writer& w, const Update& update)
{
    std::visit(Overloaded{
                   [](const Hidden&) {},  // size MUST be zero, 2.2.9.1.2.1.5
                   [](const Default&) {},
                   [&w](const Position& p) {
                       w.u16le(p.x);
                       w.u16le(p.y);
                   },
                   [&w](const ColorPointer& p) {
                       FARLAND_ASSERT(p.shape.xor_bpp == 24);
                       encode_color_attribute(w, p.shape);
                   },
                   [&w](const NewPointer& p) {
                       w.u16le(p.shape.xor_bpp);
                       encode_color_attribute(w, p.shape);
                   },
                   [&w](const LargePointer& p) { encode_large_attribute(w, p.shape); },
                   [&w](const CachedPointer& p) { w.u16le(p.cache_index); },
               },
               update);
    return fastpath_code(update);
}

Result<Update> decode_fastpath(std::uint8_t update_code, Reader& data)
{
    switch (update_code) {
    case code::pointer_hidden:
        FARLAND_TRY_VOID(data.expect_end("fast-path pointer hidden update"));
        return Hidden{};
    case code::pointer_default:
        FARLAND_TRY_VOID(data.expect_end("fast-path pointer default update"));
        return Default{};
    case code::pointer_position: {
        FARLAND_TRY(const Position p, decode_position(data));
        return p;
    }
    case code::color_pointer: {
        FARLAND_TRY(Shape s, decode_color_attribute(data, 24));
        return ColorPointer{std::move(s)};
    }
    case code::new_pointer: {
        FARLAND_TRY(Shape s, decode_new_attribute(data));
        return NewPointer{std::move(s)};
    }
    case code::large_pointer: {
        FARLAND_TRY(Shape s, decode_large_attribute(data));
        return LargePointer{std::move(s)};
    }
    case code::cached_pointer: {
        FARLAND_TRY(const CachedPointer c, decode_cached(data));
        return c;
    }
    default:
        return fail(Errc::invalid_value, "not a fast-path pointer update", data.offset());
    }
}

void encode_slow_path(Writer& w, const Update& update)
{
    FARLAND_ASSERT(!std::holds_alternative<LargePointer>(update));  // fast-path only
    const auto header = [&w](std::uint16_t type) {
        w.u16le(type);
        w.u16le(0);  // pad2Octets
    };
    std::visit(Overloaded{
                   [&](const Hidden&) {
                       header(message_type::system);
                       w.u32le(system_pointer::null);
                   },
                   [&](const Default&) {
                       header(message_type::system);
                       w.u32le(system_pointer::default_pointer);
                   },
                   [&](const Position& p) {
                       header(message_type::position);
                       w.u16le(p.x);
                       w.u16le(p.y);
                   },
                   [&](const ColorPointer& p) {
                       FARLAND_ASSERT(p.shape.xor_bpp == 24);
                       header(message_type::color);
                       encode_color_attribute(w, p.shape);
                   },
                   [&](const NewPointer& p) {
                       header(message_type::pointer);
                       w.u16le(p.shape.xor_bpp);
                       encode_color_attribute(w, p.shape);
                   },
                   [](const LargePointer&) {},
                   [&](const CachedPointer& p) {
                       header(message_type::cached);
                       w.u16le(p.cache_index);
                   },
               },
               update);
}

Result<Update> decode_slow_path(Reader& payload)
{
    const std::size_t start = payload.offset();
    FARLAND_TRY(const std::uint16_t type, payload.u16le());
    FARLAND_TRY_VOID(payload.skip(2));  // pad2Octets
    switch (type) {
    case message_type::system: {
        FARLAND_TRY(const std::uint32_t system, payload.u32le());
        FARLAND_TRY_VOID(payload.expect_end("system pointer update"));
        if (system == system_pointer::null) {
            return Hidden{};
        }
        if (system == system_pointer::default_pointer) {
            return Default{};
        }
        return fail(Errc::invalid_value, "unknown system pointer type", start + 4);
    }
    case message_type::position: {
        FARLAND_TRY(const Position p, decode_position(payload));
        return p;
    }
    case message_type::color: {
        FARLAND_TRY(Shape s, decode_color_attribute(payload, 24));
        return ColorPointer{std::move(s)};
    }
    case message_type::pointer: {
        FARLAND_TRY(Shape s, decode_new_attribute(payload));
        return NewPointer{std::move(s)};
    }
    case message_type::cached: {
        FARLAND_TRY(const CachedPointer c, decode_cached(payload));
        return c;
    }
    default:
        return fail(Errc::invalid_value, "unknown pointer messageType", start);
    }
}

Shape shape_from_bgra(std::span<const std::byte> bgra, std::uint16_t width, std::uint16_t height, std::uint16_t xor_bpp)
{
    FARLAND_ASSERT(xor_bpp == 24 || xor_bpp == 32);
    FARLAND_ASSERT(width > 0 && height > 0 && bgra.size() == std::size_t{width} * height * 4U);
    Shape s;
    s.xor_bpp = xor_bpp;
    s.width = width;
    s.height = height;
    const std::size_t xstride = xor_stride(width, xor_bpp);
    const std::size_t astride = and_stride(width);
    s.xor_mask.assign(xstride * height, std::byte{0});
    s.and_mask.assign(astride * height, std::byte{0});

    Reader in(bgra);
    for (std::size_t y = 0; y < height; ++y) {
        const std::size_t line = height - 1U - y;  // bottom-up
        std::size_t xor_at = line * xstride;
        for (std::size_t x = 0; x < width; ++x) {
            const auto pixel = in.bytes(4).value();
            const Bgra p{std::to_integer<std::uint8_t>(pixel[0]), std::to_integer<std::uint8_t>(pixel[1]),
                         std::to_integer<std::uint8_t>(pixel[2]), std::to_integer<std::uint8_t>(pixel[3])};
            const bool clear = xor_bpp == 32 ? p.a == 0 : p.a < 0x80;
            if (clear) {
                auto& bits = s.and_mask.at((line * astride) + (x / 8U));
                bits |= std::byte{static_cast<std::uint8_t>(0x80U >> (x % 8U))};
            } else {
                s.xor_mask.at(xor_at) = std::byte{p.b};
                s.xor_mask.at(xor_at + 1) = std::byte{p.g};
                s.xor_mask.at(xor_at + 2) = std::byte{p.r};
                if (xor_bpp == 32) {
                    s.xor_mask.at(xor_at + 3) = std::byte{p.a};
                }
            }
            xor_at += xor_bpp / 8U;
        }
    }
    return s;
}

Result<std::vector<std::byte>> shape_to_bgra(const Shape& shape)
{
    FARLAND_TRY_VOID(validate(shape, max_large_size));
    if (shape.xor_bpp == 4 || shape.xor_bpp == 8) {
        return fail(Errc::unsupported, "palette pointers are not supported");
    }
    const std::size_t width = shape.width;
    const std::size_t height = shape.height;
    std::vector<std::byte> out(width * height * 4U);
    Reader xor_rows(shape.xor_mask);
    Reader and_rows(shape.and_mask);
    for (std::size_t line = 0; line < height; ++line) {
        const std::size_t y = shape.xor_bpp == 1 ? line : height - 1U - line;
        FARLAND_TRY(Reader xor_row, xor_rows.sub(xor_stride(shape.width, shape.xor_bpp)));
        FARLAND_TRY(const Reader and_row, and_rows.sub(and_stride(shape.width)));
        BitReader and_bits(and_row);
        BitReader xor_bits(xor_row);
        for (std::size_t x = 0; x < width; ++x) {
            FARLAND_TRY(const bool and_bit, and_bits.next());
            Bgra p;
            if (shape.xor_bpp == 1) {
                // [MS-RDPBCGR] 2.2.9.1.1.4.4 and FreeRDP: AND 0 draws the XOR
                // color (black or white), AND 1 keeps (XOR 0) or inverts (XOR 1)
                // the screen.
                FARLAND_TRY(const bool xor_bit, xor_bits.next());
                if (!and_bit) {
                    p = xor_bit ? opaque_white : opaque_black;
                } else {
                    p = xor_bit ? inverted(x, y) : transparent;
                }
            } else {
                FARLAND_TRY(p, read_xor_pixel(xor_row, shape.xor_bpp));
                if (and_bit) {
                    if (p == opaque_white) {
                        p = inverted(x, y);
                    } else if (shape.xor_bpp == 24 || p == opaque_black) {
                        p = transparent;
                    }
                }
            }
            put(out, ((y * width) + x) * 4U, p);
        }
    }
    return out;
}

}  // namespace farland::proto::pointer
