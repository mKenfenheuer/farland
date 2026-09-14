// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/base/log.hpp>
#include <farland/server/connection.hpp>
#include <farland/server/cursor_encoder.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace farland::server {

namespace {

namespace ptr = proto::pointer;
using platform::CursorImage;

constexpr std::string_view log_component = "server.cursor";
/// Room for the slow-path TS_POINTER_PDU header (messageType, pad2Octets).
constexpr std::size_t slow_path_header = 4;
/// Bytes before the masks, and the pad after them ([MS-RDPBCGR] 2.2.9.1.1.4.4,
/// 2.2.9.1.1.4.5, 2.2.9.1.2.1.11).
constexpr std::size_t color_overhead = 14 + 1;
constexpr std::size_t new_overhead = 2 + 14 + 1;
constexpr std::size_t large_overhead = 20 + 1;

std::uint64_t fnv1a(const CursorImage& image)
{
    std::uint64_t h = 0xcbf29ce484222325ULL;
    const auto mix = [&h](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h = (h ^ (v & 0xFFU)) * 0x100000001b3ULL;
            v >>= 8U;
        }
    };
    mix(image.width);
    mix(image.height);
    mix(static_cast<std::uint32_t>(image.hotspot_x));
    mix(static_cast<std::uint32_t>(image.hotspot_y));
    for (const std::byte b : image.pixels) {
        h = (h ^ std::to_integer<std::uint64_t>(b)) * 0x100000001b3ULL;
    }
    return h;
}

std::uint16_t to_u16(std::int32_t v) noexcept
{
    return static_cast<std::uint16_t>(std::clamp<std::int32_t>(v, 0, std::numeric_limits<std::uint16_t>::max()));
}

/// Clamps the hotspot into the image and turns an empty image into one
/// transparent pixel. Nullopt when the pixels do not match the size.
std::optional<CursorImage> normalize(const CursorImage& in)
{
    if (in.width == 0 || in.height == 0) {
        return CursorImage{1, 1, 0, 0, std::vector<std::byte>(4)};
    }
    const std::uint64_t expected = std::uint64_t{in.width} * in.height * 4U;
    if (in.pixels.size() != expected) {
        return std::nullopt;
    }
    CursorImage out = in;
    // Never above the int32 hotspot it replaces, so the cast is exact.
    out.hotspot_x = static_cast<std::int32_t>(std::clamp<std::int64_t>(in.hotspot_x, 0, std::int64_t{in.width} - 1));
    out.hotspot_y = static_cast<std::int32_t>(std::clamp<std::int64_t>(in.hotspot_y, 0, std::int64_t{in.height} - 1));
    return out;
}

/// The size that fits `width` x `height` into a `limit` square, keeping the aspect ratio.
std::pair<std::uint32_t, std::uint32_t> fit(std::uint32_t width, std::uint32_t height, std::uint32_t limit)
{
    if (width <= limit && height <= limit) {
        return {width, height};
    }
    const auto scaled = [limit](std::uint32_t side, std::uint32_t longest) {
        const auto v = static_cast<std::uint32_t>(std::lround(static_cast<double>(side) * limit / longest));
        return std::clamp<std::uint32_t>(v, 1, limit);
    };
    if (width >= height) {
        return {limit, scaled(height, width)};
    }
    return {scaled(width, height), limit};
}

std::size_t update_size(std::uint8_t kind_bpp, std::size_t overhead, std::uint32_t width, std::uint32_t height)
{
    const auto w = static_cast<std::uint16_t>(width);
    return overhead + ((ptr::xor_stride(w, kind_bpp) + ptr::and_stride(w)) * height);
}

/// Source pixels that contribute to each destination pixel along one axis,
/// with their coverage.
struct Tap {
    std::uint32_t index = 0;
    double weight = 0;
};

std::vector<std::vector<Tap>> taps(std::uint32_t source, std::uint32_t target)
{
    std::vector<std::vector<Tap>> out(target);
    const double ratio = static_cast<double>(source) / target;
    for (std::uint32_t d = 0; d < target; ++d) {
        const double begin = d * ratio;
        const double end = (d + 1) * ratio;
        const auto first = static_cast<std::uint32_t>(begin);
        for (std::uint32_t s = first; s < source && s < end; ++s) {
            const double weight = std::min<double>(end, s + 1.0) - std::max<double>(begin, s);
            if (weight > 0) {
                out[d].push_back({s, weight});
            }
        }
    }
    return out;
}

/// Area-average downscale of straight-alpha BGRA, averaging in premultiplied alpha.
std::vector<std::byte> scale(const CursorImage& source, std::uint32_t width, std::uint32_t height)
{
    const auto xs = taps(source.width, width);
    const auto ys = taps(source.height, height);
    std::vector<std::byte> out(std::size_t{width} * height * 4U);
    const auto at = [&source](std::uint32_t x, std::uint32_t y, std::size_t c) {
        return std::to_integer<unsigned>(source.pixels[((std::size_t{y} * source.width + x) * 4U) + c]);
    };
    const auto to_byte = [](double v) {
        return std::byte{static_cast<std::uint8_t>(std::clamp(std::lround(v), 0L, 255L))};
    };
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            double b = 0;
            double g = 0;
            double r = 0;
            double a = 0;
            double area = 0;
            for (const Tap& ty : ys[y]) {
                for (const Tap& tx : xs[x]) {
                    const double w = ty.weight * tx.weight;
                    const double alpha = at(tx.index, ty.index, 3) * w;
                    b += at(tx.index, ty.index, 0) * alpha;
                    g += at(tx.index, ty.index, 1) * alpha;
                    r += at(tx.index, ty.index, 2) * alpha;
                    a += alpha;
                    area += w;
                }
            }
            const std::size_t o = ((std::size_t{y} * width) + x) * 4U;
            if (a > 0) {
                out[o] = to_byte(b / a);
                out[o + 1] = to_byte(g / a);
                out[o + 2] = to_byte(r / a);
                out[o + 3] = to_byte(a / area);
            }
        }
    }
    return out;
}

/// Where the hotspot pixel lands after scaling `from` pixels to `to`.
std::uint16_t scale_hotspot(std::int32_t hotspot, std::uint32_t from, std::uint32_t to)
{
    if (from == to) {
        return static_cast<std::uint16_t>(hotspot);
    }
    const auto v = static_cast<std::uint32_t>((hotspot + 0.5) * to / from);
    return static_cast<std::uint16_t>(std::min(v, to - 1));
}

}  // namespace

CursorEncoder::Config CursorEncoder::Config::negotiated(const Session& session, std::size_t max_update_size)
{
    namespace flags = proto::caps::large_pointer_flags;
    Config c;
    c.pointer_cache_size = session.pointer.pointer_cache_size;
    c.color_pointer_cache_size = session.pointer.color_pointer_cache_size;
    const std::uint16_t large = session.pointer.large_pointer_flags;
    c.max_size = (large & flags::size_96x96) != 0 ? ptr::max_size : ptr::max_legacy_size;
    c.large_pointers = (large & flags::size_384x384) != 0 && session.fastpath_output && c.pointer_cache_size > 0;
    c.max_update_size = max_update_size;
    return c;
}

CursorEncoder::CursorEncoder(Config config) : config_(config), slots_(cache_size()) {}

std::uint16_t CursorEncoder::cache_size() const noexcept
{
    return config_.pointer_cache_size > 0 ? config_.pointer_cache_size : config_.color_pointer_cache_size;
}

std::optional<CursorEncoder::Layout> CursorEncoder::choose_layout(std::uint32_t width, std::uint32_t height) const
{
    if (cache_size() == 0) {
        return std::nullopt;  // the client has nowhere to put a shape
    }
    const bool color = config_.pointer_cache_size == 0;
    const std::uint32_t small = std::clamp<std::uint32_t>(config_.max_size, 1, ptr::max_size);
    const bool large = config_.large_pointers && !color;
    for (std::uint32_t limit = large ? ptr::max_large_size : small; limit > 0; --limit) {
        const auto [w, h] = fit(width, height, limit);
        Layout layout{color ? Kind::color : Kind::new_pointer, w, h};
        std::size_t size = 0;
        if (color) {
            size = update_size(24, color_overhead, w, h);
        } else if (w > small || h > small) {
            layout.kind = Kind::large;
            size = update_size(32, large_overhead, w, h);
        } else {
            size = update_size(32, new_overhead, w, h);
        }
        if (size + slow_path_header <= config_.max_update_size) {
            return layout;
        }
    }
    return std::nullopt;
}

bool CursorEncoder::slot_holds(std::size_t index, const CursorImage& shape) const
{
    const Slot& slot = slots_.at(index);
    return slot.used && slot.hash == shape_hash_ && slot.image == shape;
}

std::size_t CursorEncoder::take_slot()
{
    const auto free = std::ranges::find_if(slots_, [](const Slot& s) { return !s.used; });
    if (free != slots_.end()) {
        return static_cast<std::size_t>(free - slots_.begin());
    }
    const auto oldest = std::ranges::min_element(slots_, {}, &Slot::last_used);
    return static_cast<std::size_t>(oldest - slots_.begin());
}

proto::PointerUpdate CursorEncoder::build(const CursorImage& source, const Layout& layout, std::uint16_t cache_index)
{
    const bool native = layout.width == source.width && layout.height == source.height;
    const std::vector<std::byte> scaled =
        native ? std::vector<std::byte>{} : scale(source, layout.width, layout.height);
    ptr::Shape shape =
        ptr::shape_from_bgra(native ? source.pixels : scaled, static_cast<std::uint16_t>(layout.width),
                             static_cast<std::uint16_t>(layout.height), layout.kind == Kind::color ? 24 : 32);
    shape.cache_index = cache_index;
    shape.hotspot_x = scale_hotspot(source.hotspot_x, source.width, layout.width);
    shape.hotspot_y = scale_hotspot(source.hotspot_y, source.height, layout.height);
    switch (layout.kind) {
    case Kind::color:
        return ptr::ColorPointer{std::move(shape)};
    case Kind::large:
        return ptr::LargePointer{std::move(shape)};
    case Kind::new_pointer:
        break;
    }
    return ptr::NewPointer{std::move(shape)};
}

void CursorEncoder::show_shape(std::vector<proto::PointerUpdate>& out, const CursorImage& shape)
{
    if (shown_ == Shown::slot && slot_holds(shown_slot_, shape)) {
        slots_.at(shown_slot_).last_used = ++tick_;
        return;
    }
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        if (slot_holds(i, shape)) {
            slots_[i].last_used = ++tick_;
            out.emplace_back(ptr::CachedPointer{static_cast<std::uint16_t>(i)});
            shown_ = Shown::slot;
            shown_slot_ = i;
            return;
        }
    }
    const auto layout = choose_layout(shape.width, shape.height);
    if (!layout) {
        log::debug(log_component, "the client cannot take a {}x{} pointer; showing its default", shape.width,
                   shape.height);
        show_system(out, Shown::system_default);
        return;
    }
    const std::size_t index = take_slot();
    out.push_back(build(shape, *layout, static_cast<std::uint16_t>(index)));
    slots_[index] = Slot{true, shape_hash_, ++tick_, shape};
    shown_ = Shown::slot;
    shown_slot_ = index;
}

void CursorEncoder::show_system(std::vector<proto::PointerUpdate>& out, Shown target)
{
    FARLAND_ASSERT(target != Shown::slot);
    if (shown_ == target) {
        return;
    }
    if (target == Shown::hidden) {
        out.emplace_back(ptr::Hidden{});
    } else {
        out.emplace_back(ptr::Default{});
    }
    shown_ = target;
}

std::vector<proto::PointerUpdate> CursorEncoder::encode(const platform::CursorUpdate& update)
{
    std::vector<proto::PointerUpdate> out;
    if (update.shape) {
        if (auto image = normalize(*update.shape)) {
            shape_ = std::move(*image);
            shape_hash_ = fnv1a(*shape_);
        } else {
            log::warn(log_component, "ignoring a {}x{} cursor with {} bytes of pixels", update.shape->width,
                      update.shape->height, update.shape->pixels.size());
        }
    }
    visible_ = update.visible;
    if (!visible_) {
        show_system(out, Shown::hidden);
    } else if (shape_) {
        show_shape(out, *shape_);
    } else {
        show_system(out, Shown::system_default);
    }
    if (update.position) {
        if (config_.send_positions && position_ != update.position) {
            out.emplace_back(ptr::Position{to_u16(update.position->first), to_u16(update.position->second)});
        }
        position_ = update.position;
    }
    return out;
}

proto::PointerUpdate CursorEncoder::warp(std::int32_t x, std::int32_t y)
{
    position_.emplace(x, y);
    return ptr::Position{to_u16(x), to_u16(y)};
}

void CursorEncoder::client_moved(std::int32_t x, std::int32_t y)
{
    position_.emplace(x, y);
}

std::vector<proto::PointerUpdate> CursorEncoder::reset(Config config)
{
    config_ = config;
    slots_.assign(cache_size(), Slot{});
    tick_ = 0;
    shown_ = Shown::system_default;
    std::vector<proto::PointerUpdate> out;
    if (!visible_) {
        show_system(out, Shown::hidden);
    } else if (shape_) {
        show_shape(out, *shape_);
    }
    if (config_.send_positions && position_) {
        out.emplace_back(ptr::Position{to_u16(position_->first), to_u16(position_->second)});
    }
    return out;
}

}  // namespace farland::server
