// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// RDP 6.0 planar bitmap codec, [MS-RDPEGDI] 2.2.2.5.1 and 3.1.9.
//
// Written independently; the wire format was cross-checked against FreeRDP's
// libfreerdp/codec/planar.c (Apache-2.0).

#include <farland/base/assert.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>
#include <farland/codec/planar.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace farland::codec::planar {

namespace {

// FormatHeader of the RDP6_BITMAP_STREAM, [MS-RDPEGDI] 2.2.2.5.1.
constexpr std::uint8_t header_cll_mask = 0x07;  // Color Loss Level; 0 means RGB planes, else YCoCg.
constexpr std::uint8_t header_cs = 0x08;        // Chroma Subsampling of the YCoCg chroma planes.
constexpr std::uint8_t header_rle = 0x10;       // Planes are RLE (RDP6_RLE_PLANES), not raw.
constexpr std::uint8_t header_na = 0x20;        // No Alpha: the AlphaPlane is absent.

// Byte offsets inside one B, G, R, X/A pixel.
constexpr std::size_t bytes_per_pixel = 4;
constexpr std::size_t offset_b = 0;
constexpr std::size_t offset_g = 1;
constexpr std::size_t offset_r = 2;
constexpr std::size_t offset_a = 3;

// RDP6_RLE_SEGMENT, [MS-RDPEGDI] 2.2.2.5.1: a control byte with cRawBytes in
// the high nibble and nRunLength in the low nibble, then cRawBytes raw
// values, then a run of nRunLength copies of the last value. nRunLength 1
// and 2 are escapes for longer runs: the run is 16 + cRawBytes or
// 32 + cRawBytes, and the segment has no raw values.
constexpr std::size_t max_raw_count = 15;
constexpr std::size_t max_short_run = 15;
constexpr std::size_t min_run = 3;
constexpr std::uint8_t run_escape_16 = 1;
constexpr std::uint8_t run_escape_32 = 2;
constexpr std::size_t max_run = 32 + 15;

[[nodiscard]] constexpr std::uint8_t control_byte(std::size_t raw_count, std::size_t run_nibble)
{
    FARLAND_ASSERT(raw_count <= 0x0F && run_nibble <= 0x0F);
    return static_cast<std::uint8_t>((raw_count << 4U) | run_nibble);
}

struct Segment {
    std::size_t raw_count = 0;
    std::size_t run = 0;
};

[[nodiscard]] constexpr Segment parse_control_byte(std::uint8_t control)
{
    const std::size_t raw_count = control >> 4U;
    const std::size_t run = control & 0x0FU;
    if (run == run_escape_16) {
        return {.raw_count = 0, .run = 16 + raw_count};
    }
    if (run == run_escape_32) {
        return {.raw_count = 0, .run = 32 + raw_count};
    }
    return {.raw_count = raw_count, .run = run};
}

// Scanline deltas, [MS-RDPEGDI] 3.1.9: every scanline after the first stores
// (value - value above) mod 256, read as a signed byte, in sign-magnitude
// form: the magnitude shifted left by one, with bit 0 set for negative deltas
// (then holding magnitude * 2 - 1, so that -128 fits as 0xFF).
[[nodiscard]] constexpr std::uint8_t delta_to_wire(std::uint8_t value, std::uint8_t above)
{
    const auto delta = static_cast<std::uint8_t>(value - above);
    if (delta < 0x80U) {
        return static_cast<std::uint8_t>(delta << 1U);
    }
    const unsigned magnitude = 0x100U - delta;  // 1..128
    return static_cast<std::uint8_t>((magnitude << 1U) - 1U);
}

/// The inverse of delta_to_wire: what to add (mod 256) to the value above.
[[nodiscard]] constexpr std::uint8_t wire_to_delta(std::uint8_t wire)
{
    const unsigned half = wire >> 1U;
    if ((wire & 1U) == 0) {
        return static_cast<std::uint8_t>(half);
    }
    return static_cast<std::uint8_t>(0x100U - (half + 1U));
}

static_assert(delta_to_wire(0x12, 0x10) == 0x04 && delta_to_wire(0x10, 0x12) == 0x03);
static_assert(delta_to_wire(0x80, 0x00) == 0xFF && delta_to_wire(0x7F, 0x00) == 0xFE);
static_assert(wire_to_delta(0xFF) == 0x80 && wire_to_delta(0x03) == 0xFE && wire_to_delta(0x04) == 0x02);

/// Maps stream scanlines to rows of the top-down image.
struct Geometry {
    std::size_t width = 0;
    std::size_t height = 0;
    Orientation orientation = Orientation::bottom_up;

    [[nodiscard]] std::size_t pixels() const { return width * height; }
    [[nodiscard]] std::size_t image_row(std::size_t scanline) const
    {
        return orientation == Orientation::bottom_up ? height - 1 - scanline : scanline;
    }
};

// ---------------------------------------------------------------------------
// Encoder

using Plane = std::vector<std::uint8_t>;

/// The Red, Green and Blue planes in stream order ([MS-RDPEGDI] 2.2.2.5.1),
/// one value per pixel, scanlines in stream row order.
[[nodiscard]] std::array<Plane, 3> split_planes(const ImageView& image, const Geometry& g)
{
    std::array<Plane, 3> planes;
    for (auto& plane : planes) {
        plane.resize(g.pixels());
    }
    std::size_t i = 0;
    for (std::size_t scanline = 0; scanline < g.height; ++scanline) {
        const auto row = image.data.subspan(g.image_row(scanline) * image.stride, g.width * bytes_per_pixel);
        for (std::size_t x = 0; x < g.width; ++x, ++i) {
            const auto pixel = row.subspan(x * bytes_per_pixel, bytes_per_pixel);
            planes[0][i] = std::to_integer<std::uint8_t>(pixel[offset_r]);
            planes[1][i] = std::to_integer<std::uint8_t>(pixel[offset_g]);
            planes[2][i] = std::to_integer<std::uint8_t>(pixel[offset_b]);
        }
    }
    return planes;
}

/// FormatHeader + three raw planes + Pad ([MS-RDPEGDI] 2.2.2.5.1).
[[nodiscard]] std::size_t raw_stream_size(const Geometry& g)
{
    return 1 + (3 * g.pixels()) + 1;
}

[[nodiscard]] std::vector<std::byte> encode_raw(const std::array<Plane, 3>& planes, const Geometry& g)
{
    Writer w(raw_stream_size(g));
    w.u8(header_na);
    for (const auto& plane : planes) {
        w.bytes(std::as_bytes(std::span(plane)));
    }
    w.u8(0);  // Pad, which FreeRDP and mstsc both accept
    return std::move(w).take();
}

/// A run of `run` (>= 3) copies of the last value, split into segments
/// without raw values. Pieces never leave a remainder of 1 or 2, which no
/// segment could express.
void write_runs(Writer& w, std::size_t run)
{
    while (run > 0) {
        FARLAND_ASSERT(run >= min_run);
        std::size_t piece = std::min(run, max_run);
        if (const std::size_t rest = run - piece; rest > 0 && rest < min_run) {
            piece = run - min_run;
        }
        if (piece >= 32) {
            w.u8(control_byte(piece - 32, run_escape_32));
        } else if (piece >= 16) {
            w.u8(control_byte(piece - 16, run_escape_16));
        } else {
            w.u8(control_byte(0, piece));
        }
        run -= piece;
    }
}

/// Raw values followed by a run (0 or >= 3) of the last of them, as one or
/// more RDP6_RLE_SEGMENTs.
void write_segments(Writer& w, std::span<const std::uint8_t> raw, std::size_t run)
{
    FARLAND_ASSERT(run == 0 || run >= min_run);
    while (raw.size() > max_raw_count) {
        w.u8(control_byte(max_raw_count, 0));
        w.bytes(std::as_bytes(raw.first(max_raw_count)));
        raw = raw.subspan(max_raw_count);
    }
    if (!raw.empty()) {
        // The segment carrying raw values holds a run of 3..15 in its low
        // nibble, leaving at least 3 for the run-only segments that follow.
        std::size_t inline_run = 0;
        if (run <= max_short_run) {
            inline_run = run;
        } else if (run - max_short_run >= min_run) {
            inline_run = max_short_run;
        } else {
            inline_run = run - min_run;
        }
        w.u8(control_byte(raw.size(), inline_run));
        w.bytes(std::as_bytes(raw));
        run -= inline_run;
    }
    write_runs(w, run);
}

/// One scanline of an RLE plane, coded on its own ([MS-RDPEGDI] 3.1.9). A run
/// repeats the last raw value of the scanline, or 0 before the first one, so
/// a group of n equal symbols costs one raw value and a run of n - 1, or no
/// raw value when it opens the scanline with symbol 0. Runs shorter than 3
/// stay raw values.
void write_rle_scanline(Writer& w, std::span<const std::uint8_t> symbols)
{
    std::size_t raw_begin = 0;
    std::size_t i = 0;
    while (i < symbols.size()) {
        std::size_t end = i + 1;
        while (end < symbols.size() && symbols[end] == symbols[i]) {
            ++end;
        }
        const std::size_t length = end - i;
        if (i == 0 && symbols[0] == 0 && length >= min_run) {
            write_segments(w, {}, length);
            raw_begin = end;
        } else if (length > min_run) {
            write_segments(w, symbols.subspan(raw_begin, i + 1 - raw_begin), length - 1);
            raw_begin = end;
        }
        i = end;
    }
    if (raw_begin < symbols.size()) {
        write_segments(w, symbols.subspan(raw_begin), 0);
    }
}

/// FormatHeader + RDP6_RLE_PLANES for Red, Green and Blue ([MS-RDPEGDI]
/// 2.2.2.5.1). There is no pad byte after RLE planes.
[[nodiscard]] std::vector<std::byte> encode_rle(const std::array<Plane, 3>& planes, const Geometry& g)
{
    Writer w;
    w.u8(header_na | header_rle);
    std::vector<std::uint8_t> symbols(g.width);
    for (const auto& plane : planes) {
        const std::span<const std::uint8_t> values(plane);
        for (std::size_t scanline = 0; scanline < g.height; ++scanline) {
            const auto row = values.subspan(scanline * g.width, g.width);
            if (scanline == 0) {
                std::ranges::copy(row, symbols.begin());
            } else {
                const auto above = values.subspan((scanline - 1) * g.width, g.width);
                std::ranges::transform(row, above, symbols.begin(), delta_to_wire);
            }
            write_rle_scanline(w, symbols);
        }
    }
    return std::move(w).take();
}

// ---------------------------------------------------------------------------
// Decoder

[[nodiscard]] std::size_t sample_index(std::size_t pixel, std::size_t channel)
{
    return (pixel * bytes_per_pixel) + channel;
}

/// One raw plane: width * height values in stream row order.
[[nodiscard]] Result<void> decode_raw_plane(Reader& r, const Geometry& g, std::size_t channel, std::span<std::byte> out)
{
    for (std::size_t scanline = 0; scanline < g.height; ++scanline) {
        FARLAND_TRY(const auto values, r.bytes(g.width));
        const std::size_t first = g.image_row(scanline) * g.width;
        for (std::size_t x = 0; x < g.width; ++x) {
            out[sample_index(first + x, channel)] = values[x];
        }
    }
    return {};
}

/// One RLE plane (RDP6_RLE_SEGMENTS per scanline, [MS-RDPEGDI] 2.2.2.5.1 and
/// 3.1.9). Segments must cover each scanline exactly.
[[nodiscard]] Result<void> decode_rle_plane(Reader& r, const Geometry& g, std::size_t channel, std::span<std::byte> out)
{
    for (std::size_t scanline = 0; scanline < g.height; ++scanline) {
        const std::size_t first = g.image_row(scanline) * g.width;
        const std::size_t first_above = scanline == 0 ? 0 : g.image_row(scanline - 1) * g.width;
        // The first scanline holds values (added to 0), later ones deltas.
        // A run repeats the last of them, and starts out as 0.
        std::uint8_t last = 0;
        const auto store = [&](std::size_t x) {
            const std::uint8_t above =
                scanline == 0 ? 0 : std::to_integer<std::uint8_t>(out[sample_index(first_above + x, channel)]);
            out[sample_index(first + x, channel)] = std::byte{static_cast<std::uint8_t>(above + last)};
        };

        std::size_t x = 0;
        while (x < g.width) {
            const std::size_t at = r.offset();
            FARLAND_TRY(const auto control, r.u8());
            const auto segment = parse_control_byte(control);
            if (segment.raw_count + segment.run > g.width - x) {
                return fail(Errc::invalid_length, "planar RLE segment overruns the scanline", at);
            }
            FARLAND_TRY(const auto raw, r.bytes(segment.raw_count));
            for (const std::byte value : raw) {
                const auto wire = std::to_integer<std::uint8_t>(value);
                last = scanline == 0 ? wire : wire_to_delta(wire);
                store(x++);
            }
            for (std::size_t i = 0; i < segment.run; ++i) {
                store(x++);
            }
        }
    }
    return {};
}

}  // namespace

std::vector<std::byte> encode(const ImageView& image, const EncodeOptions& options)
{
    FARLAND_ASSERT(image.width >= 1 && image.width <= max_dimension);
    FARLAND_ASSERT(image.height >= 1 && image.height <= max_dimension);
    const Geometry g{.width = image.width, .height = image.height, .orientation = options.orientation};
    const std::size_t row_bytes = g.width * bytes_per_pixel;
    FARLAND_ASSERT(image.stride >= row_bytes);
    FARLAND_ASSERT(image.data.size() >= row_bytes && (image.data.size() - row_bytes) / image.stride >= g.height - 1);

    const auto planes = split_planes(image, g);
    switch (options.mode) {
    case Mode::raw:
        return encode_raw(planes, g);
    case Mode::rle:
        return encode_rle(planes, g);
    case Mode::automatic:
        break;
    }
    auto rle = encode_rle(planes, g);
    if (rle.size() > raw_stream_size(g)) {
        return encode_raw(planes, g);
    }
    return rle;
}

Result<void> decode(std::span<const std::byte> stream, std::uint32_t width, std::uint32_t height,
                    Orientation orientation, std::span<std::byte> out)
{
    if (width == 0 || height == 0) {
        return fail(Errc::invalid_value, "planar bitmap has no pixels");
    }
    if (width > max_dimension || height > max_dimension) {
        return fail(Errc::limit_exceeded, "planar bitmap exceeds planar::max_dimension");
    }
    const Geometry g{.width = width, .height = height, .orientation = orientation};
    FARLAND_ASSERT(out.size() == g.pixels() * bytes_per_pixel);

    Reader r(stream);
    FARLAND_TRY(const auto header, r.u8());
    if ((header & header_cll_mask) != 0) {
        return fail(Errc::unsupported, "planar color loss reduction (YCoCg) is not supported");
    }
    if ((header & header_cs) != 0) {
        return fail(Errc::unsupported, "planar chroma subsampling is not supported");
    }
    const bool rle = (header & header_rle) != 0;
    const bool has_alpha = (header & header_na) == 0;

    // Plane order: AlphaPlane (absent with NA), Red, Green, Blue ([MS-RDPEGDI] 2.2.2.5.1).
    constexpr std::array<std::size_t, 4> plane_channels{offset_a, offset_r, offset_g, offset_b};
    for (const std::size_t channel : std::span(plane_channels).subspan(has_alpha ? 0 : 1)) {
        if (rle) {
            FARLAND_TRY_VOID(decode_rle_plane(r, g, channel, out));
        } else {
            FARLAND_TRY_VOID(decode_raw_plane(r, g, channel, out));
        }
    }
    if (!has_alpha) {
        for (std::size_t pixel = 0; pixel < g.pixels(); ++pixel) {
            out[sample_index(pixel, offset_a)] = std::byte{0xFF};
        }
    }

    // Pad: one optional byte, only after raw planes ([MS-RDPEGDI] 2.2.2.5.1).
    if (!rle && r.remaining() == 1) {
        FARLAND_TRY_VOID(r.skip(1));
    }
    return r.expect_end("planar bitmap stream has trailing data");
}

}  // namespace farland::codec::planar
