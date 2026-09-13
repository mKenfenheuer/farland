// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/hexdump.hpp>
#include <farland/proto/capabilities.hpp>
#include <farland/proto/client_info.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

namespace caps = farland::proto::caps;
namespace proto = farland::proto;
using farland::Errc;
using farland::Reader;
using farland::to_hex;
using farland::Writer;
using farland::test::hex;

TEST_CASE("Capability sets round-trip ([MS-RDPBCGR] 2.2.7)")
{
    caps::CapabilitySets sets;
    sets.general = caps::General{caps::os_major::unix,
                                 0,
                                 0x0200,
                                 0,
                                 caps::general_extra_flags::fastpath_output_supported |
                                     caps::general_extra_flags::no_bitmap_compression_hdr,
                                 0,
                                 0,
                                 0,
                                 1,
                                 1};
    sets.bitmap = caps::Bitmap{};
    sets.bitmap->desktop_width = 1920;
    sets.bitmap->desktop_height = 1080;
    sets.order = caps::Order{};
    sets.pointer = caps::Pointer{};
    sets.input = caps::Input{caps::input_flags::scancodes | caps::input_flags::fastpath_input2, 0x407, 4, 0, 12, "ime"};
    sets.virtual_channel = caps::VirtualChannel{0, 1600};
    sets.share = caps::Share{0x03ea};
    sets.font = caps::Font{};
    sets.color_cache = caps::ColorCache{};
    sets.multifragment_update = caps::MultifragmentUpdate{0x7FFF00};
    sets.large_pointer = caps::LargePointer{3};
    sets.surface_commands = caps::SurfaceCommands{0x52};
    sets.frame_acknowledge = caps::FrameAcknowledge{2};
    const auto raw_body = hex("01 02 03 04");
    sets.other.push_back(caps::RawCapabilitySet{caps::type::brush, raw_body});

    Writer w;
    const auto count = caps::encode_capability_sets(w, sets);
    CHECK(count == 14);
    Reader r(w.view());
    const auto decoded = caps::decode_capability_sets(r, count).value();
    CHECK(r.empty());
    CHECK(decoded.general->extra_flags == sets.general->extra_flags);
    CHECK(decoded.general->refresh_rect_support == 1);
    CHECK(decoded.bitmap->desktop_width == 1920);
    CHECK(decoded.order->desktop_save_size == 480 * 480);
    CHECK(decoded.pointer->pointer_cache_size == 25);
    CHECK(decoded.input->keyboard_layout == 0x407);
    CHECK(decoded.input->ime_file_name == "ime");
    CHECK(decoded.virtual_channel->chunk_size == 1600);
    CHECK(decoded.share->node_id == 0x03ea);
    CHECK(decoded.font->support_flags == 1);
    CHECK(decoded.color_cache->cache_size == 6);
    CHECK(decoded.multifragment_update->max_request_size == 0x7FFF00);
    CHECK(decoded.large_pointer->support_flags == 3);
    CHECK(decoded.surface_commands->cmd_flags == 0x52);
    CHECK(decoded.frame_acknowledge->max_unacknowledged_frame_count == 2);
    REQUIRE(decoded.other.size() == 1);
    CHECK(decoded.other[0].type == caps::type::brush);
    CHECK(to_hex(decoded.other[0].body) == "01 02 03 04");
}

TEST_CASE("Fixed capability set sizes match the specification")
{
    const auto size_of = [](auto set, auto member) {
        caps::CapabilitySets sets;
        sets.*member = set;
        Writer w;
        caps::encode_capability_sets(w, sets);
        return w.size();
    };
    CHECK(size_of(caps::General{}, &caps::CapabilitySets::general) == 24);
    CHECK(size_of(caps::Bitmap{}, &caps::CapabilitySets::bitmap) == 28);
    CHECK(size_of(caps::Order{}, &caps::CapabilitySets::order) == 88);
    CHECK(size_of(caps::Pointer{}, &caps::CapabilitySets::pointer) == 10);
    CHECK(size_of(caps::Input{}, &caps::CapabilitySets::input) == 88);
    CHECK(size_of(caps::Share{}, &caps::CapabilitySets::share) == 8);
    CHECK(size_of(caps::Font{}, &caps::CapabilitySets::font) == 8);
    CHECK(size_of(caps::MultifragmentUpdate{}, &caps::CapabilitySets::multifragment_update) == 8);
    CHECK(size_of(caps::LargePointer{}, &caps::CapabilitySets::large_pointer) == 6);
    CHECK(size_of(caps::SurfaceCommands{}, &caps::CapabilitySets::surface_commands) == 12);
}

TEST_CASE("Short or truncated capability sets are rejected; the first duplicate wins")
{
    const auto check = [](std::string_view text, std::uint16_t count, Errc expected) {
        const auto bytes = hex(text);
        Reader r(bytes);
        const auto sets = caps::decode_capability_sets(r, count);
        REQUIRE_FALSE(sets.has_value());
        CHECK(sets.error().code == expected);
    };
    check("09 00 02 00", 1, Errc::invalid_length);         // length below the header
    check("09 00 08 00 ea 03", 1, Errc::truncated);        // length beyond the data
    check("1a 00 06 00 00 00", 1, Errc::truncated);        // MultifragmentUpdate without its u32
    check("09 00 08 00 ea 03 00 00", 2, Errc::truncated);  // count says two sets

    const auto dup = hex("09 00 08 00 ea 03 00 00 09 00 08 00 01 00 00 00");
    Reader r(dup);
    CHECK(caps::decode_capability_sets(r, 2).value().share->node_id == 0x03ea);
}

TEST_CASE("Client Info round-trips with extended info and an auto-reconnect cookie")
{
    proto::ClientInfo info;
    info.flags = proto::info_flags::unicode | proto::info_flags::autologon | proto::info_flags::logon_errors;
    info.domain = "CORP";
    info.user_name = "jürgen";
    info.password = farland::SecretString("s3cr3t");
    info.working_dir = "C:\\";
    proto::ExtendedInfo ext;
    ext.client_address = "192.0.2.10";
    ext.client_dir = "C:\\Windows\\System32\\mstscax.dll";
    ext.time_zone =
        proto::TimeZoneInformation{-60, "W. Europe Standard Time", {}, 0, "W. Europe Daylight Time", {}, -60};
    ext.session_id = 7;
    ext.performance_flags = 0x80;
    ext.auto_reconnect_cookie = proto::AutoReconnectCookie{1, 42, {}};
    ext.dynamic_dst_time_zone_key_name = "W. Europe Standard Time";
    ext.dynamic_daylight_time_disabled = false;
    info.extended = ext;

    Writer w;
    proto::encode_client_info(w, info);
    Reader r(w.view());
    const auto decoded = proto::decode_client_info(r).value();
    CHECK(r.empty());
    CHECK(decoded.user_name == "jürgen");
    CHECK(decoded.password.view() == "s3cr3t");
    CHECK(decoded.working_dir == "C:\\");
    REQUIRE(decoded.extended.has_value());
    CHECK(decoded.extended->client_address == "192.0.2.10");
    CHECK(decoded.extended->time_zone->bias == -60);
    CHECK(decoded.extended->time_zone->standard_name == "W. Europe Standard Time");
    CHECK(decoded.extended->session_id == 7);
    REQUIRE(decoded.extended->auto_reconnect_cookie.has_value());
    CHECK(decoded.extended->auto_reconnect_cookie->logon_id == 42);
    CHECK(decoded.extended->dynamic_dst_time_zone_key_name == "W. Europe Standard Time");
    CHECK(decoded.extended->dynamic_daylight_time_disabled == false);
}

TEST_CASE("Client Info limits and ANSI strings")
{
    // cbUserName 514 exceeds the 512-byte limit.
    const auto too_long = hex("00 00 00 00 10 00 00 00 00 00 02 02 00 00 00 00 00 00 00 00");  // + domain terminator
    Reader r1(too_long);
    CHECK(proto::decode_client_info(r1).error().code == Errc::limit_exceeded);

    // Odd byte count for a Unicode string.
    const auto odd = hex("00 00 00 00 10 00 00 00 00 00 03 00 00 00 00 00 00 00 41 00 42 00 00");
    Reader r2(odd);
    CHECK(proto::decode_client_info(r2).error().code == Errc::invalid_length);

    // ANSI (no INFO_UNICODE): one-byte terminators, Latin-1 decoded to UTF-8.
    const auto ansi = hex("00 00 00 00 00 00 00 00 00 00 02 00 00 00 00 00 00 00 00 fc 41 00 00 00 00");
    Reader r3(ansi);
    const auto info = proto::decode_client_info(r3).value();
    CHECK(info.user_name == "\xc3\xbc"
                            "A");
}
