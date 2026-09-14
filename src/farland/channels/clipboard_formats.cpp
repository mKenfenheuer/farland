// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/reader.hpp>
#include <farland/base/text.hpp>
#include <farland/channels/clipboard_formats.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <format>
#include <optional>

namespace farland::channels::clipboard {

namespace {

/// Windows-1252 0x80-0x9F; 0 where the code page leaves a byte undefined.
constexpr std::array<char16_t, 32> cp1252_high = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

void append_utf8(std::string& out, std::uint32_t cp)
{
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

std::string crlf_to_lf(std::string text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

bool starts_with_bytes(std::span<const std::byte> data, std::initializer_list<std::uint8_t> prefix)
{
    if (data.size() < prefix.size()) {
        return false;
    }
    std::size_t i = 0;
    for (const auto b : prefix) {
        if (std::to_integer<std::uint8_t>(data[i++]) != b) {
            return false;
        }
    }
    return true;
}

std::string bytes_to_string(std::span<const std::byte> data)
{
    std::string text(data.size(), '\0');
    std::ranges::transform(data, text.begin(), [](std::byte b) { return static_cast<char>(b); });
    return text;
}

char lower(char c)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

/// Case-insensitive search for an ASCII `needle`; `from_end` finds the last one.
std::optional<std::size_t> find_ascii(std::string_view haystack, std::string_view needle, bool from_end = false)
{
    const auto equal = [](char a, char b) { return lower(a) == lower(b); };
    if (from_end) {
        const auto found = std::ranges::find_end(haystack, needle, equal);
        if (found.empty()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(found.begin() - haystack.begin());
    }
    const auto found = std::ranges::search(haystack, needle, equal);
    if (found.empty()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found.begin() - haystack.begin());
}

/// The value of "Key:NNN" in the CF_HTML header; -1 is allowed for StartHTML.
std::optional<std::int64_t> header_number(std::string_view header, std::string_view key)
{
    std::size_t pos = 0;
    while (pos < header.size()) {
        const std::size_t end = std::min(header.find_first_of("\r\n", pos), header.size());
        const std::string_view line = header.substr(pos, end - pos);
        if (line.starts_with(key) && line.size() > key.size() && line[key.size()] == ':') {
            const std::string_view value = line.substr(key.size() + 1);
            if (value == "-1") {
                return -1;
            }
            if (value.empty() || value.size() > 10) {
                return std::nullopt;
            }
            std::int64_t number = 0;
            for (const char c : value) {
                if (c < '0' || c > '9') {
                    return std::nullopt;
                }
                number = (number * 10) + (c - '0');
            }
            return number;
        }
        pos = end + 1;
    }
    return std::nullopt;
}

}  // namespace

std::vector<std::byte> utf8_to_unicode_text(std::string_view text)
{
    std::string crlf;
    crlf.reserve(text.size() + (text.size() / 16));
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
            crlf.push_back('\r');
        }
        crlf.push_back(text[i]);
    }
    auto out = utf8_to_utf16le(crlf);
    out.push_back(std::byte{0});
    out.push_back(std::byte{0});
    return out;
}

std::string unicode_text_to_utf8(std::span<const std::byte> data)
{
    return crlf_to_lf(utf16le_to_utf8(data));
}

std::string ansi_text_to_utf8(std::span<const std::byte> data)
{
    std::string out;
    out.reserve(data.size());
    for (const std::byte b : data) {
        const auto c = std::to_integer<std::uint8_t>(b);
        if (c == 0) {
            break;
        }
        if (c >= 0x80 && c < 0xA0) {
            const char16_t mapped = cp1252_high.at(c - 0x80U);
            append_utf8(out, mapped != 0 ? mapped : 0xFFFDU);
        } else {
            append_utf8(out, c);
        }
    }
    return crlf_to_lf(std::move(out));
}

std::string local_text_to_utf8(std::span<const std::byte> data)
{
    if (starts_with_bytes(data, {0xFF, 0xFE})) {
        return utf16le_to_utf8(data.subspan(2));
    }
    if (starts_with_bytes(data, {0xFE, 0xFF})) {
        std::vector<std::byte> swapped(data.begin() + 2, data.end() - static_cast<std::ptrdiff_t>(data.size() % 2));
        for (std::size_t i = 0; i + 1 < swapped.size(); i += 2) {
            std::swap(swapped[i], swapped[i + 1]);
        }
        return utf16le_to_utf8(swapped);
    }
    if (starts_with_bytes(data, {0xEF, 0xBB, 0xBF})) {
        data = data.subspan(3);
    }
    // Normalises invalid UTF-8 to U+FFFD like every other path.
    return utf16le_to_utf8(utf8_to_utf16le(bytes_to_string(data)));
}

std::vector<std::byte> html_to_cf_html(std::span<const std::byte> html)
{
    const std::string document = local_text_to_utf8(html);
    std::string_view before;
    std::string_view fragment = document;
    std::string_view after;
    const char* open = "<html><body>";
    const char* close = "</body></html>";
    if (const auto body = find_ascii(document, "<body")) {
        const auto body_end = document.find('>', *body);
        const auto body_close = find_ascii(document, "</body", true);
        if (body_end != std::string::npos && body_close && *body_close > body_end) {
            before = std::string_view(document).substr(0, body_end + 1);
            fragment = std::string_view(document).substr(body_end + 1, *body_close - body_end - 1);
            after = std::string_view(document).substr(*body_close);
            open = close = "";
        }
    }
    // The header has a fixed length: every number has 10 digits.
    const std::string sample = std::format("Version:0.9\r\nStartHTML:{:010}\r\nEndHTML:{:010}\r\n"
                                           "StartFragment:{:010}\r\nEndFragment:{:010}\r\n",
                                           0, 0, 0, 0);
    const std::string_view start_marker = "<!--StartFragment-->";
    const std::string_view end_marker = "<!--EndFragment-->";
    const std::size_t start_html = sample.size();
    const std::size_t start_fragment = start_html + std::string_view(open).size() + before.size() + start_marker.size();
    const std::size_t end_fragment = start_fragment + fragment.size();
    const std::size_t end_html = end_fragment + end_marker.size() + after.size() + std::string_view(close).size();
    std::string out = std::format("Version:0.9\r\nStartHTML:{:010}\r\nEndHTML:{:010}\r\n"
                                  "StartFragment:{:010}\r\nEndFragment:{:010}\r\n",
                                  start_html, end_html, start_fragment, end_fragment);
    out += open;
    out += before;
    out += start_marker;
    out += fragment;
    out += end_marker;
    out += after;
    out += close;
    std::vector<std::byte> bytes(out.size() + 1);
    std::ranges::transform(out, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

// CF_HTML, "HTML Clipboard Format" (Windows Dev Center): the header is ASCII
// "Key:Value" lines; the offsets count bytes from the start of the data.
Result<std::string> cf_html_to_html(std::span<const std::byte> data)
{
    const std::string whole = bytes_to_string(data);
    const std::string_view text = whole;
    const std::string_view header = text.substr(0, std::min<std::size_t>(text.size(), 1024));
    const auto start_html = header_number(header, "StartHTML");
    const auto end_html = header_number(header, "EndHTML");
    const auto start_fragment = header_number(header, "StartFragment");
    const auto end_fragment = header_number(header, "EndFragment");
    const auto range = [&text](std::optional<std::int64_t> begin,
                               std::optional<std::int64_t> end) -> std::optional<std::string_view> {
        if (!begin || !end || *begin < 0 || *begin > *end || static_cast<std::uint64_t>(*end) > text.size()) {
            return std::nullopt;
        }
        return text.substr(static_cast<std::size_t>(*begin), static_cast<std::size_t>(*end - *begin));
    };
    auto document = range(start_html, end_html);
    if (!document && (!start_html || *start_html == -1)) {
        document = range(start_fragment, end_fragment);
    }
    if (!document) {
        return fail(Errc::invalid_value, "HTML Format data without valid offsets");
    }
    std::string_view body = *document;
    while (!body.empty() && body.back() == '\0') {
        body.remove_suffix(1);
    }
    std::string html = local_text_to_utf8(std::as_bytes(std::span(body)));
    if (!find_ascii(std::string_view(html).substr(0, std::min<std::size_t>(html.size(), 1024)), "charset")) {
        html.insert(0, R"(<meta http-equiv="content-type" content="text/html; charset=utf-8">)");
    }
    return html;
}

}  // namespace farland::channels::clipboard
