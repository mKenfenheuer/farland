// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "support/transcript.hpp"

#include <farland/base/hexdump.hpp>

#include <format>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace farland::test {

std::vector<Record> parse_transcript(std::string_view text)
{
    std::vector<Record> records;
    int line_no = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t end = text.find('\n', pos);
        std::string_view line = text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        pos = end == std::string_view::npos ? text.size() + 1 : end + 1;
        ++line_no;

        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        if (line.find_first_not_of(" \t\r") == std::string_view::npos) {
            continue;
        }

        std::string_view payload;
        if (line.starts_with("C>") || line.starts_with("S>")) {
            const auto direction = line.front() == 'C' ? Direction::client_to_server : Direction::server_to_client;
            records.push_back(Record{direction, {}, line_no});
            payload = line.substr(2);
        } else if (line.front() == ' ' || line.front() == '\t') {
            if (records.empty()) {
                throw std::runtime_error(
                    std::format("transcript line {}: continuation line before any record", line_no));
            }
            payload = line;
        } else {
            throw std::runtime_error(
                std::format("transcript line {}: expected 'C>', 'S>' or an indented continuation line", line_no));
        }

        auto bytes = from_hex(payload);
        if (!bytes.has_value()) {
            throw std::runtime_error(std::format("transcript line {}: {}", line_no, bytes.error().message()));
        }
        auto& target = records.back().bytes;
        target.insert(target.end(), bytes->begin(), bytes->end());
    }
    return records;
}

std::vector<Record> load_transcript(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error(std::format("cannot open transcript {}", path.string()));
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return parse_transcript(text);
}

}  // namespace farland::test
