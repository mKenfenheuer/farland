// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "session_store.hpp"

#include <farland/base/log.hpp>
#include <farland/base/text.hpp>

#include <array>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace farland::daemon {

namespace {

constexpr std::string_view log_component = "daemon.sessions";
/// An empty field; a session's unit or login session may have none, and a
/// line has to keep its shape.
constexpr std::string_view none = "-";
constexpr std::size_t fields_per_line = 8;

[[nodiscard]] std::string hex_of(const server::broker::Token& token)
{
    static constexpr std::array<char, 16> digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                                 '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string out;
    out.reserve(token.size() * 2);
    for (const std::byte b : token) {
        const auto value = std::to_integer<std::uint8_t>(b);
        out.push_back(digits.at(value >> 4U));
        out.push_back(digits.at(value & 0x0FU));
    }
    return out;
}

[[nodiscard]] bool token_from_hex(std::string_view text, server::broker::Token& token)
{
    if (text.size() != token.size() * 2) {
        return false;
    }
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };
    for (std::size_t i = 0; i < token.size(); ++i) {
        const int high = nibble(text[i * 2]);
        const int low = nibble(text[(i * 2) + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        token[i] = static_cast<std::byte>((high << 4) | low);
    }
    return true;
}

/// A field as written: "-" for the empty string, and anything with a space
/// or a newline in it refused, because the format could not hold it.
[[nodiscard]] std::string field_of(const std::string& value)
{
    if (value.empty()) {
        return std::string(none);
    }
    if (value.find_first_of(" \t\r\n") != std::string::npos) {
        return std::string(none);
    }
    return value;
}

[[nodiscard]] std::string value_of(std::string_view field)
{
    return field == none ? std::string() : std::string(field);
}

template <class T>
[[nodiscard]] bool number_of(std::string_view field, T& out)
{
    const auto* end = field.data() + field.size();
    const auto result = std::from_chars(field.data(), end, out);
    return result.ec == std::errc{} && result.ptr == end;
}

}  // namespace

std::string encode_sessions(const std::vector<StoredSession>& sessions)
{
    std::string out = "# farland sessions; farlandd rewrites this file, do not edit\n";
    for (const auto& s : sessions) {
        if (s.id == 0 || s.account.empty()) {
            continue;
        }
        out += std::to_string(s.id);
        out += ' ';
        out += field_of(s.account);
        out += ' ';
        out += std::to_string(s.uid);
        out += ' ';
        out += s.attached ? '1' : '0';
        out += ' ';
        out += s.gdm ? '1' : '0';
        out += ' ';
        out += field_of(s.login_session);
        out += ' ';
        out += field_of(s.unit);
        out += ' ';
        out += hex_of(s.token);
        out += '\n';
    }
    return out;
}

std::vector<StoredSession> decode_sessions(std::string_view text)
{
    std::vector<StoredSession> out;
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::array<std::string, fields_per_line> fields;
        std::istringstream parts{line};
        std::size_t count = 0;
        while (count < fields.size() && (parts >> fields.at(count))) {
            ++count;
        }
        std::string extra;
        if (count != fields.size() || (parts >> extra)) {
            log::warn(log_component, "ignoring a session line that does not parse");
            continue;
        }
        StoredSession s;
        if (!number_of(fields[0], s.id) || s.id == 0 || !number_of(fields[2], s.uid) ||
            !token_from_hex(fields[7], s.token)) {
            log::warn(log_component, "ignoring a session line with a bad field");
            continue;
        }
        s.account = value_of(fields[1]);
        if (s.account.empty()) {
            continue;
        }
        s.attached = fields[3] == "1";
        s.gdm = fields[4] == "1";
        s.login_session = value_of(fields[5]);
        s.unit = value_of(fields[6]);
        out.push_back(std::move(s));
    }
    return out;
}

Result<void> save_sessions(const std::filesystem::path& path, const std::vector<StoredSession>& sessions)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path temporary = path.string() + ".new";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            return fail(Errc::io, "cannot write the session table");
        }
        const std::string text = encode_sessions(sessions);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) {
            return fail(Errc::io, "cannot write the session table");
        }
    }
    // It holds a token per session.
    if (::chmod(temporary.c_str(), S_IRUSR | S_IWUSR) != 0) {
        std::filesystem::remove(temporary, ec);
        return fail(Errc::io, "cannot set the mode of the session table");
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return fail(Errc::io, "cannot replace the session table");
    }
    return {};
}

std::vector<StoredSession> load_sessions(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};  // none yet, which is not a problem
    }
    std::ostringstream text;
    text << file.rdbuf();
    return decode_sessions(text.str());
}

}  // namespace farland::daemon
