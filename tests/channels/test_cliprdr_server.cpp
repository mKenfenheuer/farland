// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/channels/cliprdr_server.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using farland::Errc;
using namespace farland::channels::cliprdr;
using namespace std::chrono_literals;
namespace ev = farland::channels::cliprdr::server_event;

namespace {

using Bytes = std::vector<std::byte>;
using Clock = ClipboardServer::Clock;
constexpr std::uint32_t all_flags = general_flag::use_long_format_names | general_flag::stream_fileclip_enabled |
                                    general_flag::fileclip_no_file_paths | general_flag::can_lock_clipdata |
                                    general_flag::huge_file_support_enabled;
constexpr std::uint32_t html_id = 0xD001;
constexpr std::uint32_t files_id = 0xD002;

Bytes bytes(std::string_view text)
{
    const auto view = farland::test::ascii(text);
    return {view.begin(), view.end()};
}

/// A client driving the server with real PDUs.
struct Client {
    explicit Client(ServerConfig config = {}) : server(config) { server.start(); }

    ClipboardServer server;
    Clock::time_point now{std::chrono::seconds(1000)};
    bool long_names = false;

    void send(const Pdu& pdu)
    {
        auto result = server.receive(encode(pdu, long_names), now);
        if (!result) {
            FAIL(result.error().message());
        }
    }
    std::vector<Pdu> output()
    {
        std::vector<Pdu> pdus;
        for (const auto& message : server.take_output()) {
            auto pdu = decode(message, server.has(general_flag::use_long_format_names));
            REQUIRE(pdu.has_value());
            pdus.push_back(std::move(*pdu));
        }
        return pdus;
    }
    std::vector<ServerEvent> events()
    {
        std::vector<ServerEvent> all;
        while (auto event = server.poll_event()) {
            all.push_back(std::move(*event));
        }
        return all;
    }
    template <class T>
    T only(std::vector<ServerEvent> all)
    {
        REQUIRE(all.size() == 1);
        REQUIRE(std::holds_alternative<T>(all.front()));
        return std::get<T>(all.front());
    }
    template <class T>
    T only_output()
    {
        auto pdus = output();
        REQUIRE(pdus.size() == 1);
        REQUIRE(std::holds_alternative<T>(pdus.front()));
        return std::get<T>(pdus.front());
    }
    /// Capabilities and the client's first Format List.
    void handshake(std::uint32_t flags = all_flags, std::vector<Format> formats = {})
    {
        static_cast<void>(output());
        send(Capabilities{2, flags});
        long_names = (flags & general_flag::use_long_format_names) != 0;
        send(FormatList{std::move(formats)});
        CHECK(only_output<FormatListResponse>().ok);
        const auto all = events();
        REQUIRE(all.size() == 2);
        CHECK(std::get<ev::Ready>(all[0]).flags == flags);
        std::get<ev::RemoteFormatList>(all[1]);
    }
    /// Announces text, HTML and files, and has the client accept them.
    void announce()
    {
        server.announce({{cf::unicode_text, ""}, {html_id, html_format}, {files_id, file_group_descriptor_w}});
        CHECK(only_output<FormatList>().formats.size() == 3);
        send(FormatListResponse{true});
        CHECK(only<ev::FormatListAnswered>(events()).ok);
    }
};

}  // namespace

TEST_CASE("Clipboard server: initialization sequence ([MS-RDPECLIP] 1.3.2.1)")
{
    Client client;
    const auto pdus = client.output();
    REQUIRE(pdus.size() == 2);
    CHECK(std::get<Capabilities>(pdus[0]) == Capabilities{caps_version_2, all_flags});
    std::get<MonitorReady>(pdus[1]);
    CHECK_FALSE(client.server.ready());

    SECTION("capabilities, temporary directory and format list")
    {
        client.send(Capabilities{2, general_flag::use_long_format_names | general_flag::stream_fileclip_enabled});
        client.long_names = true;
        CHECK(client.only<ev::Ready>(client.events()).flags ==
              (general_flag::use_long_format_names | general_flag::stream_fileclip_enabled));
        client.send(TempDirectory{R"(C:\temp)"});
        client.send(FormatList{{{cf::unicode_text, ""}, {0xC0FE, "HTML Format"}}});
        CHECK(client.only_output<FormatListResponse>().ok);
        CHECK(client.only<ev::RemoteFormatList>(client.events()).formats ==
              std::vector<Format>{{cf::unicode_text, ""}, {0xC0FE, "HTML Format"}});
        CHECK(client.server.has(general_flag::use_long_format_names));
        CHECK_FALSE(client.server.has(general_flag::can_lock_clipdata));
    }
    SECTION("a client without capabilities uses short names")
    {
        client.send(FormatList{{{cf::unicode_text, ""}, {0xC0FE, "HTML Format"}}});
        const auto all = client.events();
        REQUIRE(all.size() == 2);
        CHECK(std::get<ev::Ready>(all[0]).flags == 0);
        CHECK(std::get<ev::RemoteFormatList>(all[1]).formats.at(1).name == "HTML Format");
        client.server.announce({{html_id, html_format}});
        const auto sent = client.output();
        REQUIRE(sent.size() == 2);
        CHECK(std::get<FormatList>(sent[1]).formats == std::vector<Format>{{html_id, html_format}});
    }
    SECTION("an announcement waits for the client")
    {
        client.server.announce({{cf::unicode_text, ""}});
        CHECK(client.output().empty());
        client.send(Capabilities{2, all_flags});
        client.long_names = true;
        CHECK(client.only_output<FormatList>().formats == std::vector<Format>{{cf::unicode_text, ""}});
    }
    SECTION("server-to-client and unknown PDUs are ignored")
    {
        client.send(MonitorReady{});
        auto unknown = encode(MonitorReady{});
        unknown[0] = std::byte{0x42};
        CHECK(client.server.receive(unknown, client.now).has_value());
        CHECK(client.events().empty());
        CHECK_FALSE(client.server.ready());
    }
}

TEST_CASE("Clipboard server: pasting on the server ([MS-RDPECLIP] 1.3.2.2.3)")
{
    Client client;
    client.handshake(all_flags, {{cf::unicode_text, ""}, {0xC0FE, "HTML Format"}});

    REQUIRE(client.server.request_data(cf::unicode_text, client.now));
    CHECK(client.only_output<FormatDataRequest>().format_id == cf::unicode_text);
    CHECK(client.server.busy());
    CHECK_FALSE(client.server.request_data(0xC0FE, client.now));
    CHECK_FALSE(client.server.request_file_contents(0, file_contents::size, 0, 8, std::nullopt, client.now));

    client.send(FormatDataResponse{true, bytes("h\0i\0\0\0")});
    CHECK(client.only<ev::DataReceived>(client.events()).data == bytes("h\0i\0\0\0"));
    CHECK_FALSE(client.server.busy());

    REQUIRE(client.server.request_data(0xC0FE, client.now));
    client.send(FormatDataResponse{false, {}});
    CHECK_FALSE(client.only<ev::DataReceived>(client.events()).data.has_value());

    SECTION("a request the client does not answer times out")
    {
        REQUIRE(client.server.request_data(cf::unicode_text, client.now));
        static_cast<void>(client.output());
        client.now += 29s;
        client.server.tick(client.now);
        CHECK(client.events().empty());
        client.now += 1s;
        client.server.tick(client.now);
        CHECK_FALSE(client.only<ev::DataReceived>(client.events()).data.has_value());
        // The response may still come, and it carries no ID: wait for it.
        CHECK_FALSE(client.server.request_data(cf::unicode_text, client.now));
        client.send(FormatDataResponse{true, bytes("late")});
        CHECK(client.events().empty());
        CHECK(client.server.request_data(cf::unicode_text, client.now));
    }
    SECTION("or it never comes")
    {
        REQUIRE(client.server.request_data(cf::unicode_text, client.now));
        client.now += 30s;
        client.server.tick(client.now);
        static_cast<void>(client.events());
        client.now += 30s;
        client.server.tick(client.now);
        CHECK(client.server.request_data(cf::unicode_text, client.now));
    }
    SECTION("responses nobody asked for are ignored")
    {
        client.send(FormatDataResponse{true, bytes("x")});
        client.send(FileContentsResponse{true, 9, bytes("x")});
        CHECK(client.events().empty());
    }
}

TEST_CASE("Clipboard server: pasting on the client ([MS-RDPECLIP] 3.1.5.4.2)")
{
    Client client;
    client.handshake();
    client.announce();

    client.send(FormatDataRequest{html_id});
    const auto first = client.only<ev::DataRequested>(client.events());
    CHECK(first.format_id == html_id);
    // A second request waits for the first answer; responses have no ID.
    client.send(FormatDataRequest{cf::unicode_text});
    CHECK(client.events().empty());
    CHECK(client.output().empty());

    CHECK(client.server.answer(first.id, bytes("<b>")));
    const auto second = client.only<ev::DataRequested>(client.events());
    CHECK(second.format_id == cf::unicode_text);
    CHECK_FALSE(client.server.answer(first.id, bytes("again")));
    CHECK(client.server.answer(second.id, std::nullopt));
    const auto pdus = client.output();
    REQUIRE(pdus.size() == 2);
    CHECK(std::get<FormatDataResponse>(pdus[0]) == FormatDataResponse{true, bytes("<b>")});
    CHECK(std::get<FormatDataResponse>(pdus[1]) == FormatDataResponse{false, {}});

    SECTION("formats that were not announced fail at once")
    {
        client.send(FormatDataRequest{cf::dib});
        CHECK(client.events().empty());
        CHECK_FALSE(client.only_output<FormatDataResponse>().ok);
    }
    SECTION("an answer that takes too long fails")
    {
        client.send(FormatDataRequest{cf::unicode_text});
        const auto late = client.only<ev::DataRequested>(client.events());
        client.now += 30s;
        client.server.tick(client.now);
        CHECK_FALSE(client.only_output<FormatDataResponse>().ok);
        CHECK_FALSE(client.server.answer(late.id, bytes("too late")));
        CHECK(client.output().empty());
    }
    SECTION("after the client refused the format list everything fails")
    {
        client.server.announce({{cf::unicode_text, ""}});
        static_cast<void>(client.output());
        client.send(FormatListResponse{false});
        CHECK_FALSE(client.only<ev::FormatListAnswered>(client.events()).ok);
        client.send(FormatDataRequest{cf::unicode_text});
        CHECK_FALSE(client.only_output<FormatDataResponse>().ok);
    }
    SECTION("the client's copy voids the announcement")
    {
        client.send(FormatList{{{cf::unicode_text, ""}}});
        static_cast<void>(client.output());
        static_cast<void>(client.events());
        client.send(FormatDataRequest{cf::unicode_text});
        CHECK_FALSE(client.only_output<FormatDataResponse>().ok);
    }
    SECTION("too many waiting requests are a protocol error")
    {
        for (int i = 0; i < 32; ++i) {
            client.send(FormatDataRequest{cf::unicode_text});
        }
        CHECK(client.server.receive(encode(FormatDataRequest{cf::unicode_text}, true), client.now).error().code ==
              Errc::limit_exceeded);
        CHECK(client.server.failed());
        CHECK_FALSE(client.server.receive(encode(FormatListResponse{true}, true), client.now).has_value());
    }
}

TEST_CASE("Clipboard server: file contents from the server ([MS-RDPECLIP] 3.1.5.4.6)")
{
    Client client;
    client.handshake();
    client.announce();

    client.send(FileContentsRequest{5, 0, file_contents::size, 0, 8, std::nullopt});
    const auto size = client.only<ev::FileContentsRequested>(client.events());
    CHECK(size.request.stream_id == 5);
    CHECK(client.server.answer(size.id, farland::test::hex("2c 00 00 00 00 00 00 00")));
    CHECK(client.only_output<FileContentsResponse>() ==
          FileContentsResponse{true, 5, farland::test::hex("2c 00 00 00 00 00 00 00")});

    client.send(FileContentsRequest{6, 0, file_contents::range, 0, 4, std::nullopt});
    const auto range = client.only<ev::FileContentsRequested>(client.events());
    CHECK(client.server.answer(range.id, bytes("abcdefgh")));  // cut to what was asked
    CHECK(client.only_output<FileContentsResponse>() == FileContentsResponse{true, 6, bytes("abcd")});

    client.send(FileContentsRequest{7, 0, file_contents::size, 0, 8, std::nullopt});
    CHECK(client.server.answer(client.only<ev::FileContentsRequested>(client.events()).id, bytes("1234")));
    CHECK(client.only_output<FileContentsResponse>() == FileContentsResponse{false, 7, {}});

    SECTION("locked data outlives the announcement")
    {
        client.send(LockClipData{3});
        CHECK(client.only<ev::Locked>(client.events()).clip_data_id == 3);
        client.send(FormatList{});
        static_cast<void>(client.output());
        static_cast<void>(client.events());
        client.send(FileContentsRequest{8, 0, file_contents::size, 0, 8, 3U});
        CHECK(client.only<ev::FileContentsRequested>(client.events()).request.clip_data_id == 3U);
        client.send(FileContentsRequest{9, 0, file_contents::size, 0, 8, 4U});  // not locked: waits, then fails
        client.send(UnlockClipData{3});
        client.send(UnlockClipData{3});  // ignored
        CHECK(client.only<ev::Unlocked>(client.events()).clip_data_id == 3);
    }
    SECTION("without files announced, file requests fail")
    {
        client.server.announce({{cf::unicode_text, ""}});
        static_cast<void>(client.output());
        client.send(FileContentsRequest{9, 0, file_contents::size, 0, 8, std::nullopt});
        CHECK(client.only_output<FileContentsResponse>() == FileContentsResponse{false, 9, {}});
    }
}

TEST_CASE("Clipboard server: file contents from the client ([MS-RDPECLIP] 3.1.5.4.5)")
{
    Client client;
    client.handshake(all_flags, {{0xC079, file_group_descriptor_w}});

    const auto stream = client.server.request_file_contents(1, file_contents::size, 0, 0, std::nullopt, client.now);
    REQUIRE(stream.has_value());
    CHECK(client.only_output<FileContentsRequest>() ==
          FileContentsRequest{*stream, 1, file_contents::size, 0, 8, std::nullopt});
    client.send(FileContentsResponse{true, *stream + 1, bytes("ignored!")});
    CHECK(client.events().empty());
    client.send(FileContentsResponse{true, *stream, farland::test::hex("0a 00 00 00 00 00 00 00")});
    const auto sized = client.only<ev::FileContentsReceived>(client.events());
    CHECK(sized.stream_id == *stream);
    CHECK(sized.data == farland::test::hex("0a 00 00 00 00 00 00 00"));

    client.server.lock(11);
    CHECK(client.only_output<LockClipData>().clip_data_id == 11);
    const auto range =
        client.server.request_file_contents(1, file_contents::range, 0x100000000ULL, 16, 11U, client.now);
    REQUIRE(range.has_value());
    CHECK(client.only_output<FileContentsRequest>() ==
          FileContentsRequest{*range, 1, file_contents::range, 0x100000000ULL, 16, 11U});

    SECTION("a response longer than requested is fatal")
    {
        const auto result =
            client.server.receive(encode(FileContentsResponse{true, *range, Bytes(17)}, true), client.now);
        CHECK(result.error().code == Errc::invalid_length);
        CHECK(client.server.failed());
    }
    SECTION("a request the client does not answer times out")
    {
        client.now += 30s;
        client.server.tick(client.now);
        const auto timed_out = client.only<ev::FileContentsReceived>(client.events());
        CHECK(timed_out.stream_id == *range);
        CHECK_FALSE(timed_out.data.has_value());
        // Stream IDs match responses to requests, so the next may go at once.
        CHECK(client.server.request_file_contents(1, file_contents::size, 0, 0, std::nullopt, client.now));
    }
}

TEST_CASE("Clipboard server: what the client cannot do is not asked")
{
    Client client;
    client.handshake(general_flag::use_long_format_names, {{0xC079, file_group_descriptor_w}});
    CHECK_FALSE(client.server.request_file_contents(0, file_contents::size, 0, 0, std::nullopt, client.now));
    client.server.lock(1);
    CHECK(client.output().empty());
    client.send(LockClipData{1});
    CHECK(client.events().empty());

    Client small;
    small.handshake(general_flag::use_long_format_names | general_flag::stream_fileclip_enabled);
    CHECK_FALSE(
        small.server.request_file_contents(0, file_contents::range, 0xFFFFFFF0ULL, 32, std::nullopt, small.now));
    const auto stream = small.server.request_file_contents(0, file_contents::range, 0, 32, 7U, small.now);
    REQUIRE(stream.has_value());
    CHECK_FALSE(small.only_output<FileContentsRequest>().clip_data_id.has_value());  // no CB_CAN_LOCK_CLIPDATA
}

TEST_CASE("Clipboard server: malformed PDUs are fatal")
{
    Client client;
    client.handshake();
    const auto result = client.server.receive(farland::test::hex("04 00 00 00 08 00 00 00 0d 00 00 00"), client.now);
    CHECK(result.error().code == Errc::invalid_length);
    CHECK_FALSE(client.server.request_data(cf::unicode_text, client.now));
    client.server.announce({{cf::unicode_text, ""}});
    CHECK(client.output().empty());
}
