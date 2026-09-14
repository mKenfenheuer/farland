// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>
#include <farland/base/reader.hpp>
#include <farland/base/writer.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

/// Capability sets exchanged in Demand Active and Confirm Active,
/// [MS-RDPBCGR] 2.2.7. The sets farland acts on are decoded into structs; the
/// rest are kept as raw bodies.
namespace farland::proto::caps {

/// capabilitySetType values, [MS-RDPBCGR] 2.2.1.13.1.1.1.
namespace type {
inline constexpr std::uint16_t general = 1;
inline constexpr std::uint16_t bitmap = 2;
inline constexpr std::uint16_t order = 3;
inline constexpr std::uint16_t bitmap_cache = 4;
inline constexpr std::uint16_t control = 5;
inline constexpr std::uint16_t activation = 7;
inline constexpr std::uint16_t pointer = 8;
inline constexpr std::uint16_t share = 9;
inline constexpr std::uint16_t color_cache = 10;
inline constexpr std::uint16_t sound = 12;
inline constexpr std::uint16_t input = 13;
inline constexpr std::uint16_t font = 14;
inline constexpr std::uint16_t brush = 15;
inline constexpr std::uint16_t glyph_cache = 16;
inline constexpr std::uint16_t offscreen_cache = 17;
inline constexpr std::uint16_t bitmap_cache_host_support = 18;
inline constexpr std::uint16_t bitmap_cache_rev2 = 19;
inline constexpr std::uint16_t virtual_channel = 20;
inline constexpr std::uint16_t draw_nine_grid_cache = 21;
inline constexpr std::uint16_t draw_gdiplus = 22;
inline constexpr std::uint16_t rail = 23;
inline constexpr std::uint16_t window = 24;
inline constexpr std::uint16_t desktop_composition = 25;
inline constexpr std::uint16_t multifragment_update = 26;
inline constexpr std::uint16_t large_pointer = 27;
inline constexpr std::uint16_t surface_commands = 28;
inline constexpr std::uint16_t bitmap_codecs = 29;
inline constexpr std::uint16_t frame_acknowledge = 30;
}  // namespace type

/// TS_GENERAL_CAPABILITYSET, [MS-RDPBCGR] 2.2.7.1.1.
struct General {
    std::uint16_t os_major_type = 0;
    std::uint16_t os_minor_type = 0;
    std::uint16_t protocol_version = 0x0200;
    std::uint16_t general_compression_types = 0;
    std::uint16_t extra_flags = 0;
    std::uint16_t update_capability_flag = 0;
    std::uint16_t remote_unshare_flag = 0;
    std::uint16_t general_compression_level = 0;
    std::uint8_t refresh_rect_support = 0;
    std::uint8_t suppress_output_support = 0;
};
namespace general_extra_flags {
inline constexpr std::uint16_t fastpath_output_supported = 0x0001;
inline constexpr std::uint16_t long_credentials_supported = 0x0004;
inline constexpr std::uint16_t autoreconnect_supported = 0x0008;
inline constexpr std::uint16_t enc_salted_checksum = 0x0010;
inline constexpr std::uint16_t no_bitmap_compression_hdr = 0x0400;
}  // namespace general_extra_flags
namespace os_major {
inline constexpr std::uint16_t windows = 0x0001;
inline constexpr std::uint16_t unix = 0x0004;
}  // namespace os_major

/// TS_BITMAP_CAPABILITYSET, [MS-RDPBCGR] 2.2.7.1.2.
struct Bitmap {
    std::uint16_t preferred_bits_per_pixel = 32;
    std::uint16_t receive_1_bit_per_pixel = 1;
    std::uint16_t receive_4_bits_per_pixel = 1;
    std::uint16_t receive_8_bits_per_pixel = 1;
    std::uint16_t desktop_width = 0;
    std::uint16_t desktop_height = 0;
    std::uint16_t desktop_resize_flag = 1;
    std::uint16_t bitmap_compression_flag = 1;
    std::uint8_t high_color_flags = 0;
    std::uint8_t drawing_flags = 0;
    std::uint16_t multiple_rectangle_support = 1;
};

/// TS_ORDER_CAPABILITYSET, [MS-RDPBCGR] 2.2.7.1.3.
struct Order {
    std::array<std::byte, 16> terminal_descriptor{};
    std::uint16_t desktop_save_x_granularity = 1;
    std::uint16_t desktop_save_y_granularity = 20;
    std::uint16_t maximum_order_level = 1;
    std::uint16_t number_fonts = 0;
    std::uint16_t order_flags = 0x0022;  ///< NEGOTIATEORDERSUPPORT | COLORINDEXSUPPORT
    std::array<std::uint8_t, 32> order_support{};
    std::uint16_t text_flags = 0;
    std::uint16_t order_support_ex_flags = 0;
    std::uint32_t desktop_save_size = 480 * 480;
    std::uint16_t text_ansi_code_page = 0;
};

/// TS_POINTER_CAPABILITYSET, [MS-RDPBCGR] 2.2.7.1.5.
struct Pointer {
    std::uint16_t color_pointer_flag = 1;
    std::uint16_t color_pointer_cache_size = 25;
    std::optional<std::uint16_t> pointer_cache_size = 25;
};

/// TS_INPUT_CAPABILITYSET, [MS-RDPBCGR] 2.2.7.1.6.
struct Input {
    std::uint16_t input_flags = 0;
    std::uint32_t keyboard_layout = 0;
    std::uint32_t keyboard_type = 0;
    std::uint32_t keyboard_subtype = 0;
    std::uint32_t keyboard_function_keys = 0;
    std::string ime_file_name;
};
namespace input_flags {
inline constexpr std::uint16_t scancodes = 0x0001;
inline constexpr std::uint16_t mousex = 0x0004;
inline constexpr std::uint16_t fastpath_input = 0x0008;
inline constexpr std::uint16_t unicode = 0x0010;
inline constexpr std::uint16_t fastpath_input2 = 0x0020;
inline constexpr std::uint16_t mouse_relative = 0x0080;
inline constexpr std::uint16_t mouse_hwheel = 0x0100;
inline constexpr std::uint16_t qoe_timestamps = 0x0200;
}  // namespace input_flags

/// TS_VIRTUALCHANNEL_CAPABILITYSET, [MS-RDPBCGR] 2.2.7.1.10.
struct VirtualChannel {
    std::uint32_t flags = 0;  ///< VCCAPS_NO_COMPR
    std::optional<std::uint32_t> chunk_size;
};

/// TS_SHARE_CAPABILITYSET (2.2.7.2.4), TS_FONT_CAPABILITYSET (2.2.7.2.5) and
/// TS_COLORTABLE_CAPABILITYSET (2.2.7.2.1).
struct Share {
    std::uint16_t node_id = 0;
};
struct Font {
    std::uint16_t support_flags = 0x0001;  ///< FONTSUPPORT_FONTLIST
};
struct ColorCache {
    std::uint16_t cache_size = 6;
};

/// TS_MULTIFRAGMENTUPDATE_CAPABILITYSET, [MS-RDPBCGR] 2.2.7.2.6.
struct MultifragmentUpdate {
    std::uint32_t max_request_size = 0;
};

/// TS_LARGE_POINTER_CAPABILITYSET, [MS-RDPBCGR] 2.2.7.2.7.
struct LargePointer {
    std::uint16_t support_flags = 0;  ///< large_pointer_flags
};
namespace large_pointer_flags {
inline constexpr std::uint16_t size_96x96 = 0x0001;    ///< LARGE_POINTER_FLAG_96x96
inline constexpr std::uint16_t size_384x384 = 0x0002;  ///< LARGE_POINTER_FLAG_384x384, Fast-Path Large Pointer Updates
}  // namespace large_pointer_flags

/// TS_SURFCMDS_CAPABILITYSET, [MS-RDPBCGR] 2.2.7.2.9.
struct SurfaceCommands {
    std::uint32_t cmd_flags = 0;
};

/// TS_FRAME_ACKNOWLEDGE_CAPABILITYSET, [MS-RDPRFX] 2.2.1.3.
struct FrameAcknowledge {
    std::uint32_t max_unacknowledged_frame_count = 0;
};

/// A capability set farland does not decode. The body refers into the input.
struct RawCapabilitySet {
    std::uint16_t type = 0;
    std::span<const std::byte> body;
};

struct CapabilitySets {
    std::optional<General> general;
    std::optional<Bitmap> bitmap;
    std::optional<Order> order;
    std::optional<Pointer> pointer;
    std::optional<Input> input;
    std::optional<VirtualChannel> virtual_channel;
    std::optional<Share> share;
    std::optional<Font> font;
    std::optional<ColorCache> color_cache;
    std::optional<MultifragmentUpdate> multifragment_update;
    std::optional<LargePointer> large_pointer;
    std::optional<SurfaceCommands> surface_commands;
    std::optional<FrameAcknowledge> frame_acknowledge;
    std::vector<RawCapabilitySet> other;
};

/// Decodes `count` capability sets. Unknown sets are kept raw; for a known
/// type that appears twice, the first one wins. Known sets may be longer than
/// farland expects (newer fields are ignored) but not shorter than their
/// mandatory part.
[[nodiscard]] Result<CapabilitySets> decode_capability_sets(Reader& r, std::uint16_t count);

/// Encodes every present set and returns how many were written.
std::uint16_t encode_capability_sets(Writer& w, const CapabilitySets& sets);

}  // namespace farland::proto::caps
