// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/assert.hpp>
#include <farland/base/text.hpp>
#include <farland/proto/capabilities.hpp>

#include <algorithm>

namespace farland::proto::caps {

namespace {

constexpr std::size_t header_size = 4;
constexpr std::size_t ime_file_name_size = 64;

Result<General> decode_general(Reader& r)
{
    General g;
    FARLAND_TRY(g.os_major_type, r.u16le());
    FARLAND_TRY(g.os_minor_type, r.u16le());
    FARLAND_TRY(g.protocol_version, r.u16le());
    FARLAND_TRY_VOID(r.skip(2));  // pad2octetsA
    FARLAND_TRY(g.general_compression_types, r.u16le());
    FARLAND_TRY(g.extra_flags, r.u16le());
    FARLAND_TRY(g.update_capability_flag, r.u16le());
    FARLAND_TRY(g.remote_unshare_flag, r.u16le());
    FARLAND_TRY(g.general_compression_level, r.u16le());
    // refreshRectSupport and suppressOutputSupport are missing from very old clients.
    if (!r.empty()) {
        FARLAND_TRY(g.refresh_rect_support, r.u8());
    }
    if (!r.empty()) {
        FARLAND_TRY(g.suppress_output_support, r.u8());
    }
    return g;
}

void encode_general(Writer& w, const General& g)
{
    w.u16le(g.os_major_type);
    w.u16le(g.os_minor_type);
    w.u16le(g.protocol_version);
    w.u16le(0);
    w.u16le(g.general_compression_types);
    w.u16le(g.extra_flags);
    w.u16le(g.update_capability_flag);
    w.u16le(g.remote_unshare_flag);
    w.u16le(g.general_compression_level);
    w.u8(g.refresh_rect_support);
    w.u8(g.suppress_output_support);
}

Result<Bitmap> decode_bitmap(Reader& r)
{
    Bitmap b;
    FARLAND_TRY(b.preferred_bits_per_pixel, r.u16le());
    FARLAND_TRY(b.receive_1_bit_per_pixel, r.u16le());
    FARLAND_TRY(b.receive_4_bits_per_pixel, r.u16le());
    FARLAND_TRY(b.receive_8_bits_per_pixel, r.u16le());
    FARLAND_TRY(b.desktop_width, r.u16le());
    FARLAND_TRY(b.desktop_height, r.u16le());
    FARLAND_TRY_VOID(r.skip(2));  // pad2octets
    FARLAND_TRY(b.desktop_resize_flag, r.u16le());
    FARLAND_TRY(b.bitmap_compression_flag, r.u16le());
    FARLAND_TRY(b.high_color_flags, r.u8());
    FARLAND_TRY(b.drawing_flags, r.u8());
    FARLAND_TRY(b.multiple_rectangle_support, r.u16le());
    return b;  // pad2octetsB may follow
}

void encode_bitmap(Writer& w, const Bitmap& b)
{
    w.u16le(b.preferred_bits_per_pixel);
    w.u16le(b.receive_1_bit_per_pixel);
    w.u16le(b.receive_4_bits_per_pixel);
    w.u16le(b.receive_8_bits_per_pixel);
    w.u16le(b.desktop_width);
    w.u16le(b.desktop_height);
    w.u16le(0);
    w.u16le(b.desktop_resize_flag);
    w.u16le(b.bitmap_compression_flag);
    w.u8(b.high_color_flags);
    w.u8(b.drawing_flags);
    w.u16le(b.multiple_rectangle_support);
    w.u16le(0);
}

Result<Order> decode_order(Reader& r)
{
    Order o;
    FARLAND_TRY(const auto descriptor, r.bytes(o.terminal_descriptor.size()));
    std::ranges::copy(descriptor, o.terminal_descriptor.begin());
    FARLAND_TRY_VOID(r.skip(4));  // pad4octetsA
    FARLAND_TRY(o.desktop_save_x_granularity, r.u16le());
    FARLAND_TRY(o.desktop_save_y_granularity, r.u16le());
    FARLAND_TRY_VOID(r.skip(2));  // pad2octetsA
    FARLAND_TRY(o.maximum_order_level, r.u16le());
    FARLAND_TRY(o.number_fonts, r.u16le());
    FARLAND_TRY(o.order_flags, r.u16le());
    FARLAND_TRY(const auto support, r.bytes(o.order_support.size()));
    std::ranges::transform(support, o.order_support.begin(),
                           [](std::byte b) { return std::to_integer<std::uint8_t>(b); });
    FARLAND_TRY(o.text_flags, r.u16le());
    FARLAND_TRY(o.order_support_ex_flags, r.u16le());
    FARLAND_TRY_VOID(r.skip(4));  // pad4octetsB
    FARLAND_TRY(o.desktop_save_size, r.u32le());
    FARLAND_TRY_VOID(r.skip(4));  // pad2octetsC, pad2octetsD
    FARLAND_TRY(o.text_ansi_code_page, r.u16le());
    return o;  // pad2octetsE
}

void encode_order(Writer& w, const Order& o)
{
    w.bytes(o.terminal_descriptor);
    w.zeros(4);
    w.u16le(o.desktop_save_x_granularity);
    w.u16le(o.desktop_save_y_granularity);
    w.zeros(2);
    w.u16le(o.maximum_order_level);
    w.u16le(o.number_fonts);
    w.u16le(o.order_flags);
    for (const std::uint8_t support : o.order_support) {
        w.u8(support);
    }
    w.u16le(o.text_flags);
    w.u16le(o.order_support_ex_flags);
    w.zeros(4);
    w.u32le(o.desktop_save_size);
    w.zeros(4);
    w.u16le(o.text_ansi_code_page);
    w.zeros(2);
}

Result<Pointer> decode_pointer(Reader& r)
{
    Pointer p;
    FARLAND_TRY(p.color_pointer_flag, r.u16le());
    FARLAND_TRY(p.color_pointer_cache_size, r.u16le());
    p.pointer_cache_size.reset();
    if (!r.empty()) {
        FARLAND_TRY(p.pointer_cache_size, r.u16le());
    }
    return p;
}

void encode_pointer(Writer& w, const Pointer& p)
{
    w.u16le(p.color_pointer_flag);
    w.u16le(p.color_pointer_cache_size);
    if (p.pointer_cache_size) {
        w.u16le(*p.pointer_cache_size);
    }
}

Result<Input> decode_input(Reader& r)
{
    Input in;
    FARLAND_TRY(in.input_flags, r.u16le());
    FARLAND_TRY_VOID(r.skip(2));  // pad2octetsA
    FARLAND_TRY(in.keyboard_layout, r.u32le());
    FARLAND_TRY(in.keyboard_type, r.u32le());
    FARLAND_TRY(in.keyboard_subtype, r.u32le());
    FARLAND_TRY(in.keyboard_function_keys, r.u32le());
    FARLAND_TRY(const auto ime, r.bytes(ime_file_name_size));
    in.ime_file_name = utf16le_to_utf8(ime);
    return in;
}

void encode_input(Writer& w, const Input& in)
{
    w.u16le(in.input_flags);
    w.u16le(0);
    w.u32le(in.keyboard_layout);
    w.u32le(in.keyboard_type);
    w.u32le(in.keyboard_subtype);
    w.u32le(in.keyboard_function_keys);
    auto ime = utf8_to_utf16le(in.ime_file_name);
    ime.resize(std::min(ime.size(), ime_file_name_size - 2));
    w.bytes(ime);
    w.zeros(ime_file_name_size - ime.size());
}

Result<VirtualChannel> decode_virtual_channel(Reader& r)
{
    VirtualChannel vc;
    FARLAND_TRY(vc.flags, r.u32le());
    if (!r.empty()) {
        FARLAND_TRY(vc.chunk_size, r.u32le());
    }
    return vc;
}

template <class T>
void keep_first(std::optional<T>& slot, T value)
{
    if (!slot) {
        slot = std::move(value);
    }
}

/// Decodes a set of a known type into its slot.
template <class T, class Decode>
Result<void> decode_into(std::optional<T>& slot, Reader& body, Decode decode)
{
    FARLAND_TRY(T value, decode(body));
    keep_first(slot, std::move(value));
    return {};
}

std::size_t begin_set(Writer& w, std::uint16_t set_type)
{
    const std::size_t start = w.size();
    w.u16le(set_type);
    w.u16le(0);  // lengthCapability, patched by end_set
    return start;
}

void end_set(Writer& w, std::size_t start)
{
    const std::size_t length = w.size() - start;
    FARLAND_ASSERT(length <= 0xFFFF);
    w.patch_u16le(start + 2, static_cast<std::uint16_t>(length));
}

}  // namespace

Result<CapabilitySets> decode_capability_sets(Reader& r, std::uint16_t count)
{
    CapabilitySets sets;
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::size_t start = r.offset();
        FARLAND_TRY(const std::uint16_t set_type, r.u16le());
        FARLAND_TRY(const std::uint16_t length, r.u16le());
        if (length < header_size) {
            return fail(Errc::invalid_length, "capability set shorter than its header", start);
        }
        FARLAND_TRY(Reader body, r.sub(length - header_size));
        switch (set_type) {
        case type::general:
            FARLAND_TRY_VOID(decode_into(sets.general, body, decode_general));
            break;
        case type::bitmap:
            FARLAND_TRY_VOID(decode_into(sets.bitmap, body, decode_bitmap));
            break;
        case type::order:
            FARLAND_TRY_VOID(decode_into(sets.order, body, decode_order));
            break;
        case type::pointer:
            FARLAND_TRY_VOID(decode_into(sets.pointer, body, decode_pointer));
            break;
        case type::input:
            FARLAND_TRY_VOID(decode_into(sets.input, body, decode_input));
            break;
        case type::virtual_channel:
            FARLAND_TRY_VOID(decode_into(sets.virtual_channel, body, decode_virtual_channel));
            break;
        case type::share:
            FARLAND_TRY_VOID(decode_into(sets.share, body, [](Reader& in) -> Result<Share> {
                FARLAND_TRY(const std::uint16_t node_id, in.u16le());
                return Share{node_id};
            }));
            break;
        case type::font:
            // Windows servers send an empty body (length 4, [MS-RDPBCGR] 4.1.12).
            FARLAND_TRY_VOID(decode_into(sets.font, body, [](Reader& in) -> Result<Font> {
                if (in.empty()) {
                    return Font{};
                }
                FARLAND_TRY(const std::uint16_t flags, in.u16le());
                return Font{flags};
            }));
            break;
        case type::color_cache:
            FARLAND_TRY_VOID(decode_into(sets.color_cache, body, [](Reader& in) -> Result<ColorCache> {
                FARLAND_TRY(const std::uint16_t size, in.u16le());
                return ColorCache{size};
            }));
            break;
        case type::multifragment_update:
            FARLAND_TRY_VOID(
                decode_into(sets.multifragment_update, body, [](Reader& in) -> Result<MultifragmentUpdate> {
                    FARLAND_TRY(const std::uint32_t size, in.u32le());
                    return MultifragmentUpdate{size};
                }));
            break;
        case type::large_pointer:
            FARLAND_TRY_VOID(decode_into(sets.large_pointer, body, [](Reader& in) -> Result<LargePointer> {
                FARLAND_TRY(const std::uint16_t flags, in.u16le());
                return LargePointer{flags};
            }));
            break;
        case type::surface_commands:
            FARLAND_TRY_VOID(decode_into(sets.surface_commands, body, [](Reader& in) -> Result<SurfaceCommands> {
                FARLAND_TRY(const std::uint32_t flags, in.u32le());
                return SurfaceCommands{flags};
            }));
            break;
        case type::frame_acknowledge:
            FARLAND_TRY_VOID(decode_into(sets.frame_acknowledge, body, [](Reader& in) -> Result<FrameAcknowledge> {
                FARLAND_TRY(const std::uint32_t frames, in.u32le());
                return FrameAcknowledge{frames};
            }));
            break;
        default:
            sets.other.push_back(RawCapabilitySet{set_type, body.rest()});
            break;
        }
    }
    return sets;
}

std::uint16_t encode_capability_sets(Writer& w, const CapabilitySets& sets)
{
    std::uint16_t count = 0;
    const auto emit = [&](std::uint16_t set_type, auto&& body) {
        const std::size_t start = begin_set(w, set_type);
        body();
        end_set(w, start);
        ++count;
    };
    if (sets.general) {
        emit(type::general, [&] { encode_general(w, *sets.general); });
    }
    if (sets.bitmap) {
        emit(type::bitmap, [&] { encode_bitmap(w, *sets.bitmap); });
    }
    if (sets.order) {
        emit(type::order, [&] { encode_order(w, *sets.order); });
    }
    if (sets.pointer) {
        emit(type::pointer, [&] { encode_pointer(w, *sets.pointer); });
    }
    if (sets.input) {
        emit(type::input, [&] { encode_input(w, *sets.input); });
    }
    if (sets.virtual_channel) {
        emit(type::virtual_channel, [&] {
            w.u32le(sets.virtual_channel->flags);
            if (sets.virtual_channel->chunk_size) {
                w.u32le(*sets.virtual_channel->chunk_size);
            }
        });
    }
    if (sets.share) {
        emit(type::share, [&] {
            w.u16le(sets.share->node_id);
            w.u16le(0);
        });
    }
    if (sets.font) {
        emit(type::font, [&] {
            w.u16le(sets.font->support_flags);
            w.u16le(0);
        });
    }
    if (sets.color_cache) {
        emit(type::color_cache, [&] {
            w.u16le(sets.color_cache->cache_size);
            w.u16le(0);
        });
    }
    if (sets.multifragment_update) {
        emit(type::multifragment_update, [&] { w.u32le(sets.multifragment_update->max_request_size); });
    }
    if (sets.large_pointer) {
        emit(type::large_pointer, [&] { w.u16le(sets.large_pointer->support_flags); });
    }
    if (sets.surface_commands) {
        emit(type::surface_commands, [&] {
            w.u32le(sets.surface_commands->cmd_flags);
            w.u32le(0);  // reserved
        });
    }
    if (sets.frame_acknowledge) {
        emit(type::frame_acknowledge, [&] { w.u32le(sets.frame_acknowledge->max_unacknowledged_frame_count); });
    }
    for (const auto& raw : sets.other) {
        emit(raw.type, [&] { w.bytes(raw.body); });
    }
    return count;
}

}  // namespace farland::proto::caps
