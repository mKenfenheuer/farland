// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/text.hpp>
#include <farland/channels/clipboard_formats.hpp>
#include <farland/channels/cliprdr.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using farland::Errc;
using farland::test::hex;
using namespace farland::channels::cliprdr;

namespace {

using Bytes = std::vector<std::byte>;

template <class T>
T decode_as(const Bytes& bytes, bool long_names = true)
{
    auto pdu = decode(bytes, long_names);
    if (!pdu) {
        FAIL(pdu.error().message());
    }
    REQUIRE(std::holds_alternative<T>(*pdu));
    return std::get<T>(*pdu);
}

Errc error_of(const Bytes& bytes, bool long_names = true)
{
    auto pdu = decode(bytes, long_names);
    REQUIRE_FALSE(pdu.has_value());
    return pdu.error().code;
}

Bytes concat(Bytes a, const Bytes& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

Bytes utf16_field(std::string_view text, std::size_t size)
{
    auto bytes = farland::utf8_to_utf16le(text);
    bytes.resize(size);
    return bytes;
}

}  // namespace

TEST_CASE("Clipboard capabilities ([MS-RDPECLIP] 4.1.1, 4.1.3)")
{
    const auto bytes = hex("07 00 00 00 10 00 00 00 01 00 00 00 01 00 0c 00 02 00 00 00 0e 00 00 00");
    const auto caps = decode_as<Capabilities>(bytes);
    CHECK(caps.version == 2);
    CHECK(caps.general_flags == (general_flag::use_long_format_names | general_flag::stream_fileclip_enabled |
                                 general_flag::fileclip_no_file_paths));
    CHECK(encode(caps) == bytes);

    SECTION("unknown sets are skipped, without a general set the defaults apply")
    {
        const auto other = decode_as<Capabilities>(hex("07 00 00 00 0a 00 00 00 01 00 00 00 09 00 06 00 aa bb"));
        CHECK_FALSE(other.general_flags.has_value());
        CHECK(encode(other) == hex("07 00 00 00 04 00 00 00 00 00 00 00"));
    }
    SECTION("malformed sets")
    {
        CHECK(error_of(hex("07 00 00 00 08 00 00 00 01 00 00 00 01 00 02 00")) == Errc::invalid_length);
        CHECK(error_of(hex("07 00 00 00 0c 00 00 00 01 00 00 00 01 00 08 00 02 00 00 00")) == Errc::invalid_length);
        CHECK(error_of(hex("07 00 00 00 1c 00 00 00 02 00 00 00 01 00 0c 00 02 00 00 00 02 00 00 00"
                           "01 00 0c 00 02 00 00 00 02 00 00 00")) == Errc::invalid_value);
        CHECK(error_of(hex("07 00 00 00 04 00 00 00 11 00 00 00")) == Errc::limit_exceeded);
        CHECK(error_of(hex("07 00 00 00 10 00 00 00 01 00 00 00 01 00 0c 00 02 00 00 00 0e 00 00")) ==
              Errc::invalid_length);
    }
}

TEST_CASE("Monitor Ready and Temporary Directory ([MS-RDPECLIP] 4.1.2, 4.1.4)")
{
    CHECK(encode(MonitorReady{}) == hex("01 00 00 00 00 00 00 00"));
    decode_as<MonitorReady>(hex("01 00 00 00 00 00 00 00"));
    CHECK(error_of(hex("01 00 00 00 01 00 00 00 00")) == Errc::trailing_data);

    const std::string path = R"(C:\DOCUME~1\ELTONS~1.NTD\LOCALS~1\Temp\cdepotslhrdp_1\_TSABD.tmp)";
    const auto bytes = concat(hex("06 00 00 00 08 02 00 00"), utf16_field(path, 520));
    CHECK(decode_as<TempDirectory>(bytes).path == path);
    CHECK(encode(TempDirectory{path}) == bytes);
    CHECK(error_of(hex("06 00 00 00 02 00 00 00 41 00")) == Errc::invalid_length);
}

TEST_CASE("Format List with long names ([MS-RDPECLIP] 4.1.5, 4.2.1, 4.5.1)")
{
    const auto initial = hex("02 00 00 00 24 00 00 00 04 c0 00 00 4e 00 61 00 74 00 69 00 76 00 65 00 00 00 03 00"
                             "00 00 00 00 08 00 00 00 00 00 11 00 00 00 00 00");
    const auto list = decode_as<FormatList>(initial);
    CHECK(list.formats == std::vector<Format>{{0xC004, "Native"}, {3, ""}, {8, ""}, {17, ""}});
    CHECK(encode(list) == initial);

    const auto copy = hex("02 00 00 00 e0 00 00 00 8a c0 00 00 52 00 69 00 63 00 68 00 20 00 54 00 65 00 78 00 74 00"
                          "20 00 46 00 6f 00 72 00 6d 00 61 00 74 00 00 00 45 c1 00 00 52 00 69 00 63 00 68 00 20 00"
                          "54 00 65 00 78 00 74 00 20 00 46 00 6f 00 72 00 6d 00 61 00 74 00 20 00 57 00 69 00 74 00"
                          "68 00 6f 00 75 00 74 00 20 00 4f 00 62 00 6a 00 65 00 63 00 74 00 73 00 00 00 43 c1 00 00"
                          "52 00 54 00 46 00 20 00 41 00 73 00 20 00 54 00 65 00 78 00 74 00 00 00 01 00 00 00 00 00"
                          "0d 00 00 00 00 00 04 c0 00 00 4e 00 61 00 74 00 69 00 76 00 65 00 00 00 0e c0 00 00 4f 00"
                          "62 00 6a 00 65 00 63 00 74 00 20 00 44 00 65 00 73 00 63 00 72 00 69 00 70 00 74 00 6f 00"
                          "72 00 00 00 03 00 00 00 00 00 10 00 00 00 00 00 07 00 00 00 00 00");
    const auto rich = decode_as<FormatList>(copy);
    CHECK(rich.formats == std::vector<Format>{{0xC08A, "Rich Text Format"},
                                              {0xC145, "Rich Text Format Without Objects"},
                                              {0xC143, "RTF As Text"},
                                              {1, ""},
                                              {13, ""},
                                              {0xC004, "Native"},
                                              {0xC00E, "Object Descriptor"},
                                              {3, ""},
                                              {16, ""},
                                              {7, ""}});
    CHECK(encode(rich) == copy);

    const auto files = hex("02 00 00 00 2e 00 00 00 79 c0 00 00 46 00 69 00 6c 00 65 00 47 00 72 00 6f 00 75 00 70 00"
                           "44 00 65 00 73 00 63 00 72 00 69 00 70 00 74 00 6f 00 72 00 57 00 00 00");
    CHECK(decode_as<FormatList>(files).formats == std::vector<Format>{{0xC079, file_group_descriptor_w}});

    CHECK(decode_as<FormatList>(hex("02 00 00 00 00 00 00 00")).formats.empty());
    CHECK(error_of(hex("02 00 00 00 06 00 00 00 01 00 00 00 41 00")) == Errc::truncated);
    CHECK(error_of(hex("02 00 00 00 03 00 00 00 01 00 00")) == Errc::truncated);

    SECTION("limits")
    {
        FormatList many;
        many.formats.resize(max_formats + 1, Format{13, ""});
        CHECK(error_of(encode(many)) == Errc::limit_exceeded);
        many.formats.resize(max_formats);
        CHECK(decode_as<FormatList>(encode(many)).formats.size() == max_formats);
        const FormatList long_name{{{0xC000, std::string(max_format_name_length + 1, 'x')}}};
        CHECK(error_of(encode(long_name)) == Errc::limit_exceeded);
    }
}

TEST_CASE("Format List with short names ([MS-RDPECLIP] 2.2.3.1.1)")
{
    const FormatList list{{{13, ""}, {0xC001, "FileGroupDescriptorW"}, {0xC002, "HTML Format"}}};
    const auto bytes = encode(list, false);
    CHECK(bytes.size() == header_size + (3 * 36));
    const auto decoded = decode_as<FormatList>(bytes, false);
    // 15 UTF-16 characters and the terminator fit.
    CHECK(decoded.formats == std::vector<Format>{{13, ""}, {0xC001, "FileGroupDescri"}, {0xC002, "HTML Format"}});

    // CB_ASCII_NAMES: 31 characters, Latin-1.
    auto ascii = concat(hex("02 00 04 00 24 00 00 00 05 c0 00 00"), Bytes(32));
    const std::string name = "Rich Text Format\xE9";
    for (std::size_t i = 0; i < name.size(); ++i) {
        ascii[12 + i] = static_cast<std::byte>(name[i]);
    }
    CHECK(decode_as<FormatList>(ascii, false).formats == std::vector<Format>{{0xC005, "Rich Text Format\xC3\xA9"}});

    CHECK(error_of(hex("02 00 00 00 04 00 00 00 0d 00 00 00"), false) == Errc::invalid_length);
    CHECK(decode_as<FormatList>(hex("02 00 00 00 00 00 00 00"), false).formats.empty());
    // A long-name list read as short names does not fill 36-byte records.
    CHECK(error_of(encode(FormatList{{{13, ""}}}, true), false) == Errc::invalid_length);
}

TEST_CASE("Format List Response, Lock and Unlock ([MS-RDPECLIP] 4.1.6, 4.3)")
{
    CHECK(decode_as<FormatListResponse>(hex("03 00 01 00 00 00 00 00")).ok);
    CHECK_FALSE(decode_as<FormatListResponse>(hex("03 00 02 00 00 00 00 00")).ok);
    CHECK(encode(FormatListResponse{true}) == hex("03 00 01 00 00 00 00 00"));
    CHECK(encode(FormatListResponse{false}) == hex("03 00 02 00 00 00 00 00"));
    CHECK(error_of(hex("03 00 00 00 00 00 00 00")) == Errc::invalid_value);
    CHECK(error_of(hex("03 00 03 00 00 00 00 00")) == Errc::invalid_value);

    const auto lock = hex("0a 00 00 00 04 00 00 00 08 00 00 00");
    CHECK(decode_as<LockClipData>(lock).clip_data_id == 8);
    CHECK(encode(LockClipData{8}) == lock);
    const auto unlock = hex("0b 00 00 00 04 00 00 00 08 00 00 00");
    CHECK(decode_as<UnlockClipData>(unlock).clip_data_id == 8);
    CHECK(encode(UnlockClipData{8}) == unlock);
    CHECK(error_of(hex("0a 00 00 00 02 00 00 00 08 00")) == Errc::invalid_length);
}

TEST_CASE("Format Data Request and Response ([MS-RDPECLIP] 4.4.1, 4.4.2)")
{
    const auto request = hex("04 00 00 00 04 00 00 00 0d 00 00 00");
    CHECK(decode_as<FormatDataRequest>(request).format_id == 13);
    CHECK(encode(FormatDataRequest{13}) == request);

    const auto response = hex("05 00 01 00 18 00 00 00 68 00 65 00 6c 00 6c 00 6f 00 20 00 77 00 6f 00 72 00 6c 00"
                              "64 00 00 00");
    const auto data = decode_as<FormatDataResponse>(response);
    CHECK(data.ok);
    CHECK(farland::channels::clipboard::unicode_text_to_utf8(data.data) == "hello world");
    CHECK(encode(data) == response);

    CHECK(encode(FormatDataResponse{false, {}}) == hex("05 00 02 00 00 00 00 00"));
    CHECK_FALSE(decode_as<FormatDataResponse>(hex("05 00 02 00 00 00 00 00")).ok);
}

TEST_CASE("File Contents Request and Response ([MS-RDPECLIP] 4.4.3, 4.4.4)")
{
    const auto size_request = hex("08 00 00 00 18 00 00 00 02 00 00 00 01 00 00 00 01 00 00 00 00 00 00 00 00 00 00 00"
                                  "08 00 00 00");
    const auto size = decode_as<FileContentsRequest>(size_request);
    CHECK(size == FileContentsRequest{2, 1, file_contents::size, 0, 8, std::nullopt});
    CHECK(encode(size) == size_request);

    const auto range_request = hex("08 00 00 00 18 00 00 00 02 00 00 00 01 00 00 00 02 00 00 00 00 00 00 00 00 00 00"
                                   "00 00 00 01 00");
    const auto range = decode_as<FileContentsRequest>(range_request);
    CHECK(range == FileContentsRequest{2, 1, file_contents::range, 0, 65536, std::nullopt});
    CHECK(encode(range) == range_request);

    const FileContentsRequest huge{7, -1, file_contents::range, 0x123456789ULL, 4096, 42U};
    CHECK(decode_as<FileContentsRequest>(encode(huge)) == huge);
    CHECK(encode(huge).size() == header_size + 28);

    CHECK(error_of(hex("08 00 00 00 18 00 00 00 02 00 00 00 01 00 00 00 03 00 00 00 00 00 00 00 00 00 00 00 08 00 00"
                       "00")) == Errc::invalid_value);
    CHECK(error_of(hex("08 00 00 00 18 00 00 00 02 00 00 00 01 00 00 00 01 00 00 00 00 00 00 00 00 00 00 00 10 00 00"
                       "00")) == Errc::invalid_value);
    CHECK(error_of(hex("08 00 00 00 14 00 00 00 02 00 00 00 01 00 00 00 01 00 00 00 00 00 00 00 00 00 00 00")) ==
          Errc::invalid_length);

    const auto size_response = hex("09 00 01 00 0c 00 00 00 02 00 00 00 2c 00 00 00 00 00 00 00");
    const auto sized = decode_as<FileContentsResponse>(size_response);
    CHECK(sized.ok);
    CHECK(sized.stream_id == 2);
    CHECK(sized.data == hex("2c 00 00 00 00 00 00 00"));
    CHECK(encode(sized) == size_response);

    const std::string text = "The quick brown fox jumps over the lazy dog.";
    const auto contents = concat(hex("09 00 01 00 30 00 00 00 02 00 00 00"),
                                 Bytes(farland::test::ascii(text).begin(), farland::test::ascii(text).end()));
    CHECK(decode_as<FileContentsResponse>(contents).data.size() == text.size());
    CHECK(encode(decode_as<FileContentsResponse>(contents)) == contents);

    // Failures: Windows keeps the streamId, some clients send nothing.
    CHECK(decode_as<FileContentsResponse>(hex("09 00 02 00 04 00 00 00 05 00 00 00")) ==
          FileContentsResponse{false, 5, {}});
    CHECK(decode_as<FileContentsResponse>(hex("09 00 02 00 00 00 00 00")) == FileContentsResponse{false, 0, {}});
    CHECK(error_of(hex("09 00 01 00 02 00 00 00 05 00")) == Errc::truncated);
}

TEST_CASE("Clipboard PDU framing ([MS-RDPECLIP] 2.2.1, 3.1.5.1)")
{
    // Windows Server 2003 to 2012 R2 append four bytes that dataLen leaves out.
    decode_as<MonitorReady>(hex("01 00 00 00 00 00 00 00 00 00 00 00"));
    CHECK(decode_as<FormatDataRequest>(hex("04 00 00 00 04 00 00 00 0d 00 00 00 aa bb cc dd")).format_id == 13);
    CHECK(error_of(hex("01 00 00 00 00 00 00 00 00 00")) == Errc::invalid_length);
    CHECK(error_of(hex("04 00 00 00 08 00 00 00 0d 00 00 00")) == Errc::invalid_length);
    CHECK(error_of(hex("01 00 00 00 00 00")) == Errc::truncated);
    CHECK(error_of(hex("0c 00 00 00 00 00 00 00")) == Errc::unsupported);
}

TEST_CASE("Packed File List ([MS-RDPECLIP] 4.5.4)")
{
    const std::vector<FileDescriptor> files{
        {0x4064, file_attribute::archive, 0x01CA55F32C305D08ULL, 44, "File1.txt"},
        {0x4064, file_attribute::archive, 0x01CA55F32C305D08ULL, 10, "File2.txt"},
    };
    const auto bytes = encode_file_list(files);
    REQUIRE(bytes.size() == 0x4A4);
    const auto first = hex("02 00 00 00 64 40 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
                           "00 00 00 00 00 00 00 00 00 00 20 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
                           "08 5d 30 2c f3 55 ca 01 00 00 00 00 2c 00 00 00 46 00 69 00 6c 00 65 00 31 00 2e 00 74 00"
                           "78 00 74 00 00 00");
    CHECK(Bytes(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(first.size())) == first);
    const auto second = hex("64 40 00 00");
    CHECK(Bytes(bytes.begin() + 0x254, bytes.begin() + 0x258) == second);
    const auto decoded = decode_file_list(bytes);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == files);

    const FileDescriptor big{fd_flag::file_size, 0, 0, 0x100000002ULL, R"(dir\sub\file.bin)"};
    CHECK(decode_file_list(encode_file_list(std::span(&big, 1))).value().front() == big);

    auto truncated = bytes;
    truncated.pop_back();
    CHECK(decode_file_list(truncated).error().code == Errc::invalid_length);
    CHECK(decode_file_list(hex("ff ff 00 00")).error().code == Errc::limit_exceeded);
    CHECK(decode_file_list(hex("00 00 00 00")).value().empty());
}
