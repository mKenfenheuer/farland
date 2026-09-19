// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/reader.hpp>
#include <farland/base/text.hpp>
#include <farland/base/writer.hpp>
#include <farland/channels/rdpecam.hpp>

#include <bit>
#include <type_traits>

namespace farland::channels::rdpecam {

namespace {

constexpr std::size_t stream_description_size = 5;

void write_media_type(Writer& w, const MediaType& t)
{
    // [MS-RDPECAM] 2.2.3.10.1.
    w.u8(static_cast<std::uint8_t>(t.format));
    w.u32le(t.width);
    w.u32le(t.height);
    w.u32le(t.frame_rate_numerator);
    w.u32le(t.frame_rate_denominator);
    w.u32le(t.pixel_aspect_numerator);
    w.u32le(t.pixel_aspect_denominator);
    w.u8(t.flags);
}

[[nodiscard]] Result<MediaType> read_media_type(Reader& r)
{
    MediaType t;
    FARLAND_TRY(const auto format, r.u8());
    if (format > static_cast<std::uint8_t>(MediaFormat::rgb32)) {
        return fail(Errc::unsupported, "unknown camera media format", r.offset());
    }
    t.format = static_cast<MediaFormat>(format);
    FARLAND_TRY(t.width, r.u32le());
    FARLAND_TRY(t.height, r.u32le());
    FARLAND_TRY(t.frame_rate_numerator, r.u32le());
    FARLAND_TRY(t.frame_rate_denominator, r.u32le());
    FARLAND_TRY(t.pixel_aspect_numerator, r.u32le());
    FARLAND_TRY(t.pixel_aspect_denominator, r.u32le());
    FARLAND_TRY(t.flags, r.u8());
    // A zero denominator would divide by zero in every consumer; a zero
    // numerator means no frames. FreeRDP's client rejects these too.
    if (t.frame_rate_numerator == 0 || t.frame_rate_denominator == 0 || t.pixel_aspect_numerator == 0 ||
        t.pixel_aspect_denominator == 0) {
        return fail(Errc::invalid_value, "camera media type with a zero rate or aspect ratio", r.offset());
    }
    return t;
}

void write_property_value(Writer& w, const PropertyValue& v)
{
    w.u8(v.mode);
    w.u32le(std::bit_cast<std::uint32_t>(v.value));
}

[[nodiscard]] Result<PropertyValue> read_property_value(Reader& r)
{
    PropertyValue v;
    FARLAND_TRY(v.mode, r.u8());
    FARLAND_TRY(const auto value, r.u32le());
    v.value = std::bit_cast<std::int32_t>(value);
    return v;
}

/// A NUL-terminated UTF-16LE string, as [MS-RDPECAM] 2.2.2.3 writes the
/// device name. Rejects one that is not terminated or longer than `limit`.
[[nodiscard]] Result<std::string> read_utf16_string(Reader& r, std::size_t limit)
{
    const auto rest = r.rest();
    for (std::size_t i = 0; i + 1 < rest.size(); i += 2) {
        if (rest[i] == std::byte{0} && rest[i + 1] == std::byte{0}) {
            if (i > limit) {
                return fail(Errc::limit_exceeded, "camera device name too long", r.offset());
            }
            FARLAND_TRY(const auto text, r.bytes(i + 2));
            return utf16le_to_utf8(text.first(i));
        }
    }
    return fail(Errc::truncated, "camera device name is not terminated", r.offset());
}

/// A NUL-terminated byte string, as the virtual channel name is written.
[[nodiscard]] Result<std::string> read_ascii_string(Reader& r, std::size_t limit)
{
    const auto rest = r.rest();
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == std::byte{0}) {
            if (i > limit) {
                return fail(Errc::limit_exceeded, "camera channel name too long", r.offset());
            }
            FARLAND_TRY(const auto text, r.bytes(i + 1));
            std::string out;
            out.reserve(i);
            for (const std::byte b : text.first(i)) {
                out.push_back(static_cast<char>(b));
            }
            return out;
        }
    }
    return fail(Errc::truncated, "camera channel name is not terminated", r.offset());
}

void write_ascii_string(Writer& w, std::string_view text)
{
    for (const char c : text) {
        w.u8(static_cast<std::uint8_t>(c));
    }
    w.u8(0);
}

}  // namespace

std::string_view error_code_name(std::uint32_t code) noexcept
{
    switch (code) {
    case error_code::none:
        return "none";
    case error_code::unexpected:
        return "an unexpected error";
    case error_code::invalid_message:
        return "an invalid message";
    case error_code::not_initialized:
        return "not initialized";
    case error_code::invalid_request:
        return "an invalid request";
    case error_code::invalid_stream_number:
        return "an invalid stream number";
    case error_code::invalid_media_type:
        return "an invalid media type";
    case error_code::out_of_memory:
        return "out of memory";
    case error_code::item_not_found:
        return "the item was not found";
    case error_code::set_not_found:
        return "the property set was not found";
    case error_code::operation_not_supported:
        return "the operation is not supported";
    default:
        return "an unknown error";
    }
}

std::string_view format_name(MediaFormat format) noexcept
{
    switch (format) {
    case MediaFormat::h264:
        return "H264";
    case MediaFormat::mjpg:
        return "MJPG";
    case MediaFormat::yuy2:
        return "YUY2";
    case MediaFormat::nv12:
        return "NV12";
    case MediaFormat::i420:
        return "I420";
    case MediaFormat::rgb24:
        return "RGB24";
    case MediaFormat::rgb32:
        return "RGB32";
    case MediaFormat::invalid:
        break;
    }
    return "invalid";
}

std::size_t frame_size(MediaFormat format, std::uint32_t width, std::uint32_t height) noexcept
{
    const std::size_t pixels = std::size_t{width} * height;
    switch (format) {
    case MediaFormat::yuy2:
        return pixels * 2;
    case MediaFormat::nv12:
    case MediaFormat::i420:
        // One luma plane plus two quarter-size chroma planes, with odd sizes
        // rounded up as every 4:2:0 layout does.
        return pixels + (2 * (((std::size_t{width} + 1) / 2) * ((std::size_t{height} + 1) / 2)));
    case MediaFormat::rgb24:
        return pixels * 3;
    case MediaFormat::rgb32:
        return pixels * 4;
    case MediaFormat::h264:
    case MediaFormat::mjpg:
    case MediaFormat::invalid:
        break;
    }
    return 0;
}

std::vector<std::byte> encode_server_pdu(const ServerPdu& pdu, std::uint8_t version)
{
    Writer w;
    std::visit(
        [&w, version](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, SelectVersionResponse>) {
                // [MS-RDPECAM] 2.2.2.2: the version is the header's.
                w.u8(p.version);
                w.u8(msg::select_version_response);
                return;
            }
            w.u8(version);
            if constexpr (std::is_same_v<T, SuccessResponse>) {
                w.u8(msg::success_response);
            } else if constexpr (std::is_same_v<T, ErrorResponse>) {
                w.u8(msg::error_response);
                w.u32le(p.code);
            } else if constexpr (std::is_same_v<T, ActivateDeviceRequest>) {
                w.u8(msg::activate_device_request);
            } else if constexpr (std::is_same_v<T, DeactivateDeviceRequest>) {
                w.u8(msg::deactivate_device_request);
            } else if constexpr (std::is_same_v<T, StreamListRequest>) {
                w.u8(msg::stream_list_request);
            } else if constexpr (std::is_same_v<T, MediaTypeListRequest>) {
                w.u8(msg::media_type_list_request);
                w.u8(p.stream_index);
            } else if constexpr (std::is_same_v<T, CurrentMediaTypeRequest>) {
                w.u8(msg::current_media_type_request);
                w.u8(p.stream_index);
            } else if constexpr (std::is_same_v<T, StartStreamsRequest>) {
                w.u8(msg::start_streams_request);
                w.u8(p.stream_index);
                write_media_type(w, p.media_type);
            } else if constexpr (std::is_same_v<T, StopStreamsRequest>) {
                w.u8(msg::stop_streams_request);
            } else if constexpr (std::is_same_v<T, SampleRequest>) {
                w.u8(msg::sample_request);
                w.u8(p.stream_index);
            } else if constexpr (std::is_same_v<T, PropertyListRequest>) {
                w.u8(msg::property_list_request);
            } else if constexpr (std::is_same_v<T, PropertyValueRequest>) {
                w.u8(msg::property_value_request);
                w.u8(p.property_set);
                w.u8(p.property_id);
            } else if constexpr (std::is_same_v<T, SetPropertyValueRequest>) {
                w.u8(msg::set_property_value_request);
                w.u8(p.property_set);
                w.u8(p.property_id);
                write_property_value(w, p.value);
            }
        },
        pdu);
    return std::move(w).take();
}

std::vector<std::byte> encode_client_pdu(const ClientPdu& pdu, std::uint8_t version)
{
    Writer w;
    std::visit(
        [&w, version](const auto& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, SelectVersionRequest>) {
                w.u8(p.version);
                w.u8(msg::select_version_request);
                return;
            }
            w.u8(version);
            if constexpr (std::is_same_v<T, SuccessResponse>) {
                w.u8(msg::success_response);
            } else if constexpr (std::is_same_v<T, ErrorResponse>) {
                w.u8(msg::error_response);
                w.u32le(p.code);
            } else if constexpr (std::is_same_v<T, DeviceAdded>) {
                // [MS-RDPECAM] 2.2.2.3: the name in UTF-16, then the channel
                // name in bytes, both with their terminator.
                w.u8(msg::device_added);
                w.bytes(utf8_to_utf16le(p.device_name));
                w.u16le(0);
                write_ascii_string(w, p.channel_name);
            } else if constexpr (std::is_same_v<T, DeviceRemoved>) {
                w.u8(msg::device_removed);
                write_ascii_string(w, p.channel_name);
            } else if constexpr (std::is_same_v<T, StreamListResponse>) {
                w.u8(msg::stream_list_response);
                for (const auto& s : p.streams) {
                    w.u16le(s.frame_source_types);
                    w.u8(s.category);
                    w.u8(s.selected ? 1 : 0);
                    w.u8(s.can_be_shared ? 1 : 0);
                }
            } else if constexpr (std::is_same_v<T, MediaTypeListResponse>) {
                w.u8(msg::media_type_list_response);
                for (const auto& t : p.media_types) {
                    write_media_type(w, t);
                }
            } else if constexpr (std::is_same_v<T, CurrentMediaTypeResponse>) {
                w.u8(msg::current_media_type_response);
                write_media_type(w, p.media_type);
            } else if constexpr (std::is_same_v<T, SampleResponse>) {
                w.u8(msg::sample_response);
                w.u8(p.stream_index);
                w.bytes(p.sample);
            } else if constexpr (std::is_same_v<T, SampleErrorResponse>) {
                w.u8(msg::sample_error_response);
                w.u8(p.stream_index);
                w.u32le(p.code);
            } else if constexpr (std::is_same_v<T, PropertyListResponse>) {
                w.u8(msg::property_list_response);
                for (const auto& d : p.properties) {
                    w.u8(d.property_set);
                    w.u8(d.property_id);
                    w.u32le(d.capabilities);
                    w.u32le(std::bit_cast<std::uint32_t>(d.min_value));
                    w.u32le(std::bit_cast<std::uint32_t>(d.max_value));
                    w.u32le(std::bit_cast<std::uint32_t>(d.step));
                    w.u32le(std::bit_cast<std::uint32_t>(d.default_value));
                }
            } else if constexpr (std::is_same_v<T, PropertyValueResponse>) {
                w.u8(msg::property_value_response);
                write_property_value(w, p.value);
            }
        },
        pdu);
    return std::move(w).take();
}

Result<ClientPdu> decode_client_pdu(std::span<const std::byte> message)
{
    Reader r(message);
    FARLAND_TRY(const auto version, r.u8());
    FARLAND_TRY(const auto id, r.u8());
    switch (id) {
    case msg::success_response:
        FARLAND_TRY_VOID(r.expect_end("Success Response"));
        return SuccessResponse{};
    case msg::error_response: {
        ErrorResponse p;
        FARLAND_TRY(p.code, r.u32le());
        FARLAND_TRY_VOID(r.expect_end("Error Response"));
        return p;
    }
    case msg::select_version_request:
        FARLAND_TRY_VOID(r.expect_end("Select Version Request"));
        return SelectVersionRequest{version};
    case msg::device_added: {
        DeviceAdded p;
        FARLAND_TRY(p.device_name, read_utf16_string(r, max_device_name));
        FARLAND_TRY(p.channel_name, read_ascii_string(r, max_channel_name));
        if (p.channel_name.empty()) {
            return fail(Errc::invalid_value, "camera with an empty channel name", r.offset());
        }
        FARLAND_TRY_VOID(r.expect_end("Device Added Notification"));
        return p;
    }
    case msg::device_removed: {
        DeviceRemoved p;
        FARLAND_TRY(p.channel_name, read_ascii_string(r, max_channel_name));
        FARLAND_TRY_VOID(r.expect_end("Device Removed Notification"));
        return p;
    }
    case msg::stream_list_response: {
        // The count is what fills the PDU ([MS-RDPECAM] 2.2.3.8).
        if (r.remaining() % stream_description_size != 0) {
            return fail(Errc::invalid_length, "Stream List Response is not a whole number of descriptions", r.offset());
        }
        const std::size_t count = r.remaining() / stream_description_size;
        if (count > max_streams) {
            return fail(Errc::limit_exceeded, "too many camera streams", r.offset());
        }
        StreamListResponse p;
        p.streams.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            StreamDescription s;
            FARLAND_TRY(s.frame_source_types, r.u16le());
            FARLAND_TRY(s.category, r.u8());
            FARLAND_TRY(const auto selected, r.u8());
            FARLAND_TRY(const auto shared, r.u8());
            s.selected = selected != 0;
            s.can_be_shared = shared != 0;
            p.streams.push_back(s);
        }
        return p;
    }
    case msg::media_type_list_response: {
        if (r.remaining() % media_type_size != 0) {
            return fail(Errc::invalid_length, "Media Type List Response is not a whole number of descriptions",
                        r.offset());
        }
        const std::size_t count = r.remaining() / media_type_size;
        if (count > max_media_types) {
            return fail(Errc::limit_exceeded, "too many camera media types", r.offset());
        }
        MediaTypeListResponse p;
        p.media_types.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            FARLAND_TRY(auto t, read_media_type(r));
            p.media_types.push_back(t);
        }
        return p;
    }
    case msg::current_media_type_response: {
        CurrentMediaTypeResponse p;
        FARLAND_TRY(p.media_type, read_media_type(r));
        FARLAND_TRY_VOID(r.expect_end("Current Media Type Response"));
        return p;
    }
    case msg::sample_response: {
        SampleResponse p;
        FARLAND_TRY(p.stream_index, r.u8());
        if (r.remaining() > max_sample_size) {
            return fail(Errc::limit_exceeded, "camera sample too large", r.offset());
        }
        p.sample = r.rest();
        return p;
    }
    case msg::sample_error_response: {
        SampleErrorResponse p;
        FARLAND_TRY(p.stream_index, r.u8());
        FARLAND_TRY(p.code, r.u32le());
        FARLAND_TRY_VOID(r.expect_end("Sample Error Response"));
        return p;
    }
    case msg::property_list_response: {
        if (r.remaining() % property_description_size != 0) {
            return fail(Errc::invalid_length, "Property List Response is not a whole number of descriptions",
                        r.offset());
        }
        const std::size_t count = r.remaining() / property_description_size;
        if (count > max_properties) {
            return fail(Errc::limit_exceeded, "too many camera properties", r.offset());
        }
        PropertyListResponse p;
        p.properties.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            PropertyDescription d;
            FARLAND_TRY(d.property_set, r.u8());
            FARLAND_TRY(d.property_id, r.u8());
            FARLAND_TRY(d.capabilities, r.u32le());
            FARLAND_TRY(const auto min_value, r.u32le());
            FARLAND_TRY(const auto max_value, r.u32le());
            FARLAND_TRY(const auto step, r.u32le());
            FARLAND_TRY(const auto default_value, r.u32le());
            d.min_value = std::bit_cast<std::int32_t>(min_value);
            d.max_value = std::bit_cast<std::int32_t>(max_value);
            d.step = std::bit_cast<std::int32_t>(step);
            d.default_value = std::bit_cast<std::int32_t>(default_value);
            p.properties.push_back(d);
        }
        return p;
    }
    case msg::property_value_response: {
        PropertyValueResponse p;
        FARLAND_TRY(p.value, read_property_value(r));
        FARLAND_TRY_VOID(r.expect_end("Property Value Response"));
        return p;
    }
    default:
        return fail(Errc::unsupported, "unknown or unexpected camera client PDU", 1);
    }
}

Result<ServerPdu> decode_server_pdu(std::span<const std::byte> message)
{
    Reader r(message);
    FARLAND_TRY(const auto version, r.u8());
    FARLAND_TRY(const auto id, r.u8());
    switch (id) {
    case msg::success_response:
        FARLAND_TRY_VOID(r.expect_end("Success Response"));
        return SuccessResponse{};
    case msg::error_response: {
        ErrorResponse p;
        FARLAND_TRY(p.code, r.u32le());
        FARLAND_TRY_VOID(r.expect_end("Error Response"));
        return p;
    }
    case msg::select_version_response:
        FARLAND_TRY_VOID(r.expect_end("Select Version Response"));
        return SelectVersionResponse{version};
    case msg::activate_device_request:
        FARLAND_TRY_VOID(r.expect_end("Activate Device Request"));
        return ActivateDeviceRequest{};
    case msg::deactivate_device_request:
        FARLAND_TRY_VOID(r.expect_end("Deactivate Device Request"));
        return DeactivateDeviceRequest{};
    case msg::stream_list_request:
        FARLAND_TRY_VOID(r.expect_end("Stream List Request"));
        return StreamListRequest{};
    case msg::stop_streams_request:
        FARLAND_TRY_VOID(r.expect_end("Stop Streams Request"));
        return StopStreamsRequest{};
    case msg::property_list_request:
        FARLAND_TRY_VOID(r.expect_end("Property List Request"));
        return PropertyListRequest{};
    case msg::media_type_list_request: {
        MediaTypeListRequest p;
        FARLAND_TRY(p.stream_index, r.u8());
        FARLAND_TRY_VOID(r.expect_end("Media Type List Request"));
        return p;
    }
    case msg::current_media_type_request: {
        CurrentMediaTypeRequest p;
        FARLAND_TRY(p.stream_index, r.u8());
        FARLAND_TRY_VOID(r.expect_end("Current Media Type Request"));
        return p;
    }
    case msg::sample_request: {
        SampleRequest p;
        FARLAND_TRY(p.stream_index, r.u8());
        FARLAND_TRY_VOID(r.expect_end("Sample Request"));
        return p;
    }
    case msg::start_streams_request: {
        StartStreamsRequest p;
        FARLAND_TRY(p.stream_index, r.u8());
        FARLAND_TRY(p.media_type, read_media_type(r));
        FARLAND_TRY_VOID(r.expect_end("Start Streams Request"));
        return p;
    }
    case msg::property_value_request: {
        PropertyValueRequest p;
        FARLAND_TRY(p.property_set, r.u8());
        FARLAND_TRY(p.property_id, r.u8());
        FARLAND_TRY_VOID(r.expect_end("Property Value Request"));
        return p;
    }
    case msg::set_property_value_request: {
        SetPropertyValueRequest p;
        FARLAND_TRY(p.property_set, r.u8());
        FARLAND_TRY(p.property_id, r.u8());
        FARLAND_TRY(p.value, read_property_value(r));
        FARLAND_TRY_VOID(r.expect_end("Set Property Value Request"));
        return p;
    }
    default:
        return fail(Errc::unsupported, "unknown or unexpected camera server PDU", 1);
    }
}

}  // namespace farland::channels::rdpecam
