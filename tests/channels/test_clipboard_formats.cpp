// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/text.hpp>
#include <farland/channels/clipboard_formats.hpp>

#include "support/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <vector>

using farland::test::hex;
using namespace farland::channels::clipboard;

namespace {

using Bytes = std::vector<std::byte>;

Bytes bytes(std::string_view text)
{
    const auto view = farland::test::ascii(text);
    return {view.begin(), view.end()};
}

std::string text(const Bytes& data)
{
    std::string out;
    for (const auto b : data) {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

/// The number after "Key:" in a CF_HTML header.
std::size_t offset(const std::string& cf_html, const std::string& key)
{
    const auto at = cf_html.find(key + ":");
    REQUIRE(at != std::string::npos);
    return std::stoul(cf_html.substr(at + key.size() + 1, 10));
}

}  // namespace

TEST_CASE("Clipboard text: CF_UNICODETEXT with CRLF and a terminator")
{
    const auto unicode = utf8_to_unicode_text("a\nb\r\nc\xE2\x82\xAC");
    CHECK(unicode == hex("61 00 0d 00 0a 00 62 00 0d 00 0a 00 63 00 ac 20 00 00"));
    CHECK(unicode_text_to_utf8(unicode) == "a\nb\nc\xE2\x82\xAC");
    CHECK(unicode_text_to_utf8(hex("68 00 69 00 00 00 78 00")) == "hi");  // up to the NUL
    CHECK(unicode_text_to_utf8(hex("3d d8 00 de")) == "\xF0\x9F\x98\x80");
    CHECK(unicode_text_to_utf8(hex("00 d8 41 00")) == "\xEF\xBF\xBD"
                                                      "A");
    CHECK(utf8_to_unicode_text("") == hex("00 00"));
}

TEST_CASE("Clipboard text: CF_TEXT as Windows-1252")
{
    CHECK(ansi_text_to_utf8(bytes("caf\xE9 \x80\r\nx\0rest")) == "caf\xC3\xA9 \xE2\x82\xAC\nx");
    CHECK(ansi_text_to_utf8(bytes("\x81")) == "\xEF\xBF\xBD");
}

TEST_CASE("Clipboard text: local text encodings")
{
    CHECK(local_text_to_utf8(bytes("plain \xC3\xA9")) == "plain \xC3\xA9");
    CHECK(local_text_to_utf8(bytes("\xEF\xBB\xBFwith BOM")) == "with BOM");
    CHECK(local_text_to_utf8(hex("ff fe 68 00 69 00")) == "hi");
    CHECK(local_text_to_utf8(hex("fe ff 00 68 00 69")) == "hi");
    CHECK(local_text_to_utf8(bytes("bad \xFF")) == "bad \xEF\xBF\xBD");
}

TEST_CASE("HTML Format: a fragment is wrapped in a document")
{
    const auto data = html_to_cf_html(bytes("<b>bold</b> \xC3\xA9"));
    REQUIRE(data.back() == std::byte{0});
    const std::string cf_html = text(Bytes(data.begin(), data.end() - 1));
    CHECK(cf_html.starts_with("Version:0.9\r\nStartHTML:"));
    const auto start_html = offset(cf_html, "StartHTML");
    const auto end_html = offset(cf_html, "EndHTML");
    const auto start_fragment = offset(cf_html, "StartFragment");
    const auto end_fragment = offset(cf_html, "EndFragment");
    CHECK(cf_html.substr(start_fragment, end_fragment - start_fragment) == "<b>bold</b> \xC3\xA9");
    CHECK(cf_html.substr(start_html, end_html - start_html) ==
          "<html><body><!--StartFragment--><b>bold</b> \xC3\xA9<!--EndFragment--></body></html>");
    CHECK(end_html == cf_html.size());

    const auto html = cf_html_to_html(data);
    REQUIRE(html.has_value());
    CHECK(*html == "<meta http-equiv=\"content-type\" content=\"text/html; charset=utf-8\">"
                   "<html><body><!--StartFragment--><b>bold</b> \xC3\xA9<!--EndFragment--></body></html>");
}

TEST_CASE("HTML Format: a document keeps its body")
{
    const std::string document = "<html><head><meta charset=\"utf-8\"></head><BODY class=\"x\"><p>one</p>"
                                 "<p>two</p></Body></html>";
    const auto data = html_to_cf_html(bytes(document));
    const std::string cf_html = text(Bytes(data.begin(), data.end() - 1));
    const auto start_fragment = offset(cf_html, "StartFragment");
    const auto end_fragment = offset(cf_html, "EndFragment");
    CHECK(cf_html.substr(start_fragment, end_fragment - start_fragment) == "<p>one</p><p>two</p>");
    CHECK(cf_html.substr(offset(cf_html, "StartHTML")) ==
          "<html><head><meta charset=\"utf-8\"></head><BODY class=\"x\"><!--StartFragment--><p>one</p><p>two</p>"
          "<!--EndFragment--></Body></html>");
    // It declares a charset already.
    CHECK(cf_html_to_html(data).value().starts_with("<html><head>"));
}

TEST_CASE("HTML Format from Windows")
{
    const std::string body = "<html><body>\r\n<!--StartFragment--><i>x</i><!--EndFragment-->\r\n</body></html>";
    const auto header = [](std::size_t start_html, std::size_t end_html, std::size_t start_fragment,
                           std::size_t end_fragment) {
        return std::format("Version:1.0\r\nStartHTML:{:010}\r\nEndHTML:{:010}\r\nStartFragment:{:010}\r\n"
                           "EndFragment:{:010}\r\nSourceURL:https://example.com/\r\n",
                           start_html, end_html, start_fragment, end_fragment);
    };
    const std::size_t header_size = header(0, 0, 0, 0).size();
    const std::size_t fragment = header_size + body.find("<i>");
    const std::string data =
        header(header_size, header_size + body.size(), fragment, fragment + 8) + body + std::string(1, '\0');
    const auto html = cf_html_to_html(bytes(data));
    REQUIRE(html.has_value());
    CHECK(html->ends_with(body));

    SECTION("without StartHTML the fragment is used")
    {
        const std::string fragment_only = "Version:0.9\r\nStartHTML:-1\r\nEndHTML:-1\r\nStartFragment:0000000089\r\n"
                                          "EndFragment:0000000097\r\n<i>x</i>";
        CHECK(cf_html_to_html(bytes(fragment_only)).value().ends_with("<i>x</i>"));
    }
    SECTION("bad offsets")
    {
        CHECK_FALSE(cf_html_to_html(bytes("<b>no header</b>")).has_value());
        CHECK_FALSE(
            cf_html_to_html(bytes("Version:0.9\r\nStartHTML:0000000010\r\nEndHTML:0000009999\r\n")).has_value());
        CHECK_FALSE(
            cf_html_to_html(bytes("Version:0.9\r\nStartHTML:0000000040\r\nEndHTML:0000000030\r\n")).has_value());
        CHECK_FALSE(cf_html_to_html(bytes("Version:0.9\r\nStartHTML:12x\r\nEndHTML:0000000030\r\n")).has_value());
    }
}
