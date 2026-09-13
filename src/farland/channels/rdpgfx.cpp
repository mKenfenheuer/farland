// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/channels/rdpgfx.hpp>

#include <bit>
#include <limits>
#include <type_traits>
#include <utility>

namespace farland::channels::rdpgfx {

namespace {

constexpr std::size_t rect16_size = 8;
constexpr std::size_t point16_size = 4;
constexpr std::size_t monitor_def_size = 20;
constexpr std::size_t cache_entry_size = 12;
constexpr std::size_t cache_slot_size = 2;
constexpr std::size_t caps_set_header_size = 8;
constexpr std::size_t reset_graphics_fixed_size = 12;  // width, height, monitorCount
constexpr std::uint32_t caps_data_length_flags = 4;
constexpr std::uint32_t caps_data_length_v10_1 = 16;

template <class T>
Result<Pdu> to_pdu(Result<T> value)
{
    if (!value) {
        return std::unexpected(std::move(value).error());
    }
    return Pdu{std::move(*value)};
}

/// Reads `count` elements of `element_size` bytes each, after checking that
/// they fit in the rest of the PDU (so the reservation is bounded by input).
template <class T, class ReadOne>
Result<std::vector<T>> read_array(Reader& r, std::size_t count, std::size_t element_size, ReadOne read_one)
{
    if (count > r.remaining() / element_size) {
        return fail(Errc::truncated, "array extends past the end of the PDU", r.offset());
    }
    std::vector<T> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        FARLAND_TRY(auto value, read_one(r));
        out.push_back(std::move(value));
    }
    return out;
}

std::uint16_t checked_count(std::size_t count)
{
    FARLAND_ASSERT(count <= std::numeric_limits<std::uint16_t>::max());
    return static_cast<std::uint16_t>(count);
}

std::uint32_t checked_length(std::size_t length)
{
    FARLAND_ASSERT(length <= std::numeric_limits<std::uint32_t>::max());
    return static_cast<std::uint32_t>(length);
}

// Common data types, [MS-RDPEGFX] 2.2.1 ----------------------------------

Result<Rect16> read_rect16(Reader& r)
{
    const std::size_t at = r.offset();
    Rect16 rect;
    FARLAND_TRY(rect.left, r.u16le());
    FARLAND_TRY(rect.top, r.u16le());
    FARLAND_TRY(rect.right, r.u16le());
    FARLAND_TRY(rect.bottom, r.u16le());
    if (rect.empty()) {
        return fail(Errc::invalid_value, "empty or inverted RDPGFX_RECT16", at);
    }
    return rect;
}

void write_rect16(Writer& w, const Rect16& rect)
{
    w.u16le(rect.left);
    w.u16le(rect.top);
    w.u16le(rect.right);
    w.u16le(rect.bottom);
}

Result<Point16> read_point16(Reader& r)
{
    FARLAND_TRY(const std::uint16_t x, r.u16le());
    FARLAND_TRY(const std::uint16_t y, r.u16le());
    return Point16{std::bit_cast<std::int16_t>(x), std::bit_cast<std::int16_t>(y)};
}

void write_point16(Writer& w, const Point16& point)
{
    w.u16le(std::bit_cast<std::uint16_t>(point.x));
    w.u16le(std::bit_cast<std::uint16_t>(point.y));
}

Result<Color32> read_color32(Reader& r)
{
    Color32 color;
    FARLAND_TRY(color.b, r.u8());
    FARLAND_TRY(color.g, r.u8());
    FARLAND_TRY(color.r, r.u8());
    FARLAND_TRY(color.xa, r.u8());
    return color;
}

void write_color32(Writer& w, const Color32& color)
{
    w.u8(color.b);
    w.u8(color.g);
    w.u8(color.r);
    w.u8(color.xa);
}

Result<std::uint8_t> read_pixel_format(Reader& r)
{
    const std::size_t at = r.offset();
    FARLAND_TRY(const std::uint8_t format, r.u8());
    if (!valid_pixel_format(format)) {
        return fail(Errc::invalid_value, "unknown RDPGFX_PIXELFORMAT", at);
    }
    return format;
}

/// Cache slots are one-based and at most 25,600 ([MS-RDPEGFX] 3.3.1.4).
Result<std::uint16_t> read_cache_slot(Reader& r)
{
    const std::size_t at = r.offset();
    FARLAND_TRY(const std::uint16_t slot, r.u16le());
    if (slot == 0 || slot > max_cache_slots) {
        return fail(Errc::invalid_value, "cacheSlot outside 1 to 25600", at);
    }
    return slot;
}

Result<std::span<const std::byte>> read_bitmap_data(Reader& r)
{
    FARLAND_TRY(const std::uint32_t length, r.u32le());
    return r.bytes(length);
}

void write_bitmap_data(Writer& w, std::span<const std::byte> data)
{
    w.u32le(checked_length(data.size()));
    w.bytes(data);
}

Result<MonitorDef> read_monitor_def(Reader& r)
{
    MonitorDef monitor;
    FARLAND_TRY(const std::uint32_t left, r.u32le());
    FARLAND_TRY(const std::uint32_t top, r.u32le());
    FARLAND_TRY(const std::uint32_t right, r.u32le());
    FARLAND_TRY(const std::uint32_t bottom, r.u32le());
    FARLAND_TRY(monitor.flags, r.u32le());
    monitor.left = std::bit_cast<std::int32_t>(left);
    monitor.top = std::bit_cast<std::int32_t>(top);
    monitor.right = std::bit_cast<std::int32_t>(right);
    monitor.bottom = std::bit_cast<std::int32_t>(bottom);
    return monitor;
}

void write_monitor_def(Writer& w, const MonitorDef& monitor)
{
    w.u32le(std::bit_cast<std::uint32_t>(monitor.left));
    w.u32le(std::bit_cast<std::uint32_t>(monitor.top));
    w.u32le(std::bit_cast<std::uint32_t>(monitor.right));
    w.u32le(std::bit_cast<std::uint32_t>(monitor.bottom));
    w.u32le(monitor.flags);
}

/// RDPGFX_CAPSET, [MS-RDPEGFX] 2.2.1.6 and 2.2.3.
Result<CapabilitySet> read_capability_set(Reader& r)
{
    CapabilitySet set;
    FARLAND_TRY(set.version, r.u32le());
    const std::size_t at = r.offset();
    FARLAND_TRY(const std::uint32_t length, r.u32le());
    if (const auto expected = caps_data_length(set.version)) {
        if (length != *expected) {
            return fail(Errc::invalid_length, "capsDataLength does not match the capability set version", at);
        }
        FARLAND_TRY(auto data, r.sub(length));
        if (set.version != cap_version::v10_1) {
            FARLAND_TRY(set.flags, data.u32le());
        }
        // 10.1: sixteen reserved bytes that "MUST be set to zero"; ignored.
        return set;
    }
    if (length > max_unknown_caps_data_length) {
        return fail(Errc::limit_exceeded, "capsDataLength of an unknown capability set version", at);
    }
    FARLAND_TRY(const auto data, r.bytes(length));
    set.unknown_data.assign(data.begin(), data.end());
    if (data.size() >= 4) {
        Reader flags(data);
        FARLAND_TRY(set.flags, flags.u32le());
    }
    return set;
}

void write_capability_set(Writer& w, const CapabilitySet& set)
{
    w.u32le(set.version);
    if (const auto length = caps_data_length(set.version)) {
        w.u32le(*length);
        if (set.version == cap_version::v10_1) {
            w.zeros(caps_data_length_v10_1);
        } else {
            w.u32le(set.flags);
        }
        return;
    }
    w.u32le(checked_length(set.unknown_data.size()));
    w.bytes(set.unknown_data);
}

// Message bodies, [MS-RDPEGFX] 2.2.2 -------------------------------------

Result<WireToSurface1> decode_wire_to_surface_1(Reader& r)
{
    WireToSurface1 pdu;
    FARLAND_TRY(pdu.surface_id, r.u16le());
    FARLAND_TRY(pdu.codec_id, r.u16le());
    FARLAND_TRY(pdu.pixel_format, read_pixel_format(r));
    FARLAND_TRY(pdu.dest_rect, read_rect16(r));
    FARLAND_TRY(pdu.bitmap_data, read_bitmap_data(r));
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_WIRE_TO_SURFACE_PDU_1 longer than its bitmapData"));
    return pdu;
}

void encode_body(Writer& w, const WireToSurface1& pdu)
{
    w.u16le(pdu.surface_id);
    w.u16le(pdu.codec_id);
    w.u8(pdu.pixel_format);
    write_rect16(w, pdu.dest_rect);
    write_bitmap_data(w, pdu.bitmap_data);
}

Result<WireToSurface2> decode_wire_to_surface_2(Reader& r)
{
    WireToSurface2 pdu;
    FARLAND_TRY(pdu.surface_id, r.u16le());
    FARLAND_TRY(pdu.codec_id, r.u16le());
    FARLAND_TRY(pdu.codec_context_id, r.u32le());
    FARLAND_TRY(pdu.pixel_format, read_pixel_format(r));
    FARLAND_TRY(pdu.bitmap_data, read_bitmap_data(r));
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_WIRE_TO_SURFACE_PDU_2 longer than its bitmapData"));
    return pdu;
}

void encode_body(Writer& w, const WireToSurface2& pdu)
{
    w.u16le(pdu.surface_id);
    w.u16le(pdu.codec_id);
    w.u32le(pdu.codec_context_id);
    w.u8(pdu.pixel_format);
    write_bitmap_data(w, pdu.bitmap_data);
}

Result<DeleteEncodingContext> decode_delete_encoding_context(Reader& r)
{
    DeleteEncodingContext pdu;
    FARLAND_TRY(pdu.surface_id, r.u16le());
    FARLAND_TRY(pdu.codec_context_id, r.u32le());
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_DELETE_ENCODING_CONTEXT_PDU"));
    return pdu;
}

void encode_body(Writer& w, const DeleteEncodingContext& pdu)
{
    w.u16le(pdu.surface_id);
    w.u32le(pdu.codec_context_id);
}

Result<SolidFill> decode_solid_fill(Reader& r)
{
    SolidFill pdu;
    FARLAND_TRY(pdu.surface_id, r.u16le());
    FARLAND_TRY(pdu.fill_pixel, read_color32(r));
    FARLAND_TRY(const std::uint16_t count, r.u16le());
    FARLAND_TRY(pdu.fill_rects, read_array<Rect16>(r, count, rect16_size, read_rect16));
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_SOLIDFILL_PDU"));
    return pdu;
}

void encode_body(Writer& w, const SolidFill& pdu)
{
    w.u16le(pdu.surface_id);
    write_color32(w, pdu.fill_pixel);
    w.u16le(checked_count(pdu.fill_rects.size()));
    for (const Rect16& rect : pdu.fill_rects) {
        write_rect16(w, rect);
    }
}

Result<SurfaceToSurface> decode_surface_to_surface(Reader& r)
{
    SurfaceToSurface pdu;
    FARLAND_TRY(pdu.surface_id_src, r.u16le());
    FARLAND_TRY(pdu.surface_id_dest, r.u16le());
    FARLAND_TRY(pdu.rect_src, read_rect16(r));
    FARLAND_TRY(const std::uint16_t count, r.u16le());
    FARLAND_TRY(pdu.dest_pts, read_array<Point16>(r, count, point16_size, read_point16));
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_SURFACE_TO_SURFACE_PDU"));
    return pdu;
}

void encode_body(Writer& w, const SurfaceToSurface& pdu)
{
    w.u16le(pdu.surface_id_src);
    w.u16le(pdu.surface_id_dest);
    write_rect16(w, pdu.rect_src);
    w.u16le(checked_count(pdu.dest_pts.size()));
    for (const Point16& point : pdu.dest_pts) {
        write_point16(w, point);
    }
}

Result<SurfaceToCache> decode_surface_to_cache(Reader& r)
{
    SurfaceToCache pdu;
    FARLAND_TRY(pdu.surface_id, r.u16le());
    FARLAND_TRY(pdu.cache_key, r.u64le());
    FARLAND_TRY(pdu.cache_slot, read_cache_slot(r));
    FARLAND_TRY(pdu.rect_src, read_rect16(r));
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_SURFACE_TO_CACHE_PDU"));
    return pdu;
}

void encode_body(Writer& w, const SurfaceToCache& pdu)
{
    w.u16le(pdu.surface_id);
    w.u64le(pdu.cache_key);
    w.u16le(pdu.cache_slot);
    write_rect16(w, pdu.rect_src);
}

Result<CacheToSurface> decode_cache_to_surface(Reader& r)
{
    CacheToSurface pdu;
    FARLAND_TRY(pdu.cache_slot, read_cache_slot(r));
    FARLAND_TRY(pdu.surface_id, r.u16le());
    FARLAND_TRY(const std::uint16_t count, r.u16le());
    FARLAND_TRY(pdu.dest_pts, read_array<Point16>(r, count, point16_size, read_point16));
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_CACHE_TO_SURFACE_PDU"));
    return pdu;
}

void encode_body(Writer& w, const CacheToSurface& pdu)
{
    w.u16le(pdu.cache_slot);
    w.u16le(pdu.surface_id);
    w.u16le(checked_count(pdu.dest_pts.size()));
    for (const Point16& point : pdu.dest_pts) {
        write_point16(w, point);
    }
}

Result<EvictCacheEntry> decode_evict_cache_entry(Reader& r)
{
    EvictCacheEntry pdu;
    FARLAND_TRY(pdu.cache_slot, read_cache_slot(r));
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_EVICT_CACHE_ENTRY_PDU"));
    return pdu;
}

void encode_body(Writer& w, const EvictCacheEntry& pdu)
{
    w.u16le(pdu.cache_slot);
}

Result<CreateSurface> decode_create_surface(Reader& r)
{
    CreateSurface pdu;
    FARLAND_TRY(pdu.surface_id, r.u16le());
    const std::size_t at = r.offset();
    FARLAND_TRY(pdu.width, r.u16le());
    FARLAND_TRY(pdu.height, r.u16le());
    if (pdu.width == 0 || pdu.height == 0) {
        return fail(Errc::invalid_value, "RDPGFX_CREATE_SURFACE_PDU with an empty surface", at);
    }
    FARLAND_TRY(pdu.pixel_format, read_pixel_format(r));
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_CREATE_SURFACE_PDU"));
    return pdu;
}

void encode_body(Writer& w, const CreateSurface& pdu)
{
    w.u16le(pdu.surface_id);
    w.u16le(pdu.width);
    w.u16le(pdu.height);
    w.u8(pdu.pixel_format);
}

Result<DeleteSurface> decode_delete_surface(Reader& r)
{
    DeleteSurface pdu;
    FARLAND_TRY(pdu.surface_id, r.u16le());
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_DELETE_SURFACE_PDU"));
    return pdu;
}

void encode_body(Writer& w, const DeleteSurface& pdu)
{
    w.u16le(pdu.surface_id);
}

Result<StartFrame> decode_start_frame(Reader& r)
{
    StartFrame pdu;
    FARLAND_TRY(pdu.timestamp, r.u32le());
    FARLAND_TRY(pdu.frame_id, r.u32le());
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_START_FRAME_PDU"));
    return pdu;
}

void encode_body(Writer& w, const StartFrame& pdu)
{
    w.u32le(pdu.timestamp);
    w.u32le(pdu.frame_id);
}

Result<EndFrame> decode_end_frame(Reader& r)
{
    EndFrame pdu;
    FARLAND_TRY(pdu.frame_id, r.u32le());
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_END_FRAME_PDU"));
    return pdu;
}

void encode_body(Writer& w, const EndFrame& pdu)
{
    w.u32le(pdu.frame_id);
}

Result<FrameAcknowledge> decode_frame_acknowledge(Reader& r)
{
    FrameAcknowledge pdu;
    FARLAND_TRY(pdu.queue_depth, r.u32le());
    FARLAND_TRY(pdu.frame_id, r.u32le());
    FARLAND_TRY(pdu.total_frames_decoded, r.u32le());
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_FRAME_ACKNOWLEDGE_PDU"));
    return pdu;
}

void encode_body(Writer& w, const FrameAcknowledge& pdu)
{
    w.u32le(pdu.queue_depth);
    w.u32le(pdu.frame_id);
    w.u32le(pdu.total_frames_decoded);
}

Result<ResetGraphics> decode_reset_graphics(Reader& r)
{
    if (r.size() != reset_graphics_pdu_size) {
        return fail(Errc::invalid_length, "RDPGFX_RESET_GRAPHICS_PDU is not 340 bytes", 0);
    }
    ResetGraphics pdu;
    const std::size_t at = r.offset();
    FARLAND_TRY(pdu.width, r.u32le());
    FARLAND_TRY(pdu.height, r.u32le());
    if (pdu.width == 0 || pdu.height == 0 || pdu.width > max_output_size || pdu.height > max_output_size) {
        return fail(Errc::invalid_value, "RDPGFX_RESET_GRAPHICS_PDU size outside 1 to 32766", at);
    }
    const std::size_t count_at = r.offset();
    FARLAND_TRY(const std::uint32_t count, r.u32le());
    if (count > max_monitors) {
        return fail(Errc::invalid_value, "RDPGFX_RESET_GRAPHICS_PDU with more than 16 monitors", count_at);
    }
    FARLAND_TRY(pdu.monitors, read_array<MonitorDef>(r, count, monitor_def_size, read_monitor_def));
    FARLAND_TRY_VOID(r.skip(r.remaining()));  // pad, "MUST be ignored"
    return pdu;
}

void encode_body(Writer& w, const ResetGraphics& pdu)
{
    FARLAND_ASSERT(pdu.monitors.size() <= max_monitors);
    w.u32le(pdu.width);
    w.u32le(pdu.height);
    w.u32le(static_cast<std::uint32_t>(pdu.monitors.size()));
    for (const MonitorDef& monitor : pdu.monitors) {
        write_monitor_def(w, monitor);
    }
    w.zeros(reset_graphics_pdu_size - header_size - reset_graphics_fixed_size -
            (pdu.monitors.size() * monitor_def_size));
}

Result<MapSurfaceToOutput> decode_map_surface_to_output(Reader& r)
{
    MapSurfaceToOutput pdu;
    FARLAND_TRY(pdu.surface_id, r.u16le());
    FARLAND_TRY_VOID(r.skip(2));  // reserved
    FARLAND_TRY(pdu.output_origin_x, r.u32le());
    FARLAND_TRY(pdu.output_origin_y, r.u32le());
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU"));
    return pdu;
}

void encode_body(Writer& w, const MapSurfaceToOutput& pdu)
{
    w.u16le(pdu.surface_id);
    w.u16le(0);
    w.u32le(pdu.output_origin_x);
    w.u32le(pdu.output_origin_y);
}

Result<CacheEntryMetadata> read_cache_entry(Reader& r)
{
    CacheEntryMetadata entry;
    FARLAND_TRY(entry.cache_key, r.u64le());
    FARLAND_TRY(entry.bitmap_length, r.u32le());
    return entry;
}

Result<CacheImportOffer> decode_cache_import_offer(Reader& r)
{
    CacheImportOffer pdu;
    const std::size_t at = r.offset();
    FARLAND_TRY(const std::uint16_t count, r.u16le());
    if (count > max_cache_import_entries) {
        return fail(Errc::invalid_value, "cacheEntriesCount not below 5462", at);
    }
    FARLAND_TRY(pdu.entries, read_array<CacheEntryMetadata>(r, count, cache_entry_size, read_cache_entry));
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_CACHE_IMPORT_OFFER_PDU"));
    return pdu;
}

void encode_body(Writer& w, const CacheImportOffer& pdu)
{
    FARLAND_ASSERT(pdu.entries.size() <= max_cache_import_entries);
    w.u16le(checked_count(pdu.entries.size()));
    for (const CacheEntryMetadata& entry : pdu.entries) {
        w.u64le(entry.cache_key);
        w.u32le(entry.bitmap_length);
    }
}

Result<CacheImportReply> decode_cache_import_reply(Reader& r)
{
    CacheImportReply pdu;
    const std::size_t at = r.offset();
    FARLAND_TRY(const std::uint16_t count, r.u16le());
    if (count > max_cache_import_entries) {
        return fail(Errc::invalid_value, "importedEntriesCount not below 5462", at);
    }
    FARLAND_TRY(pdu.cache_slots, read_array<std::uint16_t>(r, count, cache_slot_size, read_cache_slot));
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_CACHE_IMPORT_REPLY_PDU"));
    return pdu;
}

void encode_body(Writer& w, const CacheImportReply& pdu)
{
    FARLAND_ASSERT(pdu.cache_slots.size() <= max_cache_import_entries);
    w.u16le(checked_count(pdu.cache_slots.size()));
    for (const std::uint16_t slot : pdu.cache_slots) {
        w.u16le(slot);
    }
}

Result<CapsAdvertise> decode_caps_advertise(Reader& r)
{
    CapsAdvertise pdu;
    const std::size_t at = r.offset();
    FARLAND_TRY(const std::uint16_t count, r.u16le());
    if (count == 0) {
        return fail(Errc::invalid_value, "RDPGFX_CAPS_ADVERTISE_PDU without capability sets", at);
    }
    if (count > max_caps_sets) {
        return fail(Errc::limit_exceeded, "capsSetCount", at);
    }
    FARLAND_TRY(pdu.caps_sets, read_array<CapabilitySet>(r, count, caps_set_header_size, read_capability_set));
    for (auto it = pdu.caps_sets.begin(); it != pdu.caps_sets.end(); ++it) {
        const std::uint32_t version = it->version;
        if (std::ranges::any_of(pdu.caps_sets.begin(), it, [version](const auto& s) { return s.version == version; })) {
            // [MS-RDPEGFX] 3.3.5.18: each capability set type MUST NOT appear more than once.
            return fail(Errc::invalid_value, "capability set version advertised twice", at);
        }
    }
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_CAPS_ADVERTISE_PDU"));
    return pdu;
}

void encode_body(Writer& w, const CapsAdvertise& pdu)
{
    w.u16le(checked_count(pdu.caps_sets.size()));
    for (const CapabilitySet& set : pdu.caps_sets) {
        write_capability_set(w, set);
    }
}

Result<CapsConfirm> decode_caps_confirm(Reader& r)
{
    CapsConfirm pdu;
    FARLAND_TRY(pdu.caps_set, read_capability_set(r));
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_CAPS_CONFIRM_PDU"));
    return pdu;
}

void encode_body(Writer& w, const CapsConfirm& pdu)
{
    write_capability_set(w, pdu.caps_set);
}

Result<MapSurfaceToWindow> decode_map_surface_to_window(Reader& r)
{
    MapSurfaceToWindow pdu;
    FARLAND_TRY(pdu.surface_id, r.u16le());
    FARLAND_TRY(pdu.window_id, r.u64le());
    FARLAND_TRY(pdu.mapped_width, r.u32le());
    FARLAND_TRY(pdu.mapped_height, r.u32le());
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_MAP_SURFACE_TO_WINDOW_PDU"));
    return pdu;
}

void encode_body(Writer& w, const MapSurfaceToWindow& pdu)
{
    w.u16le(pdu.surface_id);
    w.u64le(pdu.window_id);
    w.u32le(pdu.mapped_width);
    w.u32le(pdu.mapped_height);
}

Result<QoeFrameAcknowledge> decode_qoe_frame_acknowledge(Reader& r)
{
    QoeFrameAcknowledge pdu;
    FARLAND_TRY(pdu.frame_id, r.u32le());
    FARLAND_TRY(pdu.timestamp, r.u32le());
    FARLAND_TRY(pdu.time_diff_se, r.u16le());
    FARLAND_TRY(pdu.time_diff_edr, r.u16le());
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_QOE_FRAME_ACKNOWLEDGE_PDU"));
    return pdu;
}

void encode_body(Writer& w, const QoeFrameAcknowledge& pdu)
{
    w.u32le(pdu.frame_id);
    w.u32le(pdu.timestamp);
    w.u16le(pdu.time_diff_se);
    w.u16le(pdu.time_diff_edr);
}

Result<MapSurfaceToScaledOutput> decode_map_surface_to_scaled_output(Reader& r)
{
    MapSurfaceToScaledOutput pdu;
    FARLAND_TRY(pdu.surface_id, r.u16le());
    FARLAND_TRY_VOID(r.skip(2));  // reserved
    FARLAND_TRY(pdu.output_origin_x, r.u32le());
    FARLAND_TRY(pdu.output_origin_y, r.u32le());
    FARLAND_TRY(pdu.target_width, r.u32le());
    FARLAND_TRY(pdu.target_height, r.u32le());
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_MAP_SURFACE_TO_SCALED_OUTPUT_PDU"));
    return pdu;
}

void encode_body(Writer& w, const MapSurfaceToScaledOutput& pdu)
{
    w.u16le(pdu.surface_id);
    w.u16le(0);
    w.u32le(pdu.output_origin_x);
    w.u32le(pdu.output_origin_y);
    w.u32le(pdu.target_width);
    w.u32le(pdu.target_height);
}

Result<MapSurfaceToScaledWindow> decode_map_surface_to_scaled_window(Reader& r)
{
    MapSurfaceToScaledWindow pdu;
    FARLAND_TRY(pdu.surface_id, r.u16le());
    FARLAND_TRY(pdu.window_id, r.u64le());
    FARLAND_TRY(pdu.mapped_width, r.u32le());
    FARLAND_TRY(pdu.mapped_height, r.u32le());
    FARLAND_TRY(pdu.target_width, r.u32le());
    FARLAND_TRY(pdu.target_height, r.u32le());
    FARLAND_TRY_VOID(r.expect_end("RDPGFX_MAP_SURFACE_TO_SCALED_WINDOW_PDU"));
    return pdu;
}

void encode_body(Writer& w, const MapSurfaceToScaledWindow& pdu)
{
    w.u16le(pdu.surface_id);
    w.u64le(pdu.window_id);
    w.u32le(pdu.mapped_width);
    w.u32le(pdu.mapped_height);
    w.u32le(pdu.target_width);
    w.u32le(pdu.target_height);
}

}  // namespace

std::optional<std::uint32_t> caps_data_length(std::uint32_t version) noexcept
{
    switch (version) {
    case cap_version::v10_1:
        return caps_data_length_v10_1;
    case cap_version::v8:
    case cap_version::v8_1:
    case cap_version::v10:
    case cap_version::v10_2:
    case cap_version::v10_3:
    case cap_version::v10_4:
    case cap_version::v10_5:
    case cap_version::v10_6:
    case cap_version::v10_6_err:
    case cap_version::v10_7:
        return caps_data_length_flags;
    default:
        return std::nullopt;
    }
}

std::uint32_t defined_caps_flags(std::uint32_t version) noexcept
{
    using namespace caps_flag;
    switch (version) {
    case cap_version::v8:
        return thin_client | small_cache;
    case cap_version::v8_1:
        return thin_client | small_cache | avc420_enabled;
    case cap_version::v10:
    case cap_version::v10_2:
        return small_cache | avc_disabled;
    case cap_version::v10_3:
        return avc_disabled | avc_thin_client;
    case cap_version::v10_4:
    case cap_version::v10_5:
    case cap_version::v10_6:
    case cap_version::v10_6_err:
        return small_cache | avc_disabled | avc_thin_client;
    case cap_version::v10_7:
        return small_cache | avc_disabled | avc_thin_client | scaledmap_disable;
    default:
        return 0;  // 10.1 and undefined versions
    }
}

std::string_view version_name(std::uint32_t version) noexcept
{
    switch (version) {
    case cap_version::v8:
        return "8.0";
    case cap_version::v8_1:
        return "8.1";
    case cap_version::v10:
        return "10.0";
    case cap_version::v10_1:
        return "10.1";
    case cap_version::v10_2:
        return "10.2";
    case cap_version::v10_3:
        return "10.3";
    case cap_version::v10_4:
        return "10.4";
    case cap_version::v10_5:
        return "10.5";
    case cap_version::v10_6:
        return "10.6";
    case cap_version::v10_6_err:
        return "10.6 (errata value)";
    case cap_version::v10_7:
        return "10.7";
    case cap_version::v11_1:
        return "11.1";
    case cap_version::v11_2:
        return "11.2";
    case cap_version::v11_3:
        return "11.3";
    default:
        return "unknown";
    }
}

CapabilitySet make_capability_set(std::uint32_t version, std::uint32_t flags)
{
    CapabilitySet set{.version = version, .flags = flags, .unknown_data = {}};
    if (version == cap_version::v10_1) {
        set.flags = 0;
    } else if (!caps_data_length(version)) {
        Writer w;
        w.u32le(flags);
        set.unknown_data = std::move(w).take();
    }
    return set;
}

std::uint32_t make_timestamp(std::uint32_t hours, std::uint32_t minutes, std::uint32_t seconds,
                             std::uint32_t milliseconds)
{
    FARLAND_ASSERT(hours <= 23 && minutes <= 59 && seconds <= 59 && milliseconds <= 999);
    return (hours << 22U) | (minutes << 16U) | (seconds << 10U) | milliseconds;
}

std::uint16_t cmd_id(const Pdu& pdu)
{
    return std::visit([](const auto& p) { return std::remove_cvref_t<decltype(p)>::command; }, pdu);
}

Result<std::optional<std::size_t>> frame_pdu(std::span<const std::byte> stream, std::size_t max_size)
{
    if (stream.size() < header_size) {
        return std::optional<std::size_t>{};
    }
    Reader r(stream);
    FARLAND_TRY_VOID(r.skip(4));  // cmdId, flags
    const std::size_t at = r.offset();
    FARLAND_TRY(const std::uint32_t length, r.u32le());
    if (length < header_size) {
        return fail(Errc::invalid_length, "pduLength below the RDPGFX_HEADER size", at);
    }
    if (length > max_size) {
        return fail(Errc::limit_exceeded, "RDPGFX PDU above the size limit", at);
    }
    if (length > stream.size()) {
        return std::optional<std::size_t>{};
    }
    return std::optional<std::size_t>{length};
}

Result<Pdu> decode_pdu(std::span<const std::byte> bytes)
{
    Reader r(bytes);
    FARLAND_TRY(const std::uint16_t command, r.u16le());
    FARLAND_TRY_VOID(r.skip(2));  // flags
    const std::size_t at = r.offset();
    FARLAND_TRY(const std::uint32_t length, r.u32le());
    if (length != bytes.size()) {
        return fail(Errc::invalid_length, "pduLength does not match the PDU size", at);
    }
    switch (command) {
    case cmd::wire_to_surface_1:
        return to_pdu(decode_wire_to_surface_1(r));
    case cmd::wire_to_surface_2:
        return to_pdu(decode_wire_to_surface_2(r));
    case cmd::delete_encoding_context:
        return to_pdu(decode_delete_encoding_context(r));
    case cmd::solid_fill:
        return to_pdu(decode_solid_fill(r));
    case cmd::surface_to_surface:
        return to_pdu(decode_surface_to_surface(r));
    case cmd::surface_to_cache:
        return to_pdu(decode_surface_to_cache(r));
    case cmd::cache_to_surface:
        return to_pdu(decode_cache_to_surface(r));
    case cmd::evict_cache_entry:
        return to_pdu(decode_evict_cache_entry(r));
    case cmd::create_surface:
        return to_pdu(decode_create_surface(r));
    case cmd::delete_surface:
        return to_pdu(decode_delete_surface(r));
    case cmd::start_frame:
        return to_pdu(decode_start_frame(r));
    case cmd::end_frame:
        return to_pdu(decode_end_frame(r));
    case cmd::frame_acknowledge:
        return to_pdu(decode_frame_acknowledge(r));
    case cmd::reset_graphics:
        return to_pdu(decode_reset_graphics(r));
    case cmd::map_surface_to_output:
        return to_pdu(decode_map_surface_to_output(r));
    case cmd::cache_import_offer:
        return to_pdu(decode_cache_import_offer(r));
    case cmd::cache_import_reply:
        return to_pdu(decode_cache_import_reply(r));
    case cmd::caps_advertise:
        return to_pdu(decode_caps_advertise(r));
    case cmd::caps_confirm:
        return to_pdu(decode_caps_confirm(r));
    case cmd::map_surface_to_window:
        return to_pdu(decode_map_surface_to_window(r));
    case cmd::qoe_frame_acknowledge:
        return to_pdu(decode_qoe_frame_acknowledge(r));
    case cmd::map_surface_to_scaled_output:
        return to_pdu(decode_map_surface_to_scaled_output(r));
    case cmd::map_surface_to_scaled_window:
        return to_pdu(decode_map_surface_to_scaled_window(r));
    default:
        return fail(Errc::unsupported, "unknown RDPGFX cmdId", 0);
    }
}

void encode(Writer& w, const Pdu& pdu)
{
    const std::size_t start = w.size();
    w.u16le(cmd_id(pdu));
    w.u16le(0);  // flags
    w.u32le(0);  // pduLength, patched below
    std::visit([&w](const auto& p) { encode_body(w, p); }, pdu);
    w.patch_u32le(start + 4, checked_length(w.size() - start));
}

std::vector<std::byte> encode(const Pdu& pdu)
{
    Writer w;
    encode(w, pdu);
    return std::move(w).take();
}

}  // namespace farland::channels::rdpgfx
