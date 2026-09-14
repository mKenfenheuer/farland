// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/server/scroll_detector.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <unordered_map>
#include <vector>

namespace farland::server {

namespace {

constexpr std::size_t bytes_per_pixel = 4;

std::span<const std::byte> row_bytes(const codec::ImageView& image, std::uint32_t y, std::uint32_t x,
                                     std::uint32_t width)
{
    return image.data.subspan((std::size_t{y} * image.stride) + (std::size_t{x} * bytes_per_pixel),
                              std::size_t{width} * bytes_per_pixel);
}

/// Up to eight bytes as one word.
std::uint64_t load(std::span<const std::byte> bytes)
{
    std::array<std::byte, 8> word{};
    std::ranges::copy(bytes, word.begin());
    return std::bit_cast<std::uint64_t>(word);
}

std::uint64_t hash_row(std::span<const std::byte> bytes)
{
    std::uint64_t h = 0x243F6A8885A308D3U;
    while (!bytes.empty()) {
        const std::size_t n = std::min<std::size_t>(bytes.size(), 8);
        h = (h ^ load(bytes.first(n))) * 0x9E3779B97F4A7C15U;
        h ^= h >> 29U;
        bytes = bytes.subspan(n);
    }
    return h;
}

/// One column strip of the area: a hash per row, before and now.
struct Strip {
    std::uint32_t x = 0;
    std::uint32_t width = 0;
    std::vector<std::uint64_t> previous;
    std::vector<std::uint64_t> current;
};

}  // namespace

std::optional<ScrollMove> detect_vertical_scroll(const codec::ImageView& previous, const codec::ImageView& current,
                                                 const PixelRect& area, const ScrollDetectorConfig& config)
{
    FARLAND_ASSERT(previous.width == current.width && previous.height == current.height);
    const std::uint32_t x0 = std::min(area.x, current.width);
    const std::uint32_t y0 = std::min(area.y, current.height);
    const std::uint32_t width = std::min(area.width, current.width - x0);
    const std::uint32_t rows = std::min(area.height, current.height - y0);
    if (config.strip_width == 0 || width < std::max(config.min_width, 1U) || rows <= config.min_rows) {
        return std::nullopt;
    }
    const std::int64_t max_distance =
        config.max_distance == 0 ? std::int64_t{rows} - 1 : std::min<std::int64_t>(config.max_distance, rows - 1);

    std::vector<Strip> strips;
    for (std::uint32_t x = x0; x < x0 + width; x += config.strip_width) {
        Strip strip{x, std::min(config.strip_width, x0 + width - x), {}, {}};
        strip.previous.reserve(rows);
        strip.current.reserve(rows);
        for (std::uint32_t row = 0; row < rows; ++row) {
            strip.previous.push_back(hash_row(row_bytes(previous, y0 + row, strip.x, strip.width)));
            strip.current.push_back(hash_row(row_bytes(current, y0 + row, strip.x, strip.width)));
        }
        strips.push_back(std::move(strip));
    }

    // 1. Every changed row whose content occurs exactly once among the
    // strip's previous rows votes for the distance it moved. Repeated rows
    // (blank lines, solid backgrounds) are ambiguous and do not vote.
    std::unordered_map<std::int64_t, std::uint32_t> votes;
    std::unordered_map<std::uint64_t, std::int64_t> where;  // hash → row, -1 when it repeats
    for (const Strip& strip : strips) {
        where.clear();
        for (std::uint32_t row = 0; row < rows; ++row) {
            const auto [it, inserted] = where.try_emplace(strip.previous[row], row);
            if (!inserted) {
                it->second = -1;
            }
        }
        for (std::uint32_t row = 0; row < rows; ++row) {
            if (strip.current[row] == strip.previous[row]) {
                continue;
            }
            const auto it = where.find(strip.current[row]);
            if (it == where.end() || it->second < 0) {
                continue;
            }
            const std::int64_t distance = std::int64_t{row} - it->second;
            if (distance != 0 && std::abs(distance) <= max_distance) {
                ++votes[distance];
            }
        }
    }
    std::int64_t dy = 0;
    std::uint32_t most = 0;
    for (const auto& [distance, count] : votes) {
        // Deterministic among ties: the shorter distance, then the upward one.
        const bool shorter = std::abs(distance) < std::abs(dy) || (std::abs(distance) == std::abs(dy) && distance < dy);
        if (count > most || (count == most && shorter)) {
            dy = distance;
            most = count;
        }
    }
    if (most < config.min_rows) {
        return std::nullopt;
    }

    // 2. The strips that moved by dy: most of the rows that can have moved
    // (whose source lies inside the area) did.
    const auto moved = [&](const Strip& strip, std::uint32_t row) {
        const std::int64_t source = std::int64_t{row} - dy;
        return source >= 0 && source < rows && strip.current[row] == strip.previous[static_cast<std::size_t>(source)];
    };
    const std::uint64_t possible = rows - static_cast<std::uint64_t>(std::abs(dy));
    std::vector<bool> good;
    good.reserve(strips.size());
    for (const Strip& strip : strips) {
        std::uint64_t count = 0;
        for (std::uint32_t row = 0; row < rows; ++row) {
            count += moved(strip, row) ? 1U : 0U;
        }
        good.push_back(count * 2 >= possible);
    }

    // 3. For each run of adjacent good strips, the longest run of rows that
    // moved across all of them, verified byte by byte; the largest wins.
    std::optional<ScrollMove> result;
    std::uint64_t best_area = 0;
    for (std::size_t first = 0; first < strips.size();) {
        if (!good[first]) {
            ++first;
            continue;
        }
        std::size_t last = first;
        while (last + 1 < strips.size() && good[last + 1]) {
            ++last;
        }
        const std::uint32_t gx = strips[first].x;
        const std::uint32_t gw = strips[last].x + strips[last].width - gx;
        if (gw >= config.min_width) {
            const auto group = std::span(strips).subspan(first, last - first + 1);
            std::uint32_t run = 0;
            std::uint32_t run_start = 0;
            std::uint32_t best_run = 0;
            std::uint32_t best_start = 0;
            for (std::uint32_t row = 0; row < rows; ++row) {
                const bool ok =
                    std::ranges::all_of(group, [&](const Strip& strip) { return moved(strip, row); }) &&
                    std::ranges::equal(
                        row_bytes(current, y0 + row, gx, gw),
                        row_bytes(previous, static_cast<std::uint32_t>(std::int64_t{y0} + row - dy), gx, gw));
                if (!ok) {
                    run = 0;
                    continue;
                }
                if (run == 0) {
                    run_start = row;
                }
                ++run;
                if (run > best_run) {
                    best_run = run;
                    best_start = run_start;
                }
            }
            const std::uint64_t moved_area = std::uint64_t{best_run} * gw;
            if (best_run >= config.min_rows && moved_area > best_area) {
                best_area = moved_area;
                const auto source_y = static_cast<std::uint32_t>(std::int64_t{y0} + best_start - dy);
                result = ScrollMove{{gx, source_y, gw, best_run}, static_cast<std::int32_t>(dy)};
            }
        }
        first = last + 1;
    }
    return result;
}

void apply_scroll(std::span<std::byte> image, std::size_t stride, const ScrollMove& move)
{
    const std::size_t width_bytes = std::size_t{move.source.width} * bytes_per_pixel;
    const std::size_t x_offset = std::size_t{move.source.x} * bytes_per_pixel;
    const std::uint32_t destination_y = move.destination().y;
    const auto copy_row = [&](std::uint32_t i) {
        const auto source = image.subspan(((std::size_t{move.source.y} + i) * stride) + x_offset, width_bytes);
        std::ranges::copy(source, image.subspan(((std::size_t{destination_y} + i) * stride) + x_offset).begin());
    };
    // Like memmove: rows the copy still has to read are not overwritten first.
    if (move.dy > 0) {
        for (std::uint32_t i = move.source.height; i-- > 0;) {
            copy_row(i);
        }
    } else {
        for (std::uint32_t i = 0; i < move.source.height; ++i) {
            copy_row(i);
        }
    }
}

}  // namespace farland::server
