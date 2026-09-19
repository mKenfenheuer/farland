// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The [MS-RDPECAM] codec: the wire layout against the bytes FreeRDP's client
// reads and writes (channels/rdpecam/client/camera_device_main.c), round
// trips, and strict decoding.

#include <farland/channels/rdpecam.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <variant>
#include <vector>

namespace cam = farland::channels::rdpecam;
using farland::Errc;
using Bytes = std::vector<std::byte>;

namespace {

Bytes bytes_of(std::initializer_list<int> values)
{
    Bytes out;
    out.reserve(values.size());
    for (const int v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

/// The error of a result that must have failed.
template <class T>
Errc error_code(const farland::Result<T>& result)
{
    REQUIRE(!result.has_value());
    return result.error().code;
}

cam::MediaType nv12_720p()
{
    return {.format = cam::MediaFormat::nv12,
            .width = 1280,
            .height = 720,
            .frame_rate_numerator = 30,
            .frame_rate_denominator = 1,
            .pixel_aspect_numerator = 1,
            .pixel_aspect_denominator = 1,
            .flags = 0};
}

}  // namespace

TEST_CASE("Camera PDUs have the two-byte shared header", "[channels][rdpecam]")
{
    // [MS-RDPECAM] 2.2.1: Version, then MessageId.
    const auto request = cam::encode_server_pdu(cam::StreamListRequest{}, cam::version2);
    CHECK(request == bytes_of({0x02, 0x09}));

    // The version handshake carries its version in the header itself, and
    // the client opens it ([MS-RDPECAM] 2.2.2.1, as FreeRDP's client does).
    CHECK(cam::encode_client_pdu(cam::SelectVersionRequest{cam::version1}) == bytes_of({0x01, 0x03}));
    const auto asked = cam::decode_client_pdu(bytes_of({0x01, 0x03}));
    REQUIRE(asked.has_value());
    CHECK(std::get<cam::SelectVersionRequest>(*asked).version == cam::version1);
    CHECK(cam::encode_server_pdu(cam::SelectVersionResponse{cam::version1}) == bytes_of({0x01, 0x04}));
    const auto answered = cam::decode_server_pdu(bytes_of({0x01, 0x04}));
    REQUIRE(answered.has_value());
    CHECK(std::get<cam::SelectVersionResponse>(*answered).version == cam::version1);
}

TEST_CASE("A device notification carries a UTF-16 name and a channel name", "[channels][rdpecam]")
{
    // [MS-RDPECAM] 2.2.2.3: version, id, the name in UTF-16LE with its
    // terminator, then the channel name in bytes with its terminator.
    const auto encoded = cam::encode_client_pdu(cam::DeviceAdded{"Cam", "rdpecam0"});
    const auto expected =
        bytes_of({0x02, 0x05, 'C', 0, 'a', 0, 'm', 0, 0, 0, 'r', 'd', 'p', 'e', 'c', 'a', 'm', '0', 0});
    CHECK(encoded == expected);

    const auto decoded = cam::decode_client_pdu(encoded);
    REQUIRE(decoded.has_value());
    const auto& added = std::get<cam::DeviceAdded>(*decoded);
    CHECK(added.device_name == "Cam");
    CHECK(added.channel_name == "rdpecam0");

    // An unterminated name, and one that does not name a channel, are errors.
    CHECK(error_code(cam::decode_client_pdu(bytes_of({0x02, 0x05, 'C', 0}))) == Errc::truncated);
    CHECK(error_code(cam::decode_client_pdu(bytes_of({0x02, 0x05, 0, 0}))) == Errc::truncated);
    CHECK(error_code(cam::decode_client_pdu(bytes_of({0x02, 0x05, 0, 0, 0}))) == Errc::invalid_value);
}

TEST_CASE("A media type description is 26 bytes in the order FreeRDP reads", "[channels][rdpecam]")
{
    const auto start = cam::encode_server_pdu(cam::StartStreamsRequest{1, nv12_720p()});
    // version, id, stream index, then the 26-byte description.
    REQUIRE(start.size() == 3 + cam::media_type_size);
    const auto expected = bytes_of({0x02, 0x0F, 0x01,        // header and stream index
                                    0x04,                    // NV12
                                    0x00, 0x05, 0x00, 0x00,  // width 1280
                                    0xD0, 0x02, 0x00, 0x00,  // height 720
                                    0x1E, 0x00, 0x00, 0x00,  // frame rate 30
                                    0x01, 0x00, 0x00, 0x00,  // / 1
                                    0x01, 0x00, 0x00, 0x00,  // aspect 1
                                    0x01, 0x00, 0x00, 0x00,  // / 1
                                    0x00});                  // flags
    CHECK(start == expected);

    const auto decoded = cam::decode_server_pdu(start);
    REQUIRE(decoded.has_value());
    const auto& request = std::get<cam::StartStreamsRequest>(*decoded);
    CHECK(request.stream_index == 1);
    CHECK(request.media_type == nv12_720p());
    CHECK(request.media_type.fps() == 30);
}

TEST_CASE("Media type and stream lists take as many entries as fill the PDU", "[channels][rdpecam]")
{
    cam::MediaTypeListResponse list;
    list.media_types.push_back(nv12_720p());
    auto second = nv12_720p();
    second.width = 640;
    second.height = 480;
    list.media_types.push_back(second);
    const auto encoded = cam::encode_client_pdu(list);
    REQUIRE(encoded.size() == 2 + (2 * cam::media_type_size));
    const auto decoded = cam::decode_client_pdu(encoded);
    REQUIRE(decoded.has_value());
    CHECK(std::get<cam::MediaTypeListResponse>(*decoded) == list);

    cam::StreamListResponse streams;
    streams.streams.push_back({cam::frame_source::color, cam::stream_category_capture, true, false});
    streams.streams.push_back({cam::frame_source::infrared, cam::stream_category_capture, false, true});
    const auto stream_bytes = cam::encode_client_pdu(streams);
    REQUIRE(stream_bytes.size() == 2 + (2 * 5));
    const auto stream_decoded = cam::decode_client_pdu(stream_bytes);
    REQUIRE(stream_decoded.has_value());
    CHECK(std::get<cam::StreamListResponse>(*stream_decoded) == streams);

    // A partial description is an error, not a silently dropped entry.
    Bytes truncated(encoded.begin(), encoded.end() - 1);
    CHECK(error_code(cam::decode_client_pdu(truncated)) == Errc::invalid_length);
    Bytes short_stream(stream_bytes.begin(), stream_bytes.end() - 1);
    CHECK(error_code(cam::decode_client_pdu(short_stream)) == Errc::invalid_length);
}

TEST_CASE("A media type with a zero rate or aspect ratio is refused", "[channels][rdpecam]")
{
    for (const std::size_t field : {12U, 16U, 20U, 24U}) {  // the four 32-bit ratio fields
        auto start = cam::encode_server_pdu(cam::StartStreamsRequest{0, nv12_720p()});
        for (std::size_t i = 0; i < 4; ++i) {
            start[field + i] = std::byte{0};
        }
        INFO("zero at offset " << field);
        CHECK(error_code(cam::decode_server_pdu(start)) == Errc::invalid_value);
    }
    // An unknown format is refused too.
    auto start = cam::encode_server_pdu(cam::StartStreamsRequest{0, nv12_720p()});
    start[3] = std::byte{0x42};
    CHECK(error_code(cam::decode_server_pdu(start)) == Errc::unsupported);
}

TEST_CASE("A sample response points into its message", "[channels][rdpecam]")
{
    const auto frame = bytes_of({1, 2, 3, 4, 5});
    const auto encoded = cam::encode_client_pdu(cam::SampleResponse{2, frame});
    CHECK(encoded == bytes_of({0x02, 0x12, 0x02, 1, 2, 3, 4, 5}));
    const auto decoded = cam::decode_client_pdu(encoded);
    REQUIRE(decoded.has_value());
    const auto& sample = std::get<cam::SampleResponse>(*decoded);
    CHECK(sample.stream_index == 2);
    CHECK(std::ranges::equal(sample.sample, frame));
    // An empty sample is still a well-formed PDU; the server decides.
    const auto empty = cam::decode_client_pdu(bytes_of({0x02, 0x12, 0x00}));
    REQUIRE(empty.has_value());
    CHECK(std::get<cam::SampleResponse>(*empty).sample.empty());
}

TEST_CASE("Camera properties round-trip", "[channels][rdpecam]")
{
    cam::PropertyListResponse list;
    list.properties.push_back({cam::property_set::camera_control, 0x06, 0x03, -10, 10, 1, 0});
    const auto encoded = cam::encode_client_pdu(list);
    REQUIRE(encoded.size() == 2 + cam::property_description_size);
    const auto decoded = cam::decode_client_pdu(encoded);
    REQUIRE(decoded.has_value());
    CHECK(std::get<cam::PropertyListResponse>(*decoded) == list);

    const cam::SetPropertyValueRequest set{cam::property_set::video_proc_amp, 0x02, {cam::property_mode::manual, -5}};
    const auto set_decoded = cam::decode_server_pdu(cam::encode_server_pdu(set));
    REQUIRE(set_decoded.has_value());
    CHECK(std::get<cam::SetPropertyValueRequest>(*set_decoded) == set);
}

TEST_CASE("Camera decoding is strict about direction and length", "[channels][rdpecam]")
{
    // A PDU only the server sends is not a client PDU, and the other way round.
    CHECK(error_code(cam::decode_client_pdu(bytes_of({0x02, 0x09}))) == Errc::unsupported);
    CHECK(error_code(cam::decode_server_pdu(bytes_of({0x02, 0x0A}))) == Errc::unsupported);
    // An unknown message id.
    CHECK(error_code(cam::decode_client_pdu(bytes_of({0x02, 0x7F}))) == Errc::unsupported);
    // Trailing bytes where the PDU is fixed.
    CHECK(error_code(cam::decode_server_pdu(bytes_of({0x02, 0x11, 0x00, 0x00}))) == Errc::trailing_data);
    CHECK(error_code(cam::decode_client_pdu(bytes_of({0x02, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00}))) ==
          Errc::trailing_data);
    // Too short for the header.
    CHECK(error_code(cam::decode_client_pdu(bytes_of({0x02}))) == Errc::truncated);
    CHECK(error_code(cam::decode_client_pdu({})) == Errc::truncated);
}

TEST_CASE("Uncompressed frame sizes follow their layout", "[channels][rdpecam]")
{
    CHECK(cam::frame_size(cam::MediaFormat::yuy2, 640, 480) == 640 * 480 * 2);
    CHECK(cam::frame_size(cam::MediaFormat::nv12, 640, 480) == (640 * 480 * 3) / 2);
    CHECK(cam::frame_size(cam::MediaFormat::i420, 640, 480) == (640 * 480 * 3) / 2);
    CHECK(cam::frame_size(cam::MediaFormat::rgb24, 640, 480) == 640 * 480 * 3);
    CHECK(cam::frame_size(cam::MediaFormat::rgb32, 640, 480) == 640 * 480 * 4);
    // Odd sizes round the chroma planes up, as every 4:2:0 layout does.
    CHECK(cam::frame_size(cam::MediaFormat::nv12, 3, 3) == 9 + (2 * 2 * 2));
    // Compressed formats have no fixed frame size.
    CHECK(cam::frame_size(cam::MediaFormat::h264, 640, 480) == 0);
    CHECK(cam::frame_size(cam::MediaFormat::mjpg, 640, 480) == 0);
    CHECK(cam::needs_decoding(cam::MediaFormat::h264));
    CHECK(cam::needs_decoding(cam::MediaFormat::mjpg));
    CHECK_FALSE(cam::needs_decoding(cam::MediaFormat::nv12));
}
