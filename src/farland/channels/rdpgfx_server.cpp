// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/channels/rdpgfx_server.hpp>

#include <algorithm>
#include <array>
#include <limits>

namespace farland::channels::rdpgfx {

namespace {

/// Highest first. 10.6's errata value only if the client lacks the real one.
constexpr std::array preferred_versions = {
    cap_version::v10_7, cap_version::v10_6, cap_version::v10_6_err, cap_version::v10_5,
    cap_version::v10_4, cap_version::v10_3, cap_version::v10_2,     cap_version::v10_1,
    cap_version::v10,   cap_version::v8_1,  cap_version::v8,
};

constexpr std::uint32_t bytes_per_pixel = 4;

Negotiated derive(std::uint32_t version, std::uint32_t client_flags, const GfxServerConfig& config)
{
    using namespace caps_flag;
    Negotiated n;
    n.version = version;
    n.client_flags = client_flags;
    // Restrictions the client asks for are always confirmed; bits the version
    // does not define are dropped (docs/PLAN.md §3.2 rule 5).
    std::uint32_t flags = client_flags & defined_caps_flags(version);
    const bool v10 = version >= cap_version::v10;

    n.thin_client = !v10 && (flags & thin_client) != 0;
    // [MS-RDPEGFX] 3.3.1.4: 16 MB with THINCLIENT or SMALL_CACHE, and for 10.3.
    n.small_cache = (flags & (thin_client | small_cache)) != 0 || version == cap_version::v10_3;

    if (version == cap_version::v8_1) {
        n.avc420 = config.avc420 && (flags & avc420_enabled) != 0;
        if (!n.avc420) {
            flags &= ~avc420_enabled;  // the server will not send AVC420, as FreeRDP's shadow server does
        }
    }
    if (v10) {
        // [MS-RDPEGFX] 2.2.3.3: without AVC_DISABLED the client "MUST be
        // capable of processing" AVC444; 10.1 has no flags at all.
        const bool client_avc = (flags & avc_disabled) == 0;
        n.avc444 = client_avc && config.avc444;
        n.avc444v2 = client_avc && config.avc444v2;
        n.avc420 = client_avc && config.avc420 && version >= cap_version::v10_4;
        n.avc_thin_client = (flags & avc_thin_client) != 0;
        n.qoe = version != cap_version::v10_1;
    }
    n.scaled_output = version == cap_version::v10_7 && (flags & scaledmap_disable) == 0;
    n.max_cache_slots = n.small_cache ? max_cache_slots_small : max_cache_slots;
    n.max_cache_bytes = n.small_cache ? cache_size_small : cache_size;
    n.flags = flags;
    return n;
}

bool inside(const Rect16& rect, std::uint16_t width, std::uint16_t height)
{
    return !rect.empty() && rect.right <= width && rect.bottom <= height;
}

bool fits(const Point16& point, std::uint16_t width, std::uint16_t height, std::uint16_t dest_width,
          std::uint16_t dest_height)
{
    return point.x >= 0 && point.y >= 0 && point.x + width <= dest_width && point.y + height <= dest_height;
}

}  // namespace

bool Negotiated::allows(std::uint16_t codec_id) const noexcept
{
    switch (codec_id) {
    case codec::uncompressed:
    case codec::cavideo:
    case codec::clearcodec:
    case codec::planar:
    case codec::alpha:
        return true;
    case codec::progressive:
        return !thin_client;  // THINCLIENT: RemoteFX "MUST be used in place of" Progressive
    case codec::avc420:
        return avc420;
    case codec::avc444:
        return avc444;
    case codec::avc444v2:
        return avc444v2;
    default:
        return false;
    }
}

std::optional<Negotiated> negotiate(const CapsAdvertise& advertise, const GfxServerConfig& config)
{
    for (const std::uint32_t version : preferred_versions) {
        if (version > config.max_version) {
            continue;
        }
        const auto it = std::ranges::find(advertise.caps_sets, version, &CapabilitySet::version);
        if (it != advertise.caps_sets.end()) {
            return derive(version, it->flags, config);
        }
    }
    return std::nullopt;
}

GfxServer::GfxServer(GfxServerConfig config) : config_(config) {}

void GfxServer::receive(std::span<const std::byte> bytes)
{
    if (state_ == GfxState::failed) {
        return;
    }
    input_.insert(input_.end(), bytes.begin(), bytes.end());
    std::size_t consumed = 0;
    while (state_ != GfxState::failed) {
        const auto rest = std::span<const std::byte>(input_).subspan(consumed);
        const auto framed = frame_pdu(rest, max_client_pdu_size);
        if (!framed) {
            fail("malformed RDPGFX header: " + framed.error().message());
            break;
        }
        if (!*framed) {
            break;
        }
        const std::size_t size = **framed;
        handle_pdu(rest.first(size));
        consumed += size;
    }
    if (state_ == GfxState::failed) {
        input_.clear();
        return;
    }
    input_.erase(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(consumed));
}

std::vector<std::vector<std::byte>> GfxServer::take_output()
{
    return std::exchange(output_, {});
}

std::optional<GfxEvent> GfxServer::poll_event()
{
    if (events_.empty()) {
        return std::nullopt;
    }
    GfxEvent event = std::move(events_.front());
    events_.pop_front();
    return event;
}

void GfxServer::handle_pdu(std::span<const std::byte> bytes)
{
    const auto pdu = decode_pdu(bytes);
    if (!pdu) {
        fail("malformed RDPGFX PDU: " + pdu.error().message());
        return;
    }
    if (const auto* advertise = std::get_if<CapsAdvertise>(&*pdu)) {
        on_caps_advertise(*advertise);
        return;
    }
    // [MS-RDPEGFX] 1.7: capabilities are exchanged before anything else.
    // FreeRDP's server rejects these before Caps Confirm as well.
    if (state_ != GfxState::ready) {
        fail("RDPGFX PDU before Caps Advertise");
        return;
    }
    if (const auto* ack = std::get_if<FrameAcknowledge>(&*pdu)) {
        on_frame_acknowledge(*ack);
    } else if (const auto* qoe = std::get_if<QoeFrameAcknowledge>(&*pdu)) {
        on_qoe_frame_acknowledge(*qoe);
    } else if (const auto* offer = std::get_if<CacheImportOffer>(&*pdu)) {
        on_cache_import_offer(*offer);
    } else {
        fail("server-to-client RDPGFX PDU received from the client");
    }
}

void GfxServer::on_caps_advertise(const CapsAdvertise& advertise)
{
    const bool reset = state_ == GfxState::ready;
    if (reset) {
        // [MS-RDPEGFX] 3.2.5.18: only allowed after confirming 10.3 or later.
        if (!negotiated_ || negotiated_->version < cap_version::v10_3) {
            fail("Caps Advertise repeated after confirming a version before 10.3");
            return;
        }
        reset_protocol();
    }
    const auto negotiated = negotiate(advertise, config_);
    if (!negotiated) {
        // [MS-RDPEGFX] 3.2.5.19: the server SHOULD close the channel.
        fail("the client offered no RDPGFX capability set farland supports");
        return;
    }
    negotiated_ = negotiated;
    state_ = GfxState::ready;
    send(CapsConfirm{make_capability_set(negotiated->version, negotiated->flags)});
    events_.emplace_back(event::Ready{*negotiated, reset});
}

void GfxServer::on_frame_acknowledge(const FrameAcknowledge& ack)
{
    queue_depth_ = ack.queue_depth;
    total_frames_decoded_ = ack.total_frames_decoded;
    const auto it = std::ranges::find(unacked_, ack.frame_id);
    const bool known = it != unacked_.end();
    if (ack.queue_depth == suspend_frame_acknowledgement) {
        // [MS-RDPEGFX] 3.2.5.13: clear Unacknowledged Frames, expect no more acks.
        acks_suspended_ = true;
        unacked_.clear();
    } else {
        acks_suspended_ = false;
        if (known) {
            unacked_.erase(unacked_.begin(), std::next(it));
        }
    }
    events_.emplace_back(event::FrameAcked{ack.frame_id, ack.queue_depth, ack.total_frames_decoded, known});
}

void GfxServer::on_qoe_frame_acknowledge(const QoeFrameAcknowledge& qoe)
{
    // Informational only (3.2.5.21); accepted even where the client should not
    // send it, since it changes nothing.
    events_.emplace_back(event::QoeFrameAcked{qoe});
}

void GfxServer::on_cache_import_offer(const CacheImportOffer& offer)
{
    send(CacheImportReply{});
    events_.emplace_back(event::CacheImportOffered{offer.entries.size(), 0});
}

void GfxServer::reset_protocol()
{
    graphics_reset_ = false;
    surfaces_.clear();
    codec_contexts_.clear();
    in_frame_ = false;
    unacked_.clear();
    acks_suspended_ = false;
    queue_depth_ = 0;
    total_frames_decoded_ = 0;
    cache_.clear();
    cache_keys_.clear();
    cache_bytes_ = 0;
    cache_hint_ = 0;
}

bool GfxServer::usable() const
{
    if (state_ == GfxState::failed) {
        return false;
    }
    FARLAND_ASSERT(state_ == GfxState::ready);
    return true;
}

const Negotiated& GfxServer::caps() const
{
    FARLAND_ASSERT(negotiated_.has_value());
    return *negotiated_;
}

const GfxServer::Surface& GfxServer::surface(std::uint16_t surface_id) const
{
    const auto it = surfaces_.find(surface_id);
    FARLAND_ASSERT(it != surfaces_.end());
    return it->second;
}

GfxServer::CacheEntry& GfxServer::cache_entry(std::uint16_t cache_slot)
{
    FARLAND_ASSERT(cache_slot >= 1 && cache_slot <= cache_.size());
    CacheEntry& entry = cache_.at(cache_slot - 1U);
    FARLAND_ASSERT(entry.used);
    return entry;
}

void GfxServer::send(const Pdu& pdu)
{
    output_.push_back(encode(pdu));
}

void GfxServer::fail(std::string reason)
{
    state_ = GfxState::failed;
    failure_ = reason;
    events_.emplace_back(event::Failed{std::move(reason)});
}

void GfxServer::reset_graphics(std::uint32_t width, std::uint32_t height, std::span<const MonitorDef> monitors)
{
    if (!usable()) {
        return;
    }
    FARLAND_ASSERT(!in_frame_);
    FARLAND_ASSERT(width >= 1 && width <= max_output_size && height >= 1 && height <= max_output_size);
    FARLAND_ASSERT(monitors.size() <= max_monitors);
    ResetGraphics pdu{.width = width, .height = height, .monitors = {monitors.begin(), monitors.end()}};
    if (pdu.monitors.empty()) {
        pdu.monitors.push_back(MonitorDef{.left = 0,
                                          .top = 0,
                                          .right = static_cast<std::int32_t>(width) - 1,
                                          .bottom = static_cast<std::int32_t>(height) - 1,
                                          .flags = monitor_primary});
    }
    send(pdu);
    graphics_reset_ = true;
    codec_contexts_.clear();
}

std::uint16_t GfxServer::create_surface(std::uint16_t width, std::uint16_t height, std::uint8_t format)
{
    if (!usable()) {
        return 0;
    }
    FARLAND_ASSERT(graphics_reset_);
    FARLAND_ASSERT(width > 0 && height > 0 && valid_pixel_format(format));
    FARLAND_ASSERT(surfaces_.size() <= std::numeric_limits<std::uint16_t>::max());
    std::uint16_t id = 0;
    for (const auto& [used, unused] : surfaces_) {
        if (used != id) {
            break;
        }
        ++id;
    }
    surfaces_.emplace(id, Surface{width, height, format});
    send(CreateSurface{.surface_id = id, .width = width, .height = height, .pixel_format = format});
    return id;
}

void GfxServer::delete_surface(std::uint16_t surface_id)
{
    if (!usable()) {
        return;
    }
    FARLAND_ASSERT(surfaces_.erase(surface_id) == 1);
    std::erase_if(codec_contexts_, [surface_id](const auto& context) { return context.first == surface_id; });
    send(DeleteSurface{surface_id});
}

void GfxServer::map_surface_to_output(std::uint16_t surface_id, std::uint32_t x, std::uint32_t y)
{
    if (!usable()) {
        return;
    }
    static_cast<void>(surface(surface_id));
    send(MapSurfaceToOutput{.surface_id = surface_id, .output_origin_x = x, .output_origin_y = y});
}

void GfxServer::map_surface_to_scaled_output(std::uint16_t surface_id, std::uint32_t x, std::uint32_t y,
                                             std::uint32_t target_width, std::uint32_t target_height)
{
    if (!usable()) {
        return;
    }
    FARLAND_ASSERT(caps().scaled_output);
    static_cast<void>(surface(surface_id));
    send(MapSurfaceToScaledOutput{.surface_id = surface_id,
                                  .output_origin_x = x,
                                  .output_origin_y = y,
                                  .target_width = target_width,
                                  .target_height = target_height});
}

void GfxServer::map_surface_to_window(std::uint16_t surface_id, std::uint64_t window_id, std::uint32_t mapped_width,
                                      std::uint32_t mapped_height)
{
    if (!usable()) {
        return;
    }
    static_cast<void>(surface(surface_id));
    send(MapSurfaceToWindow{.surface_id = surface_id,
                            .window_id = window_id,
                            .mapped_width = mapped_width,
                            .mapped_height = mapped_height});
}

void GfxServer::map_surface_to_scaled_window(std::uint16_t surface_id, std::uint64_t window_id,
                                             std::uint32_t mapped_width, std::uint32_t mapped_height,
                                             std::uint32_t target_width, std::uint32_t target_height)
{
    if (!usable()) {
        return;
    }
    FARLAND_ASSERT(caps().scaled_output);
    static_cast<void>(surface(surface_id));
    send(MapSurfaceToScaledWindow{.surface_id = surface_id,
                                  .window_id = window_id,
                                  .mapped_width = mapped_width,
                                  .mapped_height = mapped_height,
                                  .target_width = target_width,
                                  .target_height = target_height});
}

std::uint32_t GfxServer::start_frame(std::uint32_t timestamp)
{
    if (!usable()) {
        return 0;
    }
    FARLAND_ASSERT(!in_frame_);  // [MS-RDPEGFX] 3.2.5.11: frames SHOULD NOT nest
    FARLAND_ASSERT(valid_timestamp(timestamp));
    in_frame_ = true;
    current_frame_ = next_frame_id_++;
    send(StartFrame{.timestamp = timestamp, .frame_id = current_frame_});
    return current_frame_;
}

void GfxServer::end_frame()
{
    if (!usable()) {
        return;
    }
    FARLAND_ASSERT(in_frame_);
    in_frame_ = false;
    send(EndFrame{current_frame_});
    if (!acks_suspended_) {
        unacked_.push_back(current_frame_);  // [MS-RDPEGFX] 3.2.5.12
    }
}

void GfxServer::wire_to_surface_1(std::uint16_t surface_id, std::uint16_t codec_id, std::uint8_t format,
                                  Rect16 dest_rect, std::span<const std::byte> bitmap_data)
{
    if (!usable()) {
        return;
    }
    const Surface& target = surface(surface_id);
    FARLAND_ASSERT(in_frame_);
    FARLAND_ASSERT(codec_id != codec::progressive && caps().allows(codec_id));
    FARLAND_ASSERT(valid_pixel_format(format));
    FARLAND_ASSERT(inside(dest_rect, target.width, target.height));
    send(WireToSurface1{.surface_id = surface_id,
                        .codec_id = codec_id,
                        .pixel_format = format,
                        .dest_rect = dest_rect,
                        .bitmap_data = bitmap_data});
}

void GfxServer::wire_to_surface_2(std::uint16_t surface_id, std::uint16_t codec_id, std::uint32_t codec_context_id,
                                  std::uint8_t format, std::span<const std::byte> bitmap_data)
{
    if (!usable()) {
        return;
    }
    static_cast<void>(surface(surface_id));
    FARLAND_ASSERT(in_frame_);
    FARLAND_ASSERT(codec_id == codec::progressive && caps().allows(codec_id));
    FARLAND_ASSERT(valid_pixel_format(format));
    codec_contexts_.emplace(surface_id, codec_context_id);
    send(WireToSurface2{.surface_id = surface_id,
                        .codec_id = codec_id,
                        .codec_context_id = codec_context_id,
                        .pixel_format = format,
                        .bitmap_data = bitmap_data});
}

void GfxServer::delete_encoding_context(std::uint16_t surface_id, std::uint32_t codec_context_id)
{
    if (!usable()) {
        return;
    }
    FARLAND_ASSERT(codec_contexts_.erase({surface_id, codec_context_id}) == 1);
    send(DeleteEncodingContext{.surface_id = surface_id, .codec_context_id = codec_context_id});
}

void GfxServer::solid_fill(std::uint16_t surface_id, Color32 color, std::span<const Rect16> rects)
{
    if (!usable()) {
        return;
    }
    const Surface& target = surface(surface_id);
    FARLAND_ASSERT(in_frame_);
    FARLAND_ASSERT(!rects.empty() && rects.size() <= std::numeric_limits<std::uint16_t>::max());
    FARLAND_ASSERT(std::ranges::all_of(
        rects, [&target](const Rect16& rect) { return inside(rect, target.width, target.height); }));
    send(SolidFill{.surface_id = surface_id, .fill_pixel = color, .fill_rects = {rects.begin(), rects.end()}});
}

void GfxServer::surface_to_surface(std::uint16_t src_surface_id, std::uint16_t dest_surface_id, Rect16 src_rect,
                                   std::span<const Point16> dest_points)
{
    if (!usable()) {
        return;
    }
    const Surface& src = surface(src_surface_id);
    const Surface& dest = surface(dest_surface_id);
    FARLAND_ASSERT(in_frame_);
    FARLAND_ASSERT(inside(src_rect, src.width, src.height));
    FARLAND_ASSERT(!dest_points.empty() && dest_points.size() <= std::numeric_limits<std::uint16_t>::max());
    FARLAND_ASSERT(std::ranges::all_of(dest_points, [&](const Point16& point) {
        return fits(point, src_rect.width(), src_rect.height(), dest.width, dest.height);
    }));
    send(SurfaceToSurface{.surface_id_src = src_surface_id,
                          .surface_id_dest = dest_surface_id,
                          .rect_src = src_rect,
                          .dest_pts = {dest_points.begin(), dest_points.end()}});
}

std::optional<std::uint16_t> GfxServer::surface_to_cache(std::uint16_t surface_id, Rect16 src_rect,
                                                         std::uint64_t cache_key)
{
    if (!usable()) {
        return std::nullopt;
    }
    const Surface& src = surface(surface_id);
    FARLAND_ASSERT(in_frame_);
    FARLAND_ASSERT(inside(src_rect, src.width, src.height));
    FARLAND_ASSERT(!cache_keys_.contains(cache_key));

    const std::uint32_t bytes = std::uint32_t{src_rect.width()} * src_rect.height() * bytes_per_pixel;
    const Negotiated& negotiated = caps();
    if (cache_bytes_ + bytes > negotiated.max_cache_bytes || cache_keys_.size() >= negotiated.max_cache_slots) {
        return std::nullopt;
    }
    if (cache_.empty()) {
        cache_.resize(negotiated.max_cache_slots);
    }
    // Lowest free slot at or after the hint, wrapping around; one exists
    // because fewer than max_cache_slots are in use.
    std::size_t index = cache_hint_;
    while (cache_.at(index).used) {
        index = (index + 1) % cache_.size();
    }
    cache_.at(index) = CacheEntry{cache_key, bytes, src_rect.width(), src_rect.height(), true};
    cache_hint_ = (index + 1) % cache_.size();
    const auto slot = static_cast<std::uint16_t>(index + 1);
    cache_keys_.emplace(cache_key, slot);
    cache_bytes_ += bytes;
    send(SurfaceToCache{.surface_id = surface_id, .cache_key = cache_key, .cache_slot = slot, .rect_src = src_rect});
    return slot;
}

void GfxServer::cache_to_surface(std::uint16_t cache_slot, std::uint16_t surface_id,
                                 std::span<const Point16> dest_points)
{
    if (!usable()) {
        return;
    }
    const CacheEntry& entry = cache_entry(cache_slot);
    const Surface& dest = surface(surface_id);
    FARLAND_ASSERT(in_frame_);
    FARLAND_ASSERT(!dest_points.empty() && dest_points.size() <= std::numeric_limits<std::uint16_t>::max());
    FARLAND_ASSERT(std::ranges::all_of(dest_points, [&](const Point16& point) {
        return fits(point, entry.width, entry.height, dest.width, dest.height);
    }));
    send(CacheToSurface{
        .cache_slot = cache_slot, .surface_id = surface_id, .dest_pts = {dest_points.begin(), dest_points.end()}});
}

void GfxServer::evict_cache_entry(std::uint16_t cache_slot)
{
    if (!usable()) {
        return;
    }
    CacheEntry& entry = cache_entry(cache_slot);
    cache_keys_.erase(entry.key);
    cache_bytes_ -= entry.bytes;
    entry = CacheEntry{};
    cache_hint_ = std::min<std::size_t>(cache_hint_, cache_slot - 1U);
    send(EvictCacheEntry{cache_slot});
}

std::optional<std::uint16_t> GfxServer::cache_slot(std::uint64_t cache_key) const
{
    const auto it = cache_keys_.find(cache_key);
    if (it == cache_keys_.end()) {
        return std::nullopt;
    }
    return it->second;
}

}  // namespace farland::channels::rdpgfx
