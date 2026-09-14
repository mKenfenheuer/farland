// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/text.hpp>
#include <farland/channels/clipboard_formats.hpp>
#include <farland/channels/cliprdr.hpp>
#include <farland/channels/svc.hpp>
#include <farland/codec/dib.hpp>
#include <farland/codec/png.hpp>
#include <farland/server/clipboard_bridge.hpp>

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>

using namespace farland;
using namespace farland::channels::cliprdr;
using server::ClipboardBridge;
namespace fs = std::filesystem;
namespace pev = platform::clipboard_event;
namespace svc = channels::svc;

namespace {

using Bytes = std::vector<std::byte>;

Bytes bytes(std::string_view text)
{
    const auto view = std::as_bytes(std::span(text));
    return {view.begin(), view.end()};
}

Bytes test_u64(std::uint64_t value)
{
    Bytes out;
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFF));
    }
    return out;
}

std::string text(const Bytes& data)
{
    std::string out;
    for (const auto b : data) {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

/// A desktop clipboard in memory.
class FakeClipboard final : public platform::Clipboard {
public:
    [[nodiscard]] std::optional<std::vector<std::string>> mime_types() const override { return current; }
    void set_selection(const std::vector<std::string>& mime_types) override
    {
        selection = mime_types;
        current.reset();
    }
    void write(std::uint32_t serial, std::optional<std::vector<std::byte>> data) override
    {
        written[serial] = std::move(data);
    }
    [[nodiscard]] std::uint64_t read(const std::string& mime_type) override
    {
        const auto id = next_read++;
        reads.push_back(mime_type);
        const auto found = contents.find(mime_type);
        events.emplace_back(pev::ReadFinished{
            id, found == contents.end() ? std::nullopt : std::optional<std::vector<std::byte>>(found->second)});
        return id;
    }
    [[nodiscard]] std::optional<platform::ClipboardEvent> poll_event() override
    {
        if (events.empty()) {
            return std::nullopt;
        }
        auto event = std::move(events.front());
        events.pop_front();
        return event;
    }
    [[nodiscard]] std::vector<platform::PollFd> poll_fds() const override { return {}; }
    void dispatch() override {}

    /// Someone on the desktop copies.
    void copy(std::map<std::string, Bytes> data)
    {
        contents = std::move(data);
        std::vector<std::string> mimes;
        for (const auto& [mime, content] : contents) {
            mimes.push_back(mime);
        }
        current = mimes;
        events.emplace_back(pev::OwnerChanged{mimes});
    }
    /// Someone on the desktop pastes.
    void paste(std::uint32_t serial, std::string mime) { events.emplace_back(pev::TransferRequested{serial, mime}); }

    std::optional<std::vector<std::string>> current;
    std::map<std::string, Bytes> contents;
    std::vector<std::string> selection;
    std::map<std::uint32_t, std::optional<Bytes>> written;
    std::vector<std::string> reads;
    std::deque<platform::ClipboardEvent> events;
    std::uint64_t next_read = 1;
};

struct TempDir {
    TempDir()
    {
        std::random_device random;
        path = fs::temp_directory_path() / ("farland-bridge-" + std::to_string(random()));
        fs::create_directories(path);
        fs::permissions(path, fs::perms::owner_all);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path path;
};

constexpr std::uint32_t client_flags = general_flag::use_long_format_names | general_flag::stream_fileclip_enabled |
                                       general_flag::fileclip_no_file_paths | general_flag::can_lock_clipdata |
                                       general_flag::huge_file_support_enabled;

/// The bridge with a client on the other end of the channel.
struct Session {
    explicit Session(server::ClipboardOptions options = {})
    {
        options.staging_root = temp.path / "staging";
        bridge = std::make_unique<ClipboardBridge>(
            desktop,
            [this](std::span<const std::byte> chunk) {
                auto message = from_bridge.add(chunk);
                REQUIRE(message.has_value());
                if (*message) {
                    auto pdu = decode(**message, true);
                    REQUIRE(pdu.has_value());
                    received.push_back(std::move(*pdu));
                }
            },
            options);
    }

    void send(const Pdu& pdu)
    {
        for (const auto& chunk : svc::encode_chunks(encode(pdu, true))) {
            REQUIRE(bridge->receive(chunk, now).has_value());
        }
    }
    void service() { bridge->service(now); }
    std::vector<Pdu> take()
    {
        service();
        return std::exchange(received, {});
    }
    template <class T>
    T take_one()
    {
        auto pdus = take();
        REQUIRE(pdus.size() == 1);
        REQUIRE(std::holds_alternative<T>(pdus.front()));
        return std::get<T>(pdus.front());
    }
    void start(std::vector<Format> client_formats = {})
    {
        bridge->start();
        CHECK(take().size() == 2);  // capabilities, monitor ready
        send(Capabilities{2, client_flags});
        send(FormatList{std::move(client_formats)});
    }

    TempDir temp;
    FakeClipboard desktop;
    std::unique_ptr<ClipboardBridge> bridge;
    svc::Reassembler from_bridge{std::size_t{128} * 1024 * 1024};
    std::vector<Pdu> received;
    ClipboardBridge::Clock::time_point now{std::chrono::seconds(100)};
};

codec::RgbaImage image(std::uint32_t width, std::uint32_t height)
{
    codec::RgbaImage img{width, height, {}};
    for (std::uint32_t i = 0; i < width * height; ++i) {
        img.pixels.insert(img.pixels.end(), {static_cast<std::uint8_t>(i * 9), static_cast<std::uint8_t>(i * 3), 77,
                                             static_cast<std::uint8_t>(200 + (i % 50))});
    }
    return img;
}

}  // namespace

TEST_CASE("Clipboard bridge: client text and HTML pasted on the desktop")
{
    Session s;
    s.start({{cf::unicode_text, ""}, {cf::text, ""}, {0xC0AA, "HTML Format"}, {0xC0AB, "Rich Text Format"}});
    CHECK(s.take_one<FormatListResponse>().ok);
    CHECK(s.desktop.selection == std::vector<std::string>{"text/plain;charset=utf-8", "text/plain", "text/html"});

    s.desktop.paste(7, "text/plain;charset=utf-8");
    CHECK(s.take_one<FormatDataRequest>().format_id == cf::unicode_text);
    s.send(FormatDataResponse{true, channels::clipboard::utf8_to_unicode_text("h\xC3\xA9llo\nworld")});
    s.service();
    REQUIRE(s.desktop.written.at(7).has_value());
    CHECK(text(*s.desktop.written.at(7)) == "h\xC3\xA9llo\nworld");

    s.desktop.paste(8, "text/html");
    CHECK(s.take_one<FormatDataRequest>().format_id == 0xC0AA);
    s.send(FormatDataResponse{true, channels::clipboard::html_to_cf_html(bytes("<b>x</b>"))});
    s.service();
    CHECK(text(s.desktop.written.at(8).value())
              .ends_with("<!--StartFragment--><b>x</b><!--EndFragment--></body></html>"));

    SECTION("two pastes at once go one after the other")
    {
        s.desktop.paste(9, "text/plain");
        s.desktop.paste(10, "UTF8_STRING");
        CHECK(s.take_one<FormatDataRequest>().format_id == cf::unicode_text);
        s.send(FormatDataResponse{false, {}});
        CHECK(s.take_one<FormatDataRequest>().format_id == cf::unicode_text);
        s.send(FormatDataResponse{true, channels::clipboard::utf8_to_unicode_text("two")});
        s.service();
        CHECK_FALSE(s.desktop.written.at(9).has_value());
        CHECK(text(s.desktop.written.at(10).value()) == "two");
    }
    SECTION("types the client does not have are refused at once")
    {
        s.desktop.paste(11, "image/png");
        s.desktop.paste(12, "application/x-unknown");
        CHECK(s.take().empty());
        CHECK_FALSE(s.desktop.written.at(11).has_value());
        CHECK_FALSE(s.desktop.written.at(12).has_value());
    }
    SECTION("the client never answers")
    {
        s.desktop.paste(13, "text/plain");
        static_cast<void>(s.take());
        s.now += std::chrono::seconds(31);
        s.service();
        CHECK_FALSE(s.desktop.written.at(13).has_value());
    }
    SECTION("a bridge that goes away refuses what is waiting")
    {
        s.desktop.paste(14, "text/plain");
        static_cast<void>(s.take());
        s.bridge.reset();
        CHECK_FALSE(s.desktop.written.at(14).has_value());
    }
}

TEST_CASE("Clipboard bridge: client images pasted on the desktop")
{
    Session s;
    s.start({{cf::dib, ""}});
    static_cast<void>(s.take());
    const auto dib = codec::encode_dib(image(3, 2), codec::DibHeader::v5);

    s.desktop.paste(1, "image/bmp");
    CHECK(s.take_one<FormatDataRequest>().format_id == cf::dib);
    s.send(FormatDataResponse{true, dib});
    s.service();
    const auto bmp = s.desktop.written.at(1).value();
    CHECK(codec::decode_dib(codec::bmp_to_dib(bmp).value()).value() == image(3, 2));

    if (codec::png_supported()) {
        CHECK(s.desktop.selection == std::vector<std::string>{"image/png", "image/bmp"});
        s.desktop.paste(2, "image/png");
        CHECK(s.take_one<FormatDataRequest>().format_id == cf::dib);
        s.send(FormatDataResponse{true, dib});
        s.service();
        CHECK(codec::decode_png(s.desktop.written.at(2).value()).value() == image(3, 2));

        // A client with "PNG" hands it over as it is.
        s.send(FormatList{{{cf::dib, ""}, {0xC0F0, "PNG"}}});
        static_cast<void>(s.take());
        s.desktop.paste(3, "image/png");
        CHECK(s.take_one<FormatDataRequest>().format_id == 0xC0F0);
        const auto png = codec::encode_png(image(2, 2)).value();
        s.send(FormatDataResponse{true, png});
        s.service();
        CHECK(s.desktop.written.at(3) == png);
    }
}

TEST_CASE("Clipboard bridge: desktop content pasted on the client")
{
    Session s;
    s.start({{cf::unicode_text, ""}});
    static_cast<void>(s.take());
    const auto png = codec::png_supported() ? codec::encode_png(image(4, 3)).value() : Bytes{};
    std::map<std::string, Bytes> contents{
        {"text/plain;charset=utf-8", bytes("one\ntwo")},
        {"text/html", bytes("<i>it</i>")},
        {"image/bmp", codec::dib_to_bmp(codec::encode_dib(image(4, 3), codec::DibHeader::v5)).value()}};
    if (codec::png_supported()) {
        contents["image/png"] = png;
    }
    s.desktop.copy(contents);
    const auto list = s.take_one<FormatList>().formats;
    std::vector<Format> expected{{cf::unicode_text, ""}, {0xD010, "HTML Format"}, {cf::dib, ""}, {cf::dibv5, ""}};
    if (codec::png_supported()) {
        expected.push_back({0xD011, "PNG"});
    }
    CHECK(list == expected);
    s.send(FormatListResponse{true});

    s.send(FormatDataRequest{cf::unicode_text});
    auto response = s.take_one<FormatDataResponse>();
    CHECK(channels::clipboard::unicode_text_to_utf8(response.data) == "one\ntwo");
    CHECK(response.data == channels::clipboard::utf8_to_unicode_text("one\ntwo"));

    s.send(FormatDataRequest{0xD010});
    CHECK(channels::clipboard::cf_html_to_html(s.take_one<FormatDataResponse>().data)
              .value()
              .ends_with("<!--StartFragment--><i>it</i><!--EndFragment--></body></html>"));

    s.send(FormatDataRequest{cf::dibv5});
    CHECK(codec::decode_dib(s.take_one<FormatDataResponse>().data).value() == image(4, 3));
    s.send(FormatDataRequest{cf::dib});
    CHECK(codec::decode_dib(s.take_one<FormatDataResponse>().data).value().width == 4);
    if (codec::png_supported()) {
        s.send(FormatDataRequest{0xD011});
        CHECK(s.take_one<FormatDataResponse>().data == png);
    }

    SECTION("the desktop fails a read")
    {
        s.desktop.contents.clear();
        s.send(FormatDataRequest{cf::unicode_text});
        CHECK_FALSE(s.take_one<FormatDataResponse>().ok);
    }
    SECTION("the client copies: the desktop gets its content")
    {
        s.send(FormatList{{{cf::unicode_text, ""}}});
        CHECK(s.take_one<FormatListResponse>().ok);
        CHECK(s.desktop.selection == std::vector<std::string>{"text/plain;charset=utf-8", "text/plain"});
        s.send(FormatDataRequest{cf::unicode_text});  // stale
        CHECK_FALSE(s.take_one<FormatDataResponse>().ok);
    }
}

TEST_CASE("Clipboard bridge: at connection the client's empty clipboard gets the desktop's")
{
    Session s;
    s.desktop.current = std::vector<std::string>{"text/plain"};
    s.desktop.contents["text/plain"] = bytes("before");
    s.start({});
    const auto pdus = s.take();
    REQUIRE(pdus.size() == 2);
    CHECK(std::get<FormatList>(pdus[1]).formats == std::vector<Format>{{cf::unicode_text, ""}});
    CHECK(s.desktop.selection.empty());
}

TEST_CASE("Clipboard bridge: the client's files pasted on the desktop")
{
    Session s;
    s.start({{0xC0F1, file_group_descriptor_w}});
    static_cast<void>(s.take());
    CHECK(s.desktop.selection == std::vector<std::string>{"text/uri-list", "x-special/gnome-copied-files"});

    const std::vector<FileDescriptor> files{
        {fd_flag::attributes, file_attribute::directory, 0, 0, "dir"},
        {fd_flag::file_size, file_attribute::archive, 0, 5, R"(dir\a.txt)"},
        {0, file_attribute::archive, 0, 0, "b.bin"},  // no size: asked for
    };
    s.desktop.paste(20, "text/uri-list");
    CHECK(s.take_one<FormatDataRequest>().format_id == 0xC0F1);
    s.send(FormatDataResponse{true, encode_file_list(files)});

    auto pdus = s.take();
    REQUIRE(pdus.size() == 2);
    const auto lock = std::get<LockClipData>(pdus[0]).clip_data_id;
    auto request = std::get<FileContentsRequest>(pdus[1]);
    CHECK(request.index == 1);
    CHECK(request.flags == file_contents::range);
    CHECK(request.requested == 5);
    CHECK(request.clip_data_id == lock);
    s.send(FileContentsResponse{true, request.stream_id, bytes("hel")});  // short: asks for the rest
    request = s.take_one<FileContentsRequest>();
    CHECK(request.position == 3);
    CHECK(request.requested == 2);
    s.send(FileContentsResponse{true, request.stream_id, bytes("lo")});
    request = s.take_one<FileContentsRequest>();
    CHECK(request.index == 2);
    CHECK(request.flags == file_contents::size);
    s.send(FileContentsResponse{true, request.stream_id, test_u64(3)});
    request = s.take_one<FileContentsRequest>();
    s.send(FileContentsResponse{true, request.stream_id, bytes("xyz")});
    CHECK(s.take_one<UnlockClipData>().clip_data_id == lock);

    const auto uris = text(s.desktop.written.at(20).value());
    const auto paths = server::clipboard_files::parse_uri_list(uris);
    REQUIRE(paths.size() == 2);
    CHECK(paths[0].filename() == "dir");
    CHECK(paths[1].filename() == "b.bin");
    CHECK(paths[0].parent_path().parent_path() == s.temp.path / "staging");
    std::ifstream in(paths[0] / "a.txt");
    CHECK(std::string(std::istreambuf_iterator<char>(in), {}) == "hello");

    // The same clipboard again: no new transfer.
    s.desktop.paste(21, "x-special/gnome-copied-files");
    CHECK(s.take().empty());
    CHECK(text(s.desktop.written.at(21).value()).starts_with("copy\nfile://"));

    // The bridge's staged files go with it.
    s.bridge.reset();
    CHECK_FALSE(fs::exists(paths[0]));
}

TEST_CASE("Clipboard bridge: dangerous or oversized client files are refused")
{
    server::ClipboardOptions options;
    options.max_staged_bytes = 100;
    Session s(options);
    s.start({{0xC0F1, file_group_descriptor_w}});
    static_cast<void>(s.take());

    SECTION("a name that leaves the staging directory")
    {
        s.desktop.paste(1, "text/uri-list");
        static_cast<void>(s.take());
        const FileDescriptor evil{fd_flag::file_size, 0, 0, 1, R"(..\..\evil.txt)"};
        s.send(FormatDataResponse{true, encode_file_list(std::span(&evil, 1))});
        CHECK(s.take().empty());
        CHECK_FALSE(s.desktop.written.at(1).has_value());
        CHECK_FALSE(fs::exists(s.temp.path / "evil.txt"));
    }
    SECTION("more than the limit, declared")
    {
        s.desktop.paste(2, "text/uri-list");
        static_cast<void>(s.take());
        const FileDescriptor big{fd_flag::file_size, 0, 0, 101, "big.bin"};
        s.send(FormatDataResponse{true, encode_file_list(std::span(&big, 1))});
        CHECK(s.take().empty());
        CHECK_FALSE(s.desktop.written.at(2).has_value());
    }
    SECTION("more than the limit, discovered")
    {
        s.desktop.paste(3, "text/uri-list");
        static_cast<void>(s.take());
        const FileDescriptor unknown{0, 0, 0, 0, "unknown.bin"};
        s.send(FormatDataResponse{true, encode_file_list(std::span(&unknown, 1))});
        auto pdus = s.take();
        const auto request = std::get<FileContentsRequest>(pdus.back());
        s.send(FileContentsResponse{true, request.stream_id, test_u64(1000)});
        CHECK(std::holds_alternative<UnlockClipData>(s.take().back()));
        CHECK_FALSE(s.desktop.written.at(3).has_value());
    }
}

TEST_CASE("Clipboard bridge: desktop files pasted on the client")
{
    Session s;
    s.start({});
    static_cast<void>(s.take());
    const auto dir = s.temp.path / "files";
    fs::create_directories(dir / "sub");
    std::ofstream(dir / "sub" / "one.txt") << "first file";
    std::ofstream(dir / "two.txt") << "second";
    s.desktop.copy({{"text/uri-list", bytes(server::clipboard_files::file_uri(dir / "sub") + "\r\n" +
                                            server::clipboard_files::file_uri(dir / "two.txt") + "\r\n")},
                    {"text/plain", bytes("paths")}});
    CHECK(s.take_one<FormatList>().formats ==
          std::vector<Format>{{cf::unicode_text, ""}, {0xD012, file_group_descriptor_w}});
    s.send(FormatListResponse{true});

    s.send(FormatDataRequest{0xD012});
    const auto list = decode_file_list(s.take_one<FormatDataResponse>().data).value();
    REQUIRE(list.size() == 3);
    CHECK(list[0].name == "sub");
    CHECK(list[1].name == R"(sub\one.txt)");
    CHECK(list[2].name == "two.txt");

    s.send(LockClipData{5});
    s.send(FileContentsRequest{1, 1, file_contents::size, 0, 8, 5U});
    CHECK(s.take_one<FileContentsResponse>() == FileContentsResponse{true, 1, test_u64(10)});
    // The desktop copies something else; the locked list still serves.
    s.desktop.copy({{"text/plain", bytes("new")}});
    static_cast<void>(s.take());
    s.send(FileContentsRequest{2, 1, file_contents::range, 6, 100, 5U});
    CHECK(text(s.take_one<FileContentsResponse>().data) == "file");
    s.send(UnlockClipData{5});
    s.send(FileContentsRequest{3, 1, file_contents::range, 0, 100, 5U});
    CHECK_FALSE(s.take_one<FileContentsResponse>().ok);
}
