// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/base/error.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/// Video capture redirection, [MS-RDPECAM]: the client's cameras, over a
/// device enumeration channel and one dynamic virtual channel per camera.
/// Each PDU is one DVC message and starts with the two-byte shared header
/// (2.2.1): a version and a message id.
///
/// The enumeration channel ("RDCamera_Device_Enumerator") carries the version
/// handshake and the client's notifications that a camera came or went. Each
/// camera then gets a channel of its own, whose name the client chose and put
/// in the notification; on it the server activates the device, asks for its
/// streams and their media types, starts a stream and then asks for one
/// sample at a time.
///
/// Message ids are unique across both channels, so one decoder serves both;
/// which channel a PDU belongs to is the caller's business (CameraServer
/// keeps them apart).
namespace farland::channels::rdpecam {

/// The enumeration channel's name ([MS-RDPECAM] 2.1).
inline constexpr std::string_view enumerator_channel_name = "RDCamera_Device_Enumerator";

/// Protocol versions ([MS-RDPECAM] 2.2.1): 1 has no properties, 2 adds them.
inline constexpr std::uint8_t version1 = 1;
inline constexpr std::uint8_t version2 = 2;

/// MessageId of the shared header, [MS-RDPECAM] 2.2.1.
namespace msg {
inline constexpr std::uint8_t success_response = 0x01;
inline constexpr std::uint8_t error_response = 0x02;
inline constexpr std::uint8_t select_version_request = 0x03;
inline constexpr std::uint8_t select_version_response = 0x04;
inline constexpr std::uint8_t device_added = 0x05;
inline constexpr std::uint8_t device_removed = 0x06;
inline constexpr std::uint8_t activate_device_request = 0x07;
inline constexpr std::uint8_t deactivate_device_request = 0x08;
inline constexpr std::uint8_t stream_list_request = 0x09;
inline constexpr std::uint8_t stream_list_response = 0x0A;
inline constexpr std::uint8_t media_type_list_request = 0x0B;
inline constexpr std::uint8_t media_type_list_response = 0x0C;
inline constexpr std::uint8_t current_media_type_request = 0x0D;
inline constexpr std::uint8_t current_media_type_response = 0x0E;
inline constexpr std::uint8_t start_streams_request = 0x0F;
inline constexpr std::uint8_t stop_streams_request = 0x10;
inline constexpr std::uint8_t sample_request = 0x11;
inline constexpr std::uint8_t sample_response = 0x12;
inline constexpr std::uint8_t sample_error_response = 0x13;
inline constexpr std::uint8_t property_list_request = 0x14;
inline constexpr std::uint8_t property_list_response = 0x15;
inline constexpr std::uint8_t property_value_request = 0x16;
inline constexpr std::uint8_t property_value_response = 0x17;
inline constexpr std::uint8_t set_property_value_request = 0x18;
}  // namespace msg

/// CAM_ERROR_CODE, [MS-RDPECAM] 2.2.3.2.
namespace error_code {
inline constexpr std::uint32_t none = 0x00000000;
inline constexpr std::uint32_t unexpected = 0x00000001;
inline constexpr std::uint32_t invalid_message = 0x00000002;
inline constexpr std::uint32_t not_initialized = 0x00000003;
inline constexpr std::uint32_t invalid_request = 0x00000004;
inline constexpr std::uint32_t invalid_stream_number = 0x00000005;
inline constexpr std::uint32_t invalid_media_type = 0x00000006;
inline constexpr std::uint32_t out_of_memory = 0x00000007;
inline constexpr std::uint32_t item_not_found = 0x00000008;
inline constexpr std::uint32_t set_not_found = 0x00000009;
inline constexpr std::uint32_t operation_not_supported = 0x0000000A;
}  // namespace error_code

[[nodiscard]] std::string_view error_code_name(std::uint32_t code) noexcept;

/// CAM_MEDIA_FORMAT, [MS-RDPECAM] 2.2.3.10.1.
enum class MediaFormat : std::uint8_t {
    invalid = 0x00,
    h264 = 0x01,
    mjpg = 0x02,
    yuy2 = 0x03,
    nv12 = 0x04,
    i420 = 0x05,
    rgb24 = 0x06,
    rgb32 = 0x07,
};

[[nodiscard]] std::string_view format_name(MediaFormat format) noexcept;
/// Whether a sample in this format has to be decoded before it is pixels:
/// H.264 and Motion JPEG do, the rest are raw frames.
[[nodiscard]] constexpr bool needs_decoding(MediaFormat format) noexcept
{
    return format == MediaFormat::h264 || format == MediaFormat::mjpg;
}
/// Bytes one frame of `width` x `height` takes in an uncompressed format, or
/// 0 for a compressed one (whose frames vary).
[[nodiscard]] std::size_t frame_size(MediaFormat format, std::uint32_t width, std::uint32_t height) noexcept;

/// CAM_MEDIA_TYPE_DESCRIPTION_FLAGS, [MS-RDPECAM] 2.2.3.10.1. FreeRDP's
/// client rejects a description whose flags are neither of these, so a
/// description the server sends back is always one the client offered.
namespace media_flag {
inline constexpr std::uint8_t decoding_required = 0x01;
inline constexpr std::uint8_t bottom_up_image = 0x02;
}  // namespace media_flag

/// CAM_STREAM_FRAME_SOURCE_TYPES and CAM_STREAM_CATEGORY, [MS-RDPECAM] 2.2.3.8.1.
namespace frame_source {
inline constexpr std::uint16_t color = 0x0001;
inline constexpr std::uint16_t infrared = 0x0002;
inline constexpr std::uint16_t custom = 0x0008;
}  // namespace frame_source
inline constexpr std::uint8_t stream_category_capture = 0x01;

/// CAM_PROPERTY_SET and CAM_PROPERTY_MODE, [MS-RDPECAM] 2.2.3.14.1, 2.2.3.16.1.
namespace property_set {
inline constexpr std::uint8_t camera_control = 0x01;
inline constexpr std::uint8_t video_proc_amp = 0x02;
}  // namespace property_set
namespace property_mode {
inline constexpr std::uint8_t manual = 0x01;
inline constexpr std::uint8_t automatic = 0x02;
}  // namespace property_mode

/// A camera's streams, media types and properties are all small lists; these
/// bound what a client may send, so a hostile one cannot make the server
/// allocate without limit. Each is well above what any real camera has.
inline constexpr std::size_t max_streams = 255;
inline constexpr std::size_t max_media_types = 255;
inline constexpr std::size_t max_properties = 255;
/// Longest device name and channel name accepted, in bytes on the wire.
inline constexpr std::size_t max_device_name = 512;
inline constexpr std::size_t max_channel_name = 256;
/// Largest sample accepted: a 4K RGB32 frame with room to spare. A client
/// that sends more has gone wrong, and the frame would not fit a camera
/// consumer's buffer either.
inline constexpr std::size_t max_sample_size = std::size_t{64} * 1024 * 1024;

/// CAM_STREAM_DESCRIPTION, [MS-RDPECAM] 2.2.3.8.1: five bytes.
struct StreamDescription {
    std::uint16_t frame_source_types = frame_source::color;
    std::uint8_t category = stream_category_capture;
    bool selected = false;
    bool can_be_shared = false;

    friend bool operator==(const StreamDescription&, const StreamDescription&) = default;
};

/// CAM_MEDIA_TYPE_DESCRIPTION, [MS-RDPECAM] 2.2.3.10.1: 26 bytes.
struct MediaType {
    MediaFormat format = MediaFormat::invalid;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t frame_rate_numerator = 0;
    std::uint32_t frame_rate_denominator = 1;
    std::uint32_t pixel_aspect_numerator = 1;
    std::uint32_t pixel_aspect_denominator = 1;
    std::uint8_t flags = 0;

    /// Frames per second, rounded down; 0 when the rate is malformed.
    [[nodiscard]] std::uint32_t fps() const noexcept
    {
        return frame_rate_denominator == 0 ? 0 : frame_rate_numerator / frame_rate_denominator;
    }
    friend bool operator==(const MediaType&, const MediaType&) = default;
};
/// Bytes a media type description takes on the wire.
inline constexpr std::size_t media_type_size = 26;

/// CAM_PROPERTY_DESCRIPTION, [MS-RDPECAM] 2.2.3.14.1: 22 bytes.
struct PropertyDescription {
    std::uint8_t property_set = property_set::camera_control;
    std::uint8_t property_id = 0;
    std::uint32_t capabilities = 0;
    std::int32_t min_value = 0;
    std::int32_t max_value = 0;
    std::int32_t step = 0;
    std::int32_t default_value = 0;

    friend bool operator==(const PropertyDescription&, const PropertyDescription&) = default;
};
inline constexpr std::size_t property_description_size = 22;

/// CAM_PROPERTY_VALUE, [MS-RDPECAM] 2.2.3.16.1: five bytes.
struct PropertyValue {
    std::uint8_t mode = property_mode::manual;
    std::int32_t value = 0;

    friend bool operator==(const PropertyValue&, const PropertyValue&) = default;
};

// --- PDUs both sides send ---------------------------------------------------

/// [MS-RDPECAM] 2.2.3.1.
struct SuccessResponse {
    friend bool operator==(const SuccessResponse&, const SuccessResponse&) = default;
};
/// [MS-RDPECAM] 2.2.3.2.
struct ErrorResponse {
    std::uint32_t code = error_code::unexpected;
    friend bool operator==(const ErrorResponse&, const ErrorResponse&) = default;
};

// --- the enumeration channel ------------------------------------------------

/// [MS-RDPECAM] 2.2.2.1, sent by the client when it opens the enumeration
/// channel. The version travels in the shared header, not in a body.
struct SelectVersionRequest {
    std::uint8_t version = version2;
    friend bool operator==(const SelectVersionRequest&, const SelectVersionRequest&) = default;
};
/// [MS-RDPECAM] 2.2.2.2, the server's answer: the version both will speak.
struct SelectVersionResponse {
    std::uint8_t version = version2;
    friend bool operator==(const SelectVersionResponse&, const SelectVersionResponse&) = default;
};
/// [MS-RDPECAM] 2.2.2.3: the camera's name as UTF-16, then the name of the
/// channel the server is to open for it, both NUL-terminated.
struct DeviceAdded {
    std::string device_name;
    std::string channel_name;
    friend bool operator==(const DeviceAdded&, const DeviceAdded&) = default;
};
/// [MS-RDPECAM] 2.2.2.4: the channel name of a camera that went away.
struct DeviceRemoved {
    std::string channel_name;
    friend bool operator==(const DeviceRemoved&, const DeviceRemoved&) = default;
};

// --- a device channel -------------------------------------------------------

/// [MS-RDPECAM] 2.2.3.3, 2.2.3.4, 2.2.3.7, 2.2.3.13.
struct ActivateDeviceRequest {
    friend bool operator==(const ActivateDeviceRequest&, const ActivateDeviceRequest&) = default;
};
struct DeactivateDeviceRequest {
    friend bool operator==(const DeactivateDeviceRequest&, const DeactivateDeviceRequest&) = default;
};
struct StreamListRequest {
    friend bool operator==(const StreamListRequest&, const StreamListRequest&) = default;
};
struct StopStreamsRequest {
    friend bool operator==(const StopStreamsRequest&, const StopStreamsRequest&) = default;
};
struct PropertyListRequest {
    friend bool operator==(const PropertyListRequest&, const PropertyListRequest&) = default;
};

/// [MS-RDPECAM] 2.2.3.8: as many descriptions as fill the PDU.
struct StreamListResponse {
    std::vector<StreamDescription> streams;
    friend bool operator==(const StreamListResponse&, const StreamListResponse&) = default;
};
/// [MS-RDPECAM] 2.2.3.9, 2.2.3.11, 2.2.3.12.
struct MediaTypeListRequest {
    std::uint8_t stream_index = 0;
    friend bool operator==(const MediaTypeListRequest&, const MediaTypeListRequest&) = default;
};
struct CurrentMediaTypeRequest {
    std::uint8_t stream_index = 0;
    friend bool operator==(const CurrentMediaTypeRequest&, const CurrentMediaTypeRequest&) = default;
};
struct SampleRequest {
    std::uint8_t stream_index = 0;
    friend bool operator==(const SampleRequest&, const SampleRequest&) = default;
};
/// [MS-RDPECAM] 2.2.3.10: as many descriptions as fill the PDU.
struct MediaTypeListResponse {
    std::vector<MediaType> media_types;
    friend bool operator==(const MediaTypeListResponse&, const MediaTypeListResponse&) = default;
};
struct CurrentMediaTypeResponse {
    MediaType media_type;
    friend bool operator==(const CurrentMediaTypeResponse&, const CurrentMediaTypeResponse&) = default;
};
/// [MS-RDPECAM] 2.2.3.5: one stream index and the media type it is to use.
/// The specification allows several; every implementation sends one, and
/// FreeRDP's client reads exactly one.
struct StartStreamsRequest {
    std::uint8_t stream_index = 0;
    MediaType media_type;
    friend bool operator==(const StartStreamsRequest&, const StartStreamsRequest&) = default;
};
/// [MS-RDPECAM] 2.2.3.6. `sample` points into the decoded message.
struct SampleResponse {
    std::uint8_t stream_index = 0;
    std::span<const std::byte> sample;
};
/// [MS-RDPECAM] 2.2.3.6.1.
struct SampleErrorResponse {
    std::uint8_t stream_index = 0;
    std::uint32_t code = error_code::unexpected;
    friend bool operator==(const SampleErrorResponse&, const SampleErrorResponse&) = default;
};
/// [MS-RDPECAM] 2.2.3.14 to 2.2.3.17.
struct PropertyListResponse {
    std::vector<PropertyDescription> properties;
    friend bool operator==(const PropertyListResponse&, const PropertyListResponse&) = default;
};
struct PropertyValueRequest {
    std::uint8_t property_set = property_set::camera_control;
    std::uint8_t property_id = 0;
    friend bool operator==(const PropertyValueRequest&, const PropertyValueRequest&) = default;
};
struct PropertyValueResponse {
    PropertyValue value;
    friend bool operator==(const PropertyValueResponse&, const PropertyValueResponse&) = default;
};
struct SetPropertyValueRequest {
    std::uint8_t property_set = property_set::camera_control;
    std::uint8_t property_id = 0;
    PropertyValue value;
    friend bool operator==(const SetPropertyValueRequest&, const SetPropertyValueRequest&) = default;
};

using ServerPdu =
    std::variant<SuccessResponse, ErrorResponse, SelectVersionResponse, ActivateDeviceRequest, DeactivateDeviceRequest,
                 StreamListRequest, MediaTypeListRequest, CurrentMediaTypeRequest, StartStreamsRequest,
                 StopStreamsRequest, SampleRequest, PropertyListRequest, PropertyValueRequest, SetPropertyValueRequest>;

using ClientPdu = std::variant<SuccessResponse, ErrorResponse, SelectVersionRequest, DeviceAdded, DeviceRemoved,
                               StreamListResponse, MediaTypeListResponse, CurrentMediaTypeResponse, SampleResponse,
                               SampleErrorResponse, PropertyListResponse, PropertyValueResponse>;

/// Encodes a PDU with `version` in its shared header.
[[nodiscard]] std::vector<std::byte> encode_server_pdu(const ServerPdu& pdu, std::uint8_t version = version2);
[[nodiscard]] std::vector<std::byte> encode_client_pdu(const ClientPdu& pdu, std::uint8_t version = version2);

/// Decodes one PDU. Strict: the fields must fill the message exactly, list
/// PDUs must hold a whole number of descriptions and no more than the limits
/// above, a media type's rates and aspect ratio must not be zero, and a PDU
/// the other side never sends is an error. SampleResponse::sample points into
/// `message`.
[[nodiscard]] Result<ClientPdu> decode_client_pdu(std::span<const std::byte> message);
[[nodiscard]] Result<ServerPdu> decode_server_pdu(std::span<const std::byte> message);

}  // namespace farland::channels::rdpecam
