// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <farland/server/broker.hpp>

#include <optional>
#include <string>
#include <string_view>

/// The broker token (server/broker.hpp) as it travels from farlandd to a new
/// farland-agent: 64 lower-case hex digits, through an inherited pipe
/// (`--token-fd`) or, where a service manager starts the agent, the
/// environment variable FARLAND_AGENT_TOKEN, which the agent clears at once.
namespace farland::app {

inline constexpr std::string_view agent_token_variable = "FARLAND_AGENT_TOKEN";

[[nodiscard]] inline std::string token_to_hex(const server::broker::Token& token)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text;
    text.reserve(token.size() * 2);
    for (const std::byte b : token) {
        const auto value = std::to_integer<unsigned>(b);
        text.push_back(digits.at(value >> 4U));
        text.push_back(digits.at(value & 0xFU));
    }
    return text;
}

[[nodiscard]] inline std::optional<server::broker::Token> token_from_hex(std::string_view text)
{
    server::broker::Token token{};
    if (text.size() != token.size() * 2) {
        return std::nullopt;
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
        const int high = nibble(text[2 * i]);
        const int low = nibble(text[(2 * i) + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        token.at(i) = static_cast<std::byte>((high << 4) | low);
    }
    return token;
}

}  // namespace farland::app
