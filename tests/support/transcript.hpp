// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>
#include <vector>

/// Connection transcripts: recorded, TLS-decrypted byte streams replayed
/// through the sans-IO protocol core (docs/PLAN.md §5). The text format is
/// described in tests/data/transcripts/README.md.
namespace farland::test {

enum class Direction { client_to_server, server_to_client };

struct Record {
    Direction direction;
    std::vector<std::byte> bytes;
    int line = 0;  ///< Line of the record's "C>" or "S>" marker, for failure messages.
};

/// Throws std::runtime_error with the line number on malformed input.
[[nodiscard]] std::vector<Record> parse_transcript(std::string_view text);
[[nodiscard]] std::vector<Record> load_transcript(const std::filesystem::path& path);

}  // namespace farland::test
