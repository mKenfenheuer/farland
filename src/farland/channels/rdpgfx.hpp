// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

/// PDU codecs for the Graphics Pipeline Extension ([MS-RDPEGFX] 2.2) on the
/// dynamic virtual channel "Microsoft::Windows::RDS::Graphics", for both
/// directions. These are the plain graphics messages: server-to-client PDUs
/// still have to be wrapped in RDP_SEGMENTED_DATA ([MS-RDPEGFX] 2.2.5.1,
/// ZGFX) before they go on the channel; client-to-server PDUs are sent bare
/// ([MS-RDPEGFX] 2.1).
namespace farland::channels::rdpgfx {

/// [MS-RDPEGFX] 2.1. Sent without the terminating NUL here.
inline constexpr std::string_view channel_name = "Microsoft::Windows::RDS::Graphics";

/// RDPGFX_HEADER cmdId values, [MS-RDPEGFX] 2.2.1.5.
namespace cmd {
inline constexpr std::uint16_t wire_to_surface_1 = 0x0001;
inline constexpr std::uint16_t wire_to_surface_2 = 0x0002;
inline constexpr std::uint16_t delete_encoding_context = 0x0003;
inline constexpr std::uint16_t solid_fill = 0x0004;
inline constexpr std::uint16_t surface_to_surface = 0x0005;
inline constexpr std::uint16_t surface_to_cache = 0x0006;
inline constexpr std::uint16_t cache_to_surface = 0x0007;
inline constexpr std::uint16_t evict_cache_entry = 0x0008;
inline constexpr std::uint16_t create_surface = 0x0009;
inline constexpr std::uint16_t delete_surface = 0x000A;
inline constexpr std::uint16_t start_frame = 0x000B;
inline constexpr std::uint16_t end_frame = 0x000C;
inline constexpr std::uint16_t frame_acknowledge = 0x000D;
inline constexpr std::uint16_t reset_graphics = 0x000E;
inline constexpr std::uint16_t map_surface_to_output = 0x000F;
inline constexpr std::uint16_t cache_import_offer = 0x0010;
inline constexpr std::uint16_t cache_import_reply = 0x0011;
inline constexpr std::uint16_t caps_advertise = 0x0012;
inline constexpr std::uint16_t caps_confirm = 0x0013;
inline constexpr std::uint16_t map_surface_to_window = 0x0015;
inline constexpr std::uint16_t qoe_frame_acknowledge = 0x0016;
inline constexpr std::uint16_t map_surface_to_scaled_output = 0x0017;
inline constexpr std::uint16_t map_surface_to_scaled_window = 0x0018;
}  // namespace cmd

/// codecId values, [MS-RDPEGFX] 2.2.2.1 and 2.2.2.2.
namespace codec {
inline constexpr std::uint16_t uncompressed = 0x0000;
inline constexpr std::uint16_t cavideo = 0x0003;  ///< RemoteFX, [MS-RDPRFX]
inline constexpr std::uint16_t clearcodec = 0x0008;
inline constexpr std::uint16_t progressive = 0x0009;  ///< WireToSurface2 only
inline constexpr std::uint16_t planar = 0x000A;
inline constexpr std::uint16_t avc420 = 0x000B;
inline constexpr std::uint16_t alpha = 0x000C;
inline constexpr std::uint16_t avc444 = 0x000E;
inline constexpr std::uint16_t avc444v2 = 0x000F;
}  // namespace codec

/// RDPGFX_PIXELFORMAT, [MS-RDPEGFX] 2.2.1.4.
namespace pixel_format {
inline constexpr std::uint8_t xrgb_8888 = 0x20;
inline constexpr std::uint8_t argb_8888 = 0x21;
}  // namespace pixel_format

[[nodiscard]] constexpr bool valid_pixel_format(std::uint8_t format) noexcept
{
    return format == pixel_format::xrgb_8888 || format == pixel_format::argb_8888;
}

/// RDPGFX_CAPSET version values, [MS-RDPEGFX] 2.2.1.6 and 2.2.3.
namespace cap_version {
inline constexpr std::uint32_t v8 = 0x00080004;     ///< 2.2.3.1
inline constexpr std::uint32_t v8_1 = 0x00080105;   ///< 2.2.3.2
inline constexpr std::uint32_t v10 = 0x000A0002;    ///< 2.2.3.3
inline constexpr std::uint32_t v10_1 = 0x000A0100;  ///< 2.2.3.4, no flags, 16 reserved bytes
inline constexpr std::uint32_t v10_2 = 0x000A0200;  ///< 2.2.3.5
inline constexpr std::uint32_t v10_3 = 0x000A0301;  ///< 2.2.3.6
inline constexpr std::uint32_t v10_4 = 0x000A0400;  ///< 2.2.3.7
inline constexpr std::uint32_t v10_5 = 0x000A0502;  ///< 2.2.3.8
inline constexpr std::uint32_t v10_6 = 0x000A0600;  ///< 2.2.3.9
/// The value older revisions of the specification gave for 10.6 (fixed by
/// the [MS-RDPEGFX]-180912 errata). FreeRDP clients still offer it next to
/// 10.6; farland treats it as 10.6.
inline constexpr std::uint32_t v10_6_err = 0x000A0601;
inline constexpr std::uint32_t v10_7 = 0x000A0701;  ///< 2.2.3.10
/// Undocumented versions (Azure Virtual Desktop; FreeRDP clients built with
/// WITH_GFX_AZURE offer them). [MS-RDPEGFX] 6 <5>: Windows used to reflect
/// them in the Caps Confirm by mistake while behaving as for 10.7, and fixed
/// that. farland decodes them as raw capability sets and never confirms them.
inline constexpr std::uint32_t v11_1 = 0x000B0101;
inline constexpr std::uint32_t v11_2 = 0x000B0200;
inline constexpr std::uint32_t v11_3 = 0x000B0300;
}  // namespace cap_version

/// Capability flags, [MS-RDPEGFX] 2.2.3. Which ones a version defines:
/// `defined_caps_flags`.
namespace caps_flag {
inline constexpr std::uint32_t thin_client = 0x00000001;        ///< 8.0, 8.1
inline constexpr std::uint32_t small_cache = 0x00000002;        ///< 8.0 to 10.2, 10.4 to 10.7
inline constexpr std::uint32_t avc420_enabled = 0x00000010;     ///< 8.1
inline constexpr std::uint32_t avc_disabled = 0x00000020;       ///< 10.0, 10.2 and later
inline constexpr std::uint32_t avc_thin_client = 0x00000040;    ///< 10.3 and later
inline constexpr std::uint32_t scaledmap_disable = 0x00000080;  ///< 10.7
}  // namespace caps_flag

/// The capsDataLength a version defined by the specification must have
/// ([MS-RDPEGFX] 2.2.3): 16 for 10.1, 4 for every other version (10.6's
/// errata value included). nullopt for versions the specification does not
/// define. Interop trap: a Caps Confirm for 10.1 with length 4 is wrong.
[[nodiscard]] std::optional<std::uint32_t> caps_data_length(std::uint32_t version) noexcept;
/// The flags [MS-RDPEGFX] 2.2.3 defines for `version`; 0 for 10.1 and for
/// versions it does not define.
[[nodiscard]] std::uint32_t defined_caps_flags(std::uint32_t version) noexcept;
/// "8.0", "10.6", ... for logs; "unknown" for undefined versions.
[[nodiscard]] std::string_view version_name(std::uint32_t version) noexcept;

inline constexpr std::size_t header_size = 8;
/// [MS-RDPEGFX] 2.2.2.14: ResetGraphics is always exactly this long.
inline constexpr std::size_t reset_graphics_pdu_size = 340;
inline constexpr std::size_t max_monitors = 16;
/// Largest Graphics Output Buffer width and height, [MS-RDPEGFX] 2.2.2.14.
inline constexpr std::uint32_t max_output_size = 32766;
/// cacheEntriesCount "MUST be less than 5462", [MS-RDPEGFX] 2.2.2.16.
inline constexpr std::size_t max_cache_import_entries = 5461;
/// Bitmap cache slots are one-based; at most 25,600 slots (100 MB cache) or
/// 4,096 (16 MB cache), [MS-RDPEGFX] 3.3.1.4.
inline constexpr std::uint16_t max_cache_slots = 25600;
inline constexpr std::uint16_t max_cache_slots_small = 4096;
inline constexpr std::uint32_t cache_size = 100U * 1024U * 1024U;
inline constexpr std::uint32_t cache_size_small = 16U * 1024U * 1024U;
/// FrameAcknowledge queueDepth values, [MS-RDPEGFX] 2.2.2.13.
inline constexpr std::uint32_t queue_depth_unavailable = 0x00000000;
inline constexpr std::uint32_t suspend_frame_acknowledgement = 0xFFFFFFFF;

/// farland limits. Clients offer about a dozen capability sets; FreeRDP's
/// client caps its list at 256.
inline constexpr std::size_t max_caps_sets = 64;
/// Longest capsData accepted for a version the specification does not define.
inline constexpr std::size_t max_unknown_caps_data_length = 256;
/// Largest client-to-server PDU: a CacheImportOffer with 5461 entries.
inline constexpr std::size_t max_client_pdu_size = header_size + 2 + (max_cache_import_entries * 12);

// Common data types, [MS-RDPEGFX] 2.2.1 ----------------------------------

/// RDPGFX_POINT16, [MS-RDPEGFX] 2.2.1.1 (signed coordinates).
struct Point16 {
    std::int16_t x = 0;
    std::int16_t y = 0;
    friend bool operator==(const Point16&, const Point16&) = default;
};

/// RDPGFX_RECT16, [MS-RDPEGFX] 2.2.1.2: exclusive right and bottom bounds.
/// The decoder rejects empty and inverted rectangles, as FreeRDP's client does.
struct Rect16 {
    std::uint16_t left = 0;
    std::uint16_t top = 0;
    std::uint16_t right = 0;
    std::uint16_t bottom = 0;

    [[nodiscard]] constexpr bool empty() const noexcept { return left >= right || top >= bottom; }
    [[nodiscard]] constexpr std::uint16_t width() const noexcept
    {
        return empty() ? std::uint16_t{0} : static_cast<std::uint16_t>(right - left);
    }
    [[nodiscard]] constexpr std::uint16_t height() const noexcept
    {
        return empty() ? std::uint16_t{0} : static_cast<std::uint16_t>(bottom - top);
    }
    friend bool operator==(const Rect16&, const Rect16&) = default;
};

/// RDPGFX_COLOR32, [MS-RDPEGFX] 2.2.1.3 (B, G, R, XA on the wire).
struct Color32 {
    std::uint8_t b = 0;
    std::uint8_t g = 0;
    std::uint8_t r = 0;
    std::uint8_t xa = 0;
    friend bool operator==(const Color32&, const Color32&) = default;
};

/// TS_MONITOR_DEF, [MS-RDPBCGR] 2.2.1.3.6.1: inclusive bounds in virtual
/// desktop coordinates.
struct MonitorDef {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    std::uint32_t flags = 0;
    friend bool operator==(const MonitorDef&, const MonitorDef&) = default;
};
inline constexpr std::uint32_t monitor_primary = 0x00000001;  ///< TS_MONITOR_PRIMARY

/// RDPGFX_CAPSET, [MS-RDPEGFX] 2.2.1.6, with the version-specific capsData of
/// 2.2.3. For versions the specification defines, capsDataLength follows
/// from the version and `unknown_data` stays empty. For other versions,
/// `unknown_data` is capsData as sent and is what gets encoded; `flags` is
/// its first four bytes, if it has that many, for information only.
struct CapabilitySet {
    std::uint32_t version = 0;
    /// The flags field. Always 0 for 10.1, which has 16 reserved bytes instead.
    std::uint32_t flags = 0;
    std::vector<std::byte> unknown_data;
    friend bool operator==(const CapabilitySet&, const CapabilitySet&) = default;
};

/// A capability set with these flags. For versions the specification does
/// not define, capsData becomes the four flag bytes.
[[nodiscard]] CapabilitySet make_capability_set(std::uint32_t version, std::uint32_t flags);

/// [MS-RDPEGFX] 2.2.2.11: StartFrame.timestamp packs a UTC time of day as
/// milliseconds (bits 0-9), seconds (10-15), minutes (16-21) and hours
/// (22-31); 0 means "no timestamp".
[[nodiscard]] constexpr bool valid_timestamp(std::uint32_t timestamp) noexcept
{
    return (timestamp & 0x3FFU) <= 999 && ((timestamp >> 10U) & 0x3FU) <= 59 && ((timestamp >> 16U) & 0x3FU) <= 59 &&
           (timestamp >> 22U) <= 23;
}
/// Packs a StartFrame timestamp. The values must be in range (asserted).
[[nodiscard]] std::uint32_t make_timestamp(std::uint32_t hours, std::uint32_t minutes, std::uint32_t seconds,
                                           std::uint32_t milliseconds);

// Graphics messages, [MS-RDPEGFX] 2.2.2 ----------------------------------
//
// `bitmap_data` spans refer into the decoded input, or into the caller's
// buffer when encoding; they are not owned.

/// RDPGFX_WIRE_TO_SURFACE_PDU_1, [MS-RDPEGFX] 2.2.2.1 (server to client).
struct WireToSurface1 {
    static constexpr std::uint16_t command = cmd::wire_to_surface_1;
    std::uint16_t surface_id = 0;
    std::uint16_t codec_id = 0;
    std::uint8_t pixel_format = pixel_format::xrgb_8888;
    Rect16 dest_rect;
    std::span<const std::byte> bitmap_data;

    friend bool operator==(const WireToSurface1& a, const WireToSurface1& b) noexcept
    {
        return a.surface_id == b.surface_id && a.codec_id == b.codec_id && a.pixel_format == b.pixel_format &&
               a.dest_rect == b.dest_rect && std::ranges::equal(a.bitmap_data, b.bitmap_data);
    }
};

/// RDPGFX_WIRE_TO_SURFACE_PDU_2, [MS-RDPEGFX] 2.2.2.2 (server to client).
/// Note the bitmapDataLength field: leaving it out made mstsc fail with
/// 0x8007006f in macRDP.
struct WireToSurface2 {
    static constexpr std::uint16_t command = cmd::wire_to_surface_2;
    std::uint16_t surface_id = 0;
    std::uint16_t codec_id = codec::progressive;
    std::uint32_t codec_context_id = 0;
    std::uint8_t pixel_format = pixel_format::xrgb_8888;
    std::span<const std::byte> bitmap_data;

    friend bool operator==(const WireToSurface2& a, const WireToSurface2& b) noexcept
    {
        return a.surface_id == b.surface_id && a.codec_id == b.codec_id && a.codec_context_id == b.codec_context_id &&
               a.pixel_format == b.pixel_format && std::ranges::equal(a.bitmap_data, b.bitmap_data);
    }
};

/// RDPGFX_DELETE_ENCODING_CONTEXT_PDU, [MS-RDPEGFX] 2.2.2.3.
struct DeleteEncodingContext {
    static constexpr std::uint16_t command = cmd::delete_encoding_context;
    std::uint16_t surface_id = 0;
    std::uint32_t codec_context_id = 0;
    friend bool operator==(const DeleteEncodingContext&, const DeleteEncodingContext&) = default;
};

/// RDPGFX_SOLIDFILL_PDU, [MS-RDPEGFX] 2.2.2.4.
struct SolidFill {
    static constexpr std::uint16_t command = cmd::solid_fill;
    std::uint16_t surface_id = 0;
    Color32 fill_pixel;
    std::vector<Rect16> fill_rects;
    friend bool operator==(const SolidFill&, const SolidFill&) = default;
};

/// RDPGFX_SURFACE_TO_SURFACE_PDU, [MS-RDPEGFX] 2.2.2.5.
struct SurfaceToSurface {
    static constexpr std::uint16_t command = cmd::surface_to_surface;
    std::uint16_t surface_id_src = 0;
    std::uint16_t surface_id_dest = 0;
    Rect16 rect_src;
    std::vector<Point16> dest_pts;
    friend bool operator==(const SurfaceToSurface&, const SurfaceToSurface&) = default;
};

/// RDPGFX_SURFACE_TO_CACHE_PDU, [MS-RDPEGFX] 2.2.2.6. cacheSlot is one-based.
struct SurfaceToCache {
    static constexpr std::uint16_t command = cmd::surface_to_cache;
    std::uint16_t surface_id = 0;
    std::uint64_t cache_key = 0;
    std::uint16_t cache_slot = 0;
    Rect16 rect_src;
    friend bool operator==(const SurfaceToCache&, const SurfaceToCache&) = default;
};

/// RDPGFX_CACHE_TO_SURFACE_PDU, [MS-RDPEGFX] 2.2.2.7.
struct CacheToSurface {
    static constexpr std::uint16_t command = cmd::cache_to_surface;
    std::uint16_t cache_slot = 0;
    std::uint16_t surface_id = 0;
    std::vector<Point16> dest_pts;
    friend bool operator==(const CacheToSurface&, const CacheToSurface&) = default;
};

/// RDPGFX_EVICT_CACHE_ENTRY_PDU, [MS-RDPEGFX] 2.2.2.8.
struct EvictCacheEntry {
    static constexpr std::uint16_t command = cmd::evict_cache_entry;
    std::uint16_t cache_slot = 0;
    friend bool operator==(const EvictCacheEntry&, const EvictCacheEntry&) = default;
};

/// RDPGFX_CREATE_SURFACE_PDU, [MS-RDPEGFX] 2.2.2.9.
struct CreateSurface {
    static constexpr std::uint16_t command = cmd::create_surface;
    std::uint16_t surface_id = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t pixel_format = pixel_format::xrgb_8888;
    friend bool operator==(const CreateSurface&, const CreateSurface&) = default;
};

/// RDPGFX_DELETE_SURFACE_PDU, [MS-RDPEGFX] 2.2.2.10.
struct DeleteSurface {
    static constexpr std::uint16_t command = cmd::delete_surface;
    std::uint16_t surface_id = 0;
    friend bool operator==(const DeleteSurface&, const DeleteSurface&) = default;
};

/// RDPGFX_START_FRAME_PDU, [MS-RDPEGFX] 2.2.2.11. See `valid_timestamp`.
struct StartFrame {
    static constexpr std::uint16_t command = cmd::start_frame;
    std::uint32_t timestamp = 0;
    std::uint32_t frame_id = 0;
    friend bool operator==(const StartFrame&, const StartFrame&) = default;
};

/// RDPGFX_END_FRAME_PDU, [MS-RDPEGFX] 2.2.2.12.
struct EndFrame {
    static constexpr std::uint16_t command = cmd::end_frame;
    std::uint32_t frame_id = 0;
    friend bool operator==(const EndFrame&, const EndFrame&) = default;
};

/// RDPGFX_FRAME_ACKNOWLEDGE_PDU, [MS-RDPEGFX] 2.2.2.13 (client to server).
struct FrameAcknowledge {
    static constexpr std::uint16_t command = cmd::frame_acknowledge;
    std::uint32_t queue_depth = queue_depth_unavailable;
    std::uint32_t frame_id = 0;
    std::uint32_t total_frames_decoded = 0;
    friend bool operator==(const FrameAcknowledge&, const FrameAcknowledge&) = default;
};

/// RDPGFX_RESET_GRAPHICS_PDU, [MS-RDPEGFX] 2.2.2.14. Encoded padded to 340
/// bytes; the decoder requires exactly 340 and ignores the padding.
struct ResetGraphics {
    static constexpr std::uint16_t command = cmd::reset_graphics;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<MonitorDef> monitors;  ///< at most 16
    friend bool operator==(const ResetGraphics&, const ResetGraphics&) = default;
};

/// RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU, [MS-RDPEGFX] 2.2.2.15.
struct MapSurfaceToOutput {
    static constexpr std::uint16_t command = cmd::map_surface_to_output;
    std::uint16_t surface_id = 0;
    std::uint32_t output_origin_x = 0;
    std::uint32_t output_origin_y = 0;
    friend bool operator==(const MapSurfaceToOutput&, const MapSurfaceToOutput&) = default;
};

/// RDPGFX_CACHE_ENTRY_METADATA, [MS-RDPEGFX] 2.2.2.16.1.
struct CacheEntryMetadata {
    std::uint64_t cache_key = 0;
    std::uint32_t bitmap_length = 0;
    friend bool operator==(const CacheEntryMetadata&, const CacheEntryMetadata&) = default;
};

/// RDPGFX_CACHE_IMPORT_OFFER_PDU, [MS-RDPEGFX] 2.2.2.16 (client to server),
/// at most 5461 entries.
struct CacheImportOffer {
    static constexpr std::uint16_t command = cmd::cache_import_offer;
    std::vector<CacheEntryMetadata> entries;
    friend bool operator==(const CacheImportOffer&, const CacheImportOffer&) = default;
};

/// RDPGFX_CACHE_IMPORT_REPLY_PDU, [MS-RDPEGFX] 2.2.2.17: the slot assigned
/// to each of the first N offered entries.
struct CacheImportReply {
    static constexpr std::uint16_t command = cmd::cache_import_reply;
    std::vector<std::uint16_t> cache_slots;
    friend bool operator==(const CacheImportReply&, const CacheImportReply&) = default;
};

/// RDPGFX_CAPS_ADVERTISE_PDU, [MS-RDPEGFX] 2.2.2.18 (client to server). The
/// decoder requires at least one set, at most `max_caps_sets`, no version
/// twice (3.3.5.18), and the exact capsDataLength for defined versions.
struct CapsAdvertise {
    static constexpr std::uint16_t command = cmd::caps_advertise;
    std::vector<CapabilitySet> caps_sets;
    friend bool operator==(const CapsAdvertise&, const CapsAdvertise&) = default;
};

/// RDPGFX_CAPS_CONFIRM_PDU, [MS-RDPEGFX] 2.2.2.19.
struct CapsConfirm {
    static constexpr std::uint16_t command = cmd::caps_confirm;
    CapabilitySet caps_set;
    friend bool operator==(const CapsConfirm&, const CapsConfirm&) = default;
};

/// RDPGFX_MAP_SURFACE_TO_WINDOW_PDU, [MS-RDPEGFX] 2.2.2.20 (RAIL).
struct MapSurfaceToWindow {
    static constexpr std::uint16_t command = cmd::map_surface_to_window;
    std::uint16_t surface_id = 0;
    std::uint64_t window_id = 0;
    std::uint32_t mapped_width = 0;
    std::uint32_t mapped_height = 0;
    friend bool operator==(const MapSurfaceToWindow&, const MapSurfaceToWindow&) = default;
};

/// RDPGFX_QOE_FRAME_ACKNOWLEDGE_PDU, [MS-RDPEGFX] 2.2.2.21 (client to server).
struct QoeFrameAcknowledge {
    static constexpr std::uint16_t command = cmd::qoe_frame_acknowledge;
    std::uint32_t frame_id = 0;
    std::uint32_t timestamp = 0;      ///< ms, client clock with arbitrary origin
    std::uint16_t time_diff_se = 0;   ///< ms from StartFrame to EndFrame decoding
    std::uint16_t time_diff_edr = 0;  ///< ms from EndFrame decoding to rendered
    friend bool operator==(const QoeFrameAcknowledge&, const QoeFrameAcknowledge&) = default;
};

/// RDPGFX_MAP_SURFACE_TO_SCALED_OUTPUT_PDU, [MS-RDPEGFX] 2.2.2.22.
struct MapSurfaceToScaledOutput {
    static constexpr std::uint16_t command = cmd::map_surface_to_scaled_output;
    std::uint16_t surface_id = 0;
    std::uint32_t output_origin_x = 0;
    std::uint32_t output_origin_y = 0;
    std::uint32_t target_width = 0;
    std::uint32_t target_height = 0;
    friend bool operator==(const MapSurfaceToScaledOutput&, const MapSurfaceToScaledOutput&) = default;
};

/// RDPGFX_MAP_SURFACE_TO_SCALED_WINDOW_PDU, [MS-RDPEGFX] 2.2.2.23.
struct MapSurfaceToScaledWindow {
    static constexpr std::uint16_t command = cmd::map_surface_to_scaled_window;
    std::uint16_t surface_id = 0;
    std::uint64_t window_id = 0;
    std::uint32_t mapped_width = 0;
    std::uint32_t mapped_height = 0;
    std::uint32_t target_width = 0;
    std::uint32_t target_height = 0;
    friend bool operator==(const MapSurfaceToScaledWindow&, const MapSurfaceToScaledWindow&) = default;
};

using Pdu =
    std::variant<WireToSurface1, WireToSurface2, DeleteEncodingContext, SolidFill, SurfaceToSurface, SurfaceToCache,
                 CacheToSurface, EvictCacheEntry, CreateSurface, DeleteSurface, StartFrame, EndFrame, FrameAcknowledge,
                 ResetGraphics, MapSurfaceToOutput, CacheImportOffer, CacheImportReply, CapsAdvertise, CapsConfirm,
                 MapSurfaceToWindow, QoeFrameAcknowledge, MapSurfaceToScaledOutput, MapSurfaceToScaledWindow>;

/// The cmdId `pdu` is encoded with.
[[nodiscard]] std::uint16_t cmd_id(const Pdu& pdu);

/// The size of the PDU at the start of `stream` (its pduLength), or nullopt
/// while the stream holds less than that. Fails when pduLength is below the
/// header size or above `max_size`.
[[nodiscard]] Result<std::optional<std::size_t>> frame_pdu(std::span<const std::byte> stream,
                                                           std::size_t max_size = max_client_pdu_size);

/// Decodes exactly one PDU, header included, whose pduLength must equal
/// `bytes.size()`. The header flags field "MUST be zero" but is ignored, as
/// FreeRDP does. Every body must fill the PDU exactly. Unknown cmdIds fail
/// with Errc::unsupported. Spans in the result refer into `bytes`.
[[nodiscard]] Result<Pdu> decode_pdu(std::span<const std::byte> bytes);

/// Appends one PDU with its RDPGFX_HEADER (flags 0, exact pduLength).
void encode(Writer& w, const Pdu& pdu);
[[nodiscard]] std::vector<std::byte> encode(const Pdu& pdu);

}  // namespace farland::channels::rdpgfx
