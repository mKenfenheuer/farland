// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/proto/save_session_info.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using namespace farland;
using namespace farland::proto;

namespace {

ServerAutoReconnectCookie sample_cookie()
{
    ServerAutoReconnectCookie cookie;
    cookie.logon_id = 2;
    for (std::size_t i = 0; i < cookie.random_bits.size(); ++i) {
        cookie.random_bits.at(i) = static_cast<std::byte>(0x10 + i);
    }
    return cookie;
}

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values)
{
    std::vector<std::byte> out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

Result<LogonInfoExtended> decode_all(std::span<const std::byte> bytes)
{
    Reader r(bytes);
    auto info = decode_save_session_info(r);
    if (info) {
        FARLAND_TRY_VOID(r.expect_end("Save Session Info"));
    }
    return info;
}

}  // namespace

TEST_CASE("Save Session Info carries the auto-reconnect cookie, [MS-RDPBCGR] 2.2.10.1.1.4 and 2.2.4.2")
{
    Writer w;
    encode_save_session_info(w, LogonInfoExtended{sample_cookie(), std::nullopt});
    const auto bytes = w.view();
    REQUIRE(bytes.size() == 4 + 2 + 4 + 4 + 28 + 570);

    const auto expected = bytes_of({
        0x03, 0x00, 0x00, 0x00,  // infoType INFOTYPE_LOGON_EXTENDED_INFO
        0x60, 0x02,              // Length 608: the whole structure, pad included
        0x01, 0x00, 0x00, 0x00,  // FieldsPresent LOGON_EX_AUTORECONNECTCOOKIE
        0x1C, 0x00, 0x00, 0x00,  // cbFieldData
        0x1C, 0x00, 0x00, 0x00,  // ARC_SC_PRIVATE_PACKET cbLen
        0x01, 0x00, 0x00, 0x00,  // Version AUTO_RECONNECT_VERSION_1
        0x02, 0x00, 0x00, 0x00,  // LogonId
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
    });
    CHECK(std::ranges::equal(bytes.first(expected.size()), expected));
    CHECK(std::ranges::all_of(bytes.subspan(expected.size()), [](std::byte b) { return b == std::byte{0}; }));

    const auto info = decode_all(bytes).value();
    REQUIRE(info.auto_reconnect_cookie.has_value());
    CHECK(info.auto_reconnect_cookie->version == auto_reconnect_version_1);
    CHECK(info.auto_reconnect_cookie->logon_id == 2);
    CHECK(info.auto_reconnect_cookie->random_bits == sample_cookie().random_bits);
    CHECK_FALSE(info.logon_errors.has_value());
}

TEST_CASE("Save Session Info with logon errors round-trips, [MS-RDPBCGR] 2.2.10.1.1.4.1.1")
{
    Writer w;
    encode_save_session_info(w, LogonInfoExtended{sample_cookie(), LogonErrorsInfo{0xFFFFFFFE, 7}});
    CHECK(w.size() == 4 + 2 + 4 + 32 + 12 + 570);
    const auto info = decode_all(w.view()).value();
    REQUIRE(info.logon_errors.has_value());
    CHECK(info.logon_errors->error_notification_type == 0xFFFFFFFE);
    CHECK(info.logon_errors->error_notification_data == 7);
    CHECK(info.auto_reconnect_cookie.has_value());

    Writer empty;
    encode_save_session_info(empty, LogonInfoExtended{});
    CHECK(empty.size() == 4 + 2 + 4 + 570);
    const auto none = decode_all(empty.view()).value();
    CHECK_FALSE(none.auto_reconnect_cookie.has_value());
}

TEST_CASE("Save Session Info decodes whichever way the sender counted Length")
{
    // The text of 2.2.10.1.1.4 leaves open whether Length counts the pad, so
    // the decoder only requires Length to cover its own two fields and not to
    // run past the data (as FreeRDP's rdp_recv_logon_info_extended does).
    Writer w;
    encode_save_session_info(w, LogonInfoExtended{sample_cookie(), std::nullopt});
    auto bytes = std::vector<std::byte>(w.view().begin(), w.view().end());
    // FreeRDP's rdp_write_logon_info_ex: 2 + 4 + 570 + 28 = 604.
    bytes[4] = std::byte{0x5C};
    bytes[5] = std::byte{0x02};
    CHECK(decode_all(bytes).has_value());
    // Without the pad: 2 + 4 + 4 + 28 = 38.
    bytes[4] = std::byte{38};
    bytes[5] = std::byte{0x00};
    CHECK(decode_all(bytes).has_value());
    // Below Length and FieldsPresent themselves.
    bytes[4] = std::byte{5};
    const auto too_short = decode_all(bytes);
    REQUIRE_FALSE(too_short.has_value());
    CHECK(too_short.error().code == Errc::invalid_length);
}

TEST_CASE("Malformed Save Session Info is rejected")
{
    Writer w;
    encode_save_session_info(w, LogonInfoExtended{sample_cookie(), std::nullopt});
    const std::vector<std::byte> good(w.view().begin(), w.view().end());
    const auto check = [&good](std::size_t offset, std::byte value, Errc code) {
        auto bytes = good;
        bytes.at(offset) = value;
        const auto info = decode_all(bytes);
        REQUIRE_FALSE(info.has_value());
        CHECK(info.error().code == code);
    };
    check(0, std::byte{0x02}, Errc::unsupported);      // INFOTYPE_LOGON_PLAINNOTIFY
    check(6, std::byte{0x05}, Errc::unsupported);      // an unknown FieldsPresent bit
    check(10, std::byte{0x1B}, Errc::invalid_length);  // cbFieldData 27
    check(14, std::byte{0x1B}, Errc::invalid_length);  // cbLen 27
    check(5, std::byte{0x03}, Errc::invalid_length);   // Length beyond the data

    // One byte short: Length (608) now runs past the data.
    const auto short_data = decode_all(std::span(good).first(good.size() - 1));
    REQUIRE_FALSE(short_data.has_value());
    CHECK(short_data.error().code == Errc::invalid_length);
    // A Length without the pad passes that check, but the pad is still required.
    auto no_pad_length = good;
    no_pad_length[4] = std::byte{38};
    no_pad_length[5] = std::byte{0x00};
    const auto truncated = decode_all(std::span(no_pad_length).first(good.size() - 1));
    REQUIRE_FALSE(truncated.has_value());
    CHECK(truncated.error().code == Errc::truncated);
}
