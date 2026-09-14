// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// ClearCodec encoder, [MS-RDPEGFX] 2.2.4.1 and 3.3.8.1.
//
// Written from the specification: there is no open-source encoder to follow
// (FreeRDP's clear_compress() is a stub). What the client does with each
// structure was checked against FreeRDP's libfreerdp/codec/clear.c and
// ZeroVDI's clear.js, which serve as test oracles.

#include <farland/base/assert.hpp>
#include <farland/base/writer.hpp>
#include <farland/codec/clear.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace farland::codec::clear {

namespace {

/// A pixel as 0x00RRGGBB.
using Color = std::uint32_t;

constexpr std::size_t bytes_per_pixel = 4;

// Encoded sizes, [MS-RDPEGFX] 2.2.4.1.1.
constexpr std::size_t composite_header_size = 12;  // three byte counts
constexpr std::size_t band_header_size = 11;       // xStart, xEnd, yStart, yEnd, BGR background
constexpr std::size_t subcodec_header_size = 13;   // xStart, yStart, width, height, byteCount, id
constexpr std::size_t run_segment_size = 4;        // BGR + runLengthFactor1

// vBarHeader forms, [MS-RDPEGFX] 2.2.4.1.1.2.1.1.1 to 2.2.4.1.1.2.1.1.3.
constexpr std::uint16_t vbar_cache_hit = 0x8000;        // 1, vBarIndex (15 bits)
constexpr std::uint16_t short_vbar_cache_hit = 0x4000;  // 01, shortVBarIndex (14 bits), then shortVBarYOn
constexpr std::size_t vbar_hit_size = 2;
constexpr std::size_t short_vbar_hit_size = 3;
constexpr std::size_t short_vbar_miss_size = 2;  // + 3 per pixel

/// Bands are cut at this many consecutive background columns: a band header
/// (11 bytes) costs less than that many V-bar hits (2 bytes each).
constexpr std::size_t min_band_gap = 6;

/// Bound on the columns remembered for second-chance admission (see
/// Encoder::State::bands_cost); the sets start over when they reach it.
constexpr std::size_t max_seen_columns = 2 * vbar_cache_size;

[[nodiscard]] Color load_pixel(std::span<const std::byte> pixel)
{
    return std::to_integer<Color>(pixel[0]) | (std::to_integer<Color>(pixel[1]) << 8U) |
           (std::to_integer<Color>(pixel[2]) << 16U);
}

/// Blue, green, red: the byte order of every colour on the wire.
void write_bgr(Writer& w, Color c)
{
    w.u8(static_cast<std::uint8_t>(c));
    w.u8(static_cast<std::uint8_t>(c >> 8U));
    w.u8(static_cast<std::uint8_t>(c >> 16U));
}

/// runLengthFactor1, optionally followed by runLengthFactor2 and 3
/// ([MS-RDPEGFX] 2.2.4.1.1.1.1, 2.2.4.1.1.3.1.1.2).
void write_run_length(Writer& w, std::size_t run)
{
    FARLAND_ASSERT(run <= std::numeric_limits<std::uint32_t>::max());
    if (run < 0xFF) {
        w.u8(static_cast<std::uint8_t>(run));
    } else if (run < 0xFFFF) {
        w.u8(0xFF);
        w.u16le(static_cast<std::uint16_t>(run));
    } else {
        w.u8(0xFF);
        w.u16le(0xFFFF);
        w.u32le(static_cast<std::uint32_t>(run));
    }
}

[[nodiscard]] std::uint64_t hash_colors(std::span<const Color> colors, std::uint64_t seed) noexcept
{
    std::uint64_t h = 0x9E37'79B9'7F4A'7C15ULL ^ (seed * 0xFF51'AFD7'ED55'8CCDULL) ^ colors.size();
    for (const Color c : colors) {
        h = (h ^ c) * 0x100'0000'01B3ULL;
        h ^= h >> 29U;
    }
    return h;
}

/// The region as 0x00RRGGBB pixels.
struct Image {
    std::vector<Color> pixels;
    std::size_t width = 0;
    std::size_t height = 0;

    [[nodiscard]] Color at(std::size_t x, std::size_t y) const { return pixels[(y * width) + x]; }
    [[nodiscard]] std::span<const Color> row(std::size_t y) const
    {
        return std::span(pixels).subspan(y * width, width);
    }
};

/// Encoder mirror of the V-Bar or Short V-Bar Storage ([MS-RDPEGFX] 3.3.1.10
/// to 3.3.1.13): entries are written at a cursor that wraps around, exactly
/// as the client writes them, so every slot here holds what the client holds.
/// Lookups compare the content, so a hash collision is only a missed hit.
class ColumnCache {
public:
    explicit ColumnCache(std::size_t capacity) : capacity_(capacity) {}

    [[nodiscard]] std::optional<std::uint16_t> find(std::span<const Color> content) const
    {
        const auto it = index_.find(hash_colors(content, 0));
        if (it == index_.end() || !std::ranges::equal(slots_[it->second].content, content)) {
            return std::nullopt;
        }
        return it->second;
    }

    /// Stores `content` at the cursor (evicting what was there) and advances it.
    void insert(std::span<const Color> content)
    {
        if (slots_.empty()) {
            slots_.resize(capacity_);  // on first use
        }
        const auto slot_index = static_cast<std::uint16_t>(cursor_);
        Slot& slot = slots_[cursor_];
        if (slot.used) {
            if (const auto it = index_.find(slot.hash); it != index_.end() && it->second == slot_index) {
                index_.erase(it);
            }
        }
        slot.content.assign(content.begin(), content.end());
        slot.hash = hash_colors(content, 0);
        slot.used = true;
        index_[slot.hash] = slot_index;
        cursor_ = (cursor_ + 1) % slots_.size();
    }

    void clear()
    {
        slots_.clear();
        index_.clear();
        cursor_ = 0;
    }

private:
    struct Slot {
        std::vector<Color> content;
        std::uint64_t hash = 0;
        bool used = false;
    };
    std::size_t capacity_;
    std::vector<Slot> slots_;
    std::unordered_map<std::uint64_t, std::uint16_t> index_;
    std::size_t cursor_ = 0;
};

/// Encoder mirror of the Decompressor Glyph Storage ([MS-RDPEGFX] 3.3.1.9).
/// The server picks the slot, so this one evicts the least recently used.
class GlyphCache {
public:
    [[nodiscard]] std::optional<std::uint16_t> find(const Image& image)
    {
        const auto it = index_.find(hash_of(image));
        if (it == index_.end()) {
            return std::nullopt;
        }
        Slot& slot = slots_[it->second];
        if (slot.width != image.width || slot.height != image.height || slot.pixels != image.pixels) {
            return std::nullopt;
        }
        slot.last_use = ++clock_;
        return it->second;
    }

    [[nodiscard]] std::uint16_t insert(const Image& image)
    {
        if (slots_.empty()) {
            slots_.resize(glyph_cache_size);  // on first use
        }
        std::size_t victim = used_;
        if (used_ < slots_.size()) {
            ++used_;
        } else {
            victim = static_cast<std::size_t>(
                std::ranges::min_element(slots_, {}, [](const Slot& s) { return s.last_use; }) - slots_.begin());
            if (const auto it = index_.find(slots_[victim].hash);
                it != index_.end() && it->second == static_cast<std::uint16_t>(victim)) {
                index_.erase(it);
            }
        }
        Slot& slot = slots_[victim];
        slot.pixels = image.pixels;
        slot.width = image.width;
        slot.height = image.height;
        slot.hash = hash_of(image);
        slot.last_use = ++clock_;
        index_[slot.hash] = static_cast<std::uint16_t>(victim);
        return static_cast<std::uint16_t>(victim);
    }

    void clear()
    {
        slots_.clear();
        index_.clear();
        used_ = 0;
        clock_ = 0;
    }

private:
    struct Slot {
        std::vector<Color> pixels;
        std::size_t width = 0;
        std::size_t height = 0;
        std::uint64_t hash = 0;
        std::uint64_t last_use = 0;
    };

    [[nodiscard]] static std::uint64_t hash_of(const Image& image)
    {
        return hash_colors(image.pixels, (image.width << 16U) | image.height);
    }

    std::vector<Slot> slots_;
    std::unordered_map<std::uint64_t, std::uint16_t> index_;
    std::size_t used_ = 0;
    std::uint64_t clock_ = 0;
};

/// A rectangle of the region, half-open.
struct Area {
    std::size_t x0 = 0;
    std::size_t x1 = 0;
    std::size_t y0 = 0;
    std::size_t y1 = 0;

    [[nodiscard]] std::size_t width() const { return x1 - x0; }
    [[nodiscard]] std::size_t height() const { return y1 - y0; }
    [[nodiscard]] std::size_t pixels() const { return width() * height(); }
};

/// One column of a band split into the part that differs from the band
/// background (the Short V-bar) and the background around it.
struct Column {
    std::vector<Color> pixels;  // the whole V-bar, top to bottom
    std::size_t y_on = 0;       // shortVBarYOn
    std::size_t y_off = 0;      // shortVBarYOff (exclusive)

    [[nodiscard]] std::span<const Color> short_vbar() const { return std::span(pixels).subspan(y_on, y_off - y_on); }
};

void load_column(const Image& image, std::size_t x, std::size_t y0, std::size_t y1, Color background, Column& out)
{
    out.pixels.resize(y1 - y0);
    for (std::size_t y = y0; y < y1; ++y) {
        out.pixels[y - y0] = image.at(x, y);
    }
    const auto first = std::ranges::find_if(out.pixels, [&](Color c) { return c != background; });
    if (first == out.pixels.end()) {
        // All background: an empty Short V-bar.
        out.y_on = 0;
        out.y_off = 0;
        return;
    }
    const auto last =
        std::ranges::find_if(out.pixels.rbegin(), out.pixels.rend(), [&](Color c) { return c != background; });
    out.y_on = static_cast<std::size_t>(first - out.pixels.begin());
    out.y_off = out.pixels.size() - static_cast<std::size_t>(last - out.pixels.rbegin());
}

/// The layer chosen for one strip.
enum class Layer : std::uint8_t { residual, bands, rlex, raw };

}  // namespace

struct Encoder::State {
    ColumnCache vbars{vbar_cache_size};
    ColumnCache short_vbars{short_vbar_cache_size};
    GlyphCache glyphs;
    // Hashes of the V-bars and Short V-bars of every strip costed so far,
    // sent or not (second-chance admission, see bands_cost).
    std::unordered_set<std::uint64_t> seen_vbars;
    std::unordered_set<std::uint64_t> seen_short_vbars;
    std::uint8_t sequence = 0;
    bool cache_reset_pending = true;
    EncodeStats stats;

    // Scratch for one encode() call.
    Image image;
    std::vector<std::uint8_t> covered;  // pixels that bands or subcodecs paint
    Writer bands;
    Writer subcodecs;
    Column column;

    void reset()
    {
        vbars.clear();
        short_vbars.clear();
        glyphs.clear();
        seen_vbars.clear();
        seen_short_vbars.clear();
        sequence = 0;
        cache_reset_pending = true;
        stats = {};
    }

    void load(const ImageView& region);
    [[nodiscard]] std::vector<std::byte> encode_composite();
    void encode_strip(std::size_t y0, std::size_t y1);
    [[nodiscard]] Color dominant_color(const Area& area) const;
    [[nodiscard]] std::size_t residual_cost(const Area& area, Color background) const;
    [[nodiscard]] std::vector<Area> split_bands(const Area& area, Color background) const;
    [[nodiscard]] std::size_t bands_cost(std::span<const Area> band_areas, Color background);
    void write_band(const Area& band, Color background);
    [[nodiscard]] std::optional<Writer> encode_rlex(const Area& area) const;
    void write_subcodec(const Area& area, std::uint8_t id, std::span<const std::byte> data);
    [[nodiscard]] std::vector<std::byte> encode_residual() const;
    void cover(const Area& area);
};

void Encoder::State::load(const ImageView& region)
{
    image.width = region.width;
    image.height = region.height;
    image.pixels.resize(image.width * image.height);
    for (std::size_t y = 0; y < image.height; ++y) {
        const auto row = region.data.subspan(y * region.stride, image.width * bytes_per_pixel);
        for (std::size_t x = 0; x < image.width; ++x) {
            image.pixels[(y * image.width) + x] = load_pixel(row.subspan(x * bytes_per_pixel, bytes_per_pixel));
        }
    }
}

/// CLEARCODEC_COMPOSITE_PAYLOAD, [MS-RDPEGFX] 2.2.4.1.1.
std::vector<std::byte> Encoder::State::encode_composite()
{
    covered.assign(image.pixels.size(), 0);
    bands = Writer();
    subcodecs = Writer();

    // Strips: maximal runs of rows that are not one colour, at most
    // max_band_height rows each. Single-colour rows are the residual layer's.
    const auto single_color = [&](std::size_t y) {
        const auto row = image.row(y);
        return std::ranges::all_of(row, [&](Color c) { return c == row.front(); });
    };
    std::size_t y = 0;
    while (y < image.height) {
        if (single_color(y)) {
            ++y;
            continue;
        }
        std::size_t end = y + 1;
        while (end < image.height && !single_color(end)) {
            ++end;
        }
        for (; y < end; y += std::min<std::size_t>(max_band_height, end - y)) {
            encode_strip(y, std::min<std::size_t>(y + max_band_height, end));
        }
    }

    const auto residual = encode_residual();
    stats.residual_bytes = residual.size();
    stats.bands_bytes = bands.size();
    stats.subcodec_bytes = subcodecs.size();

    Writer w(composite_header_size + residual.size() + bands.size() + subcodecs.size());
    w.u32le(static_cast<std::uint32_t>(residual.size()));
    w.u32le(static_cast<std::uint32_t>(bands.size()));
    w.u32le(static_cast<std::uint32_t>(subcodecs.size()));
    w.bytes(residual);
    w.bytes(bands.view());
    w.bytes(subcodecs.view());
    return std::move(w).take();
}

/// Picks the cheapest layer for rows [y0, y1) and writes it, unless that is
/// the residual layer, which is written last.
void Encoder::State::encode_strip(std::size_t y0, std::size_t y1)
{
    const Color background = dominant_color({.x0 = 0, .x1 = image.width, .y0 = y0, .y1 = y1});
    // Trim background columns on both sides: the residual layer covers them
    // in the same run as the rows above and below.
    const auto background_column = [&](std::size_t x) {
        for (std::size_t y = y0; y < y1; ++y) {
            if (image.at(x, y) != background) {
                return false;
            }
        }
        return true;
    };
    Area area{.x0 = 0, .x1 = image.width, .y0 = y0, .y1 = y1};
    while (area.x0 < area.x1 && background_column(area.x0)) {
        ++area.x0;
    }
    while (area.x1 > area.x0 && background_column(area.x1 - 1)) {
        --area.x1;
    }
    FARLAND_ASSERT(area.x0 < area.x1);  // the rows hold more than one colour

    const auto band_areas = split_bands(area, background);
    const std::size_t residual = residual_cost(area, background);
    const std::size_t banded = bands_cost(band_areas, background);
    const auto rlex = encode_rlex(area);
    const std::size_t raw = subcodec_header_size + (3 * area.pixels());
    const std::size_t rlex_cost =
        rlex.has_value() ? subcodec_header_size + rlex->size() : std::numeric_limits<std::size_t>::max();

    Layer layer = Layer::residual;
    std::size_t best = residual;
    for (const auto& [candidate, cost] :
         {std::pair{Layer::bands, banded}, std::pair{Layer::rlex, rlex_cost}, std::pair{Layer::raw, raw}}) {
        if (cost < best) {
            layer = candidate;
            best = cost;
        }
    }

    switch (layer) {
    case Layer::residual:
        break;
    case Layer::bands:
        for (const Area& band : band_areas) {
            write_band(band, background);
            cover(band);
        }
        break;
    case Layer::rlex:
        if (rlex.has_value()) {  // always: rlex_cost is finite only then
            write_subcodec(area, subcodec_rlex, rlex->view());
            ++stats.rlex_subcodecs;
            cover(area);
        }
        break;
    case Layer::raw: {
        Writer data(3 * area.pixels());
        for (std::size_t y = area.y0; y < area.y1; ++y) {
            for (std::size_t x = area.x0; x < area.x1; ++x) {
                write_bgr(data, image.at(x, y));
            }
        }
        write_subcodec(area, subcodec_uncompressed, data.view());
        ++stats.raw_subcodecs;
        cover(area);
        break;
    }
    }
}

/// The most frequent colour of `area`; the first one to reach the highest
/// count wins, so the result does not depend on hash table order.
Color Encoder::State::dominant_color(const Area& area) const
{
    std::unordered_map<Color, std::size_t> counts;
    Color best = image.at(area.x0, area.y0);
    std::size_t best_count = 0;
    for (std::size_t y = area.y0; y < area.y1; ++y) {
        std::size_t x = area.x0;
        while (x < area.x1) {
            const Color c = image.at(x, y);
            std::size_t end = x + 1;
            while (end < area.x1 && image.at(end, y) == c) {
                ++end;
            }
            const std::size_t count = counts[c] += end - x;
            if (count > best_count) {
                best = c;
                best_count = count;
            }
            x = end;
        }
    }
    return best;
}

/// Roughly what `area` adds to the residual layer: one run segment per colour
/// change, reading each row from the background on its left.
std::size_t Encoder::State::residual_cost(const Area& area, Color background) const
{
    std::size_t cost = 0;
    for (std::size_t y = area.y0; y < area.y1; ++y) {
        Color previous = background;
        for (std::size_t x = area.x0; x < area.x1; ++x) {
            const Color c = image.at(x, y);
            if (c != previous) {
                cost += run_segment_size;
                previous = c;
            }
        }
    }
    return cost;
}

/// Cuts `area` into bands at runs of at least min_band_gap background columns.
std::vector<Area> Encoder::State::split_bands(const Area& area, Color background) const
{
    std::vector<Area> out;
    std::size_t start = area.x0;
    std::size_t gap = 0;  // background columns seen since the last other one
    for (std::size_t x = area.x0; x < area.x1; ++x) {
        bool is_background = true;
        for (std::size_t y = area.y0; y < area.y1 && is_background; ++y) {
            is_background = image.at(x, y) == background;
        }
        if (!is_background) {
            if (gap >= min_band_gap && x - gap > start) {
                out.push_back({.x0 = start, .x1 = x - gap, .y0 = area.y0, .y1 = area.y1});
                start = x;
            }
            gap = 0;
        } else {
            ++gap;
        }
    }
    out.push_back({.x0 = start, .x1 = area.x1, .y0 = area.y0, .y1 = area.y1});
    return out;
}

/// The size of `band_areas` as CLEARCODEC_BANDs against the current caches,
/// counting V-bars that an earlier column of the same bands would insert.
/// Leaves the caches alone.
///
/// Second-chance admission: a column whose V-bar or Short V-bar was costed
/// before (in an earlier strip or frame) but is not in the cache is charged
/// like a short V-bar hit instead of its full size. Content that recurs, such
/// as text in one font, is thereby put into the client's V-bar storage the
/// second time it shows up and costs 2 bytes per column from then on, while
/// content seen only once (a photo, noise) goes to the stateless layers and
/// does not evict useful V-bars.
std::size_t Encoder::State::bands_cost(std::span<const Area> band_areas, Color background)
{
    std::unordered_set<std::uint64_t> new_vbars;
    std::unordered_set<std::uint64_t> new_short_vbars;
    std::size_t cost = 0;
    for (const Area& band : band_areas) {
        cost += band_header_size;
        for (std::size_t x = band.x0; x < band.x1; ++x) {
            load_column(image, x, band.y0, band.y1, background, column);
            const auto short_vbar = column.short_vbar();
            const std::uint64_t vbar_hash = hash_colors(column.pixels, 0);
            const std::uint64_t short_hash = hash_colors(short_vbar, 0);
            const bool recurring = seen_vbars.contains(vbar_hash) || seen_short_vbars.contains(short_hash);
            seen_vbars.insert(vbar_hash);
            seen_short_vbars.insert(short_hash);

            if (vbars.find(column.pixels).has_value() || new_vbars.contains(vbar_hash)) {
                cost += vbar_hit_size;
                continue;
            }
            new_vbars.insert(vbar_hash);
            if (short_vbar.empty()) {
                cost += short_vbar_miss_size;
            } else if (short_vbars.find(short_vbar).has_value() || new_short_vbars.contains(short_hash) || recurring) {
                cost += short_vbar_hit_size;
            } else {
                cost += short_vbar_miss_size + (3 * short_vbar.size());
            }
            new_short_vbars.insert(short_hash);
        }
    }
    if (seen_vbars.size() > max_seen_columns || seen_short_vbars.size() > max_seen_columns) {
        seen_vbars.clear();
        seen_short_vbars.clear();
    }
    return cost;
}

/// One CLEARCODEC_BAND, [MS-RDPEGFX] 2.2.4.1.1.2.1, updating the caches as
/// the client will.
void Encoder::State::write_band(const Area& band, Color background)
{
    FARLAND_ASSERT(band.height() >= 1 && band.height() <= max_band_height);
    bands.u16le(static_cast<std::uint16_t>(band.x0));
    bands.u16le(static_cast<std::uint16_t>(band.x1 - 1));
    bands.u16le(static_cast<std::uint16_t>(band.y0));
    bands.u16le(static_cast<std::uint16_t>(band.y1 - 1));
    write_bgr(bands, background);
    ++stats.bands;

    for (std::size_t x = band.x0; x < band.x1; ++x) {
        load_column(image, x, band.y0, band.y1, background, column);
        if (const auto slot = vbars.find(column.pixels)) {
            // VBAR_CACHE_HIT, 2.2.4.1.1.2.1.1.1.
            bands.u16le(static_cast<std::uint16_t>(vbar_cache_hit | *slot));
            ++stats.vbar_hits;
            continue;
        }
        const auto short_vbar = column.short_vbar();
        const auto short_slot = short_vbar.empty() ? std::nullopt : short_vbars.find(short_vbar);
        if (short_slot.has_value()) {
            // SHORT_VBAR_CACHE_HIT, 2.2.4.1.1.2.1.1.2.
            bands.u16le(static_cast<std::uint16_t>(short_vbar_cache_hit | *short_slot));
            bands.u8(static_cast<std::uint8_t>(column.y_on));
            ++stats.short_vbar_hits;
        } else {
            // SHORT_VBAR_CACHE_MISS, 2.2.4.1.1.2.1.1.3: shortVBarYOn in the
            // low byte, shortVBarYOff in the next 6 bits, the top bits 0.
            bands.u16le(static_cast<std::uint16_t>((column.y_off << 8U) | column.y_on));
            for (const Color c : short_vbar) {
                write_bgr(bands, c);
            }
            short_vbars.insert(short_vbar);
            ++stats.short_vbar_misses;
        }
        // Both short forms also store the whole V-bar at the V-bar cursor.
        vbars.insert(column.pixels);
    }
}

/// CLEARCODEC_SUBCODEC_RLEX for `area` ([MS-RDPEGFX] 2.2.4.1.1.3.1.1), or
/// nothing if it has more than max_palette_size colours or would be larger
/// than the raw pixels (bitmapDataByteCount must not exceed 3 * width *
/// height, 2.2.4.1.1.3.1).
std::optional<Writer> Encoder::State::encode_rlex(const Area& area) const
{
    // Palette in order of first appearance, which turns the first pass over
    // an anti-aliased edge into suites.
    std::vector<Color> palette;
    std::unordered_map<Color, std::uint8_t> lookup;
    std::vector<std::uint8_t> indices;
    indices.reserve(area.pixels());
    for (std::size_t y = area.y0; y < area.y1; ++y) {
        for (std::size_t x = area.x0; x < area.x1; ++x) {
            const Color c = image.at(x, y);
            auto [it, inserted] = lookup.try_emplace(c, static_cast<std::uint8_t>(palette.size()));
            if (inserted) {
                if (palette.size() == max_palette_size) {
                    return std::nullopt;
                }
                palette.push_back(c);
            }
            indices.push_back(it->second);
        }
    }

    const unsigned index_bits = rlex_index_bits(palette.size());
    const std::size_t max_depth = (std::size_t{1} << (8U - index_bits)) - 1;
    Writer w;
    w.u8(static_cast<std::uint8_t>(palette.size()));
    for (const Color c : palette) {
        write_bgr(w, c);
    }
    // Each CLEARCODEC_SUBCODEC_RLEX_SEGMENT is runLengthFactor copies of the
    // start index, then the suite start, start + 1, ..., stop.
    std::size_t i = 0;
    while (i < indices.size()) {
        const std::size_t start = indices[i];
        std::size_t next = i + 1;
        while (next < indices.size() && indices[next] == start) {
            ++next;
        }
        const std::size_t run = next - i - 1;
        std::size_t stop = start;
        while (next < indices.size() && stop - start < max_depth && indices[next] == stop + 1) {
            ++stop;
            ++next;
        }
        w.u8(static_cast<std::uint8_t>(((stop - start) << index_bits) | stop));
        write_run_length(w, run);
        i = next;
        if (w.size() > 3 * area.pixels()) {
            return std::nullopt;
        }
    }
    return w;
}

/// One CLEARCODEC_SUBCODEC, [MS-RDPEGFX] 2.2.4.1.1.3.1.
void Encoder::State::write_subcodec(const Area& area, std::uint8_t id, std::span<const std::byte> data)
{
    subcodecs.u16le(static_cast<std::uint16_t>(area.x0));
    subcodecs.u16le(static_cast<std::uint16_t>(area.y0));
    subcodecs.u16le(static_cast<std::uint16_t>(area.width()));
    subcodecs.u16le(static_cast<std::uint16_t>(area.height()));
    subcodecs.u32le(static_cast<std::uint32_t>(data.size()));
    subcodecs.u8(id);
    subcodecs.bytes(data);
}

/// CLEARCODEC_RESIDUAL_DATA, [MS-RDPEGFX] 2.2.4.1.1.1, covering every pixel
/// (FreeRDP rejects less). Pixels that a later layer paints take whatever
/// colour the current run has. Empty if later layers paint everything.
std::vector<std::byte> Encoder::State::encode_residual() const
{
    const auto first = std::ranges::find(covered, std::uint8_t{0});
    if (first == covered.end()) {
        return {};
    }
    Writer w;
    Color current = image.pixels[static_cast<std::size_t>(first - covered.begin())];
    std::size_t run = 0;
    for (std::size_t i = 0; i < image.pixels.size(); ++i) {
        if (covered[i] == 0 && image.pixels[i] != current) {
            write_bgr(w, current);
            write_run_length(w, run);
            current = image.pixels[i];
            run = 0;
        }
        ++run;
    }
    write_bgr(w, current);
    write_run_length(w, run);
    return std::move(w).take();
}

void Encoder::State::cover(const Area& area)
{
    for (std::size_t y = area.y0; y < area.y1; ++y) {
        std::ranges::fill(std::span(covered).subspan((y * image.width) + area.x0, area.width()), std::uint8_t{1});
    }
}

Encoder::Encoder() : state_(std::make_unique<State>()) {}
Encoder::~Encoder() = default;
Encoder::Encoder(Encoder&&) noexcept = default;
Encoder& Encoder::operator=(Encoder&&) noexcept = default;

std::vector<std::byte> Encoder::encode(const ImageView& region, const EncodeOptions& options)
{
    FARLAND_ASSERT(region.width >= 1 && region.width <= max_dimension);
    FARLAND_ASSERT(region.height >= 1 && region.height <= max_dimension);
    const std::size_t row_bytes = std::size_t{region.width} * bytes_per_pixel;
    FARLAND_ASSERT(region.stride >= row_bytes);
    FARLAND_ASSERT(region.data.size() >= row_bytes &&
                   (region.data.size() - row_bytes) / region.stride >= region.height - 1);

    State& s = *state_;
    s.stats = {};
    s.load(region);

    // CLEARCODEC_BITMAP_STREAM header, [MS-RDPEGFX] 2.2.4.1.
    std::uint8_t flags = 0;
    if (s.cache_reset_pending) {
        flags |= flag_cache_reset;
        s.cache_reset_pending = false;
    }
    std::optional<std::uint16_t> glyph_index;
    if (options.glyph_cache && s.image.pixels.size() <= max_glyph_pixels) {
        glyph_index = s.glyphs.find(s.image);
        if (glyph_index.has_value()) {
            flags |= flag_glyph_index | flag_glyph_hit;
            s.stats.glyph_hit = true;
        } else {
            glyph_index = s.glyphs.insert(s.image);
            flags |= flag_glyph_index;
            s.stats.glyph_stored = true;
        }
    }

    Writer w;
    w.u8(flags);
    w.u8(s.sequence);
    s.sequence = static_cast<std::uint8_t>(s.sequence + 1);
    if (glyph_index.has_value()) {
        w.u16le(*glyph_index);
    }
    std::vector<std::byte> header = std::move(w).take();
    if (s.stats.glyph_hit) {
        return header;
    }
    // One allocation of the final size and two copies, rather than appending
    // to the header: GCC 14 to 16 report a memset of SIZE_MAX bytes in the
    // inlined vector::resize of Writer::bytes here at -O3.
    const std::vector<std::byte> composite = s.encode_composite();
    std::vector<std::byte> out(header.size() + composite.size());
    const auto rest = std::ranges::copy(header, out.begin()).out;
    std::ranges::copy(composite, rest);
    return out;
}

void Encoder::reset()
{
    state_->reset();
}

const EncodeStats& Encoder::last_stats() const noexcept
{
    return state_->stats;
}

std::uint8_t Encoder::next_sequence_number() const noexcept
{
    return state_->sequence;
}

}  // namespace farland::codec::clear
