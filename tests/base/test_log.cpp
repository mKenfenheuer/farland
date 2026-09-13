// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/base/log.hpp>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <string>
#include <vector>

namespace flog = farland::log;

namespace {

std::vector<std::string>& captured()
{
    static std::vector<std::string> lines;
    return lines;
}

void capture(flog::Level level, std::string_view component, std::string_view message)
{
    captured().push_back(std::format("{} {} {}", flog::to_string(level), component, message));
}

}  // namespace

TEST_CASE("Log messages are filtered by level and reach the sink")
{
    captured().clear();
    flog::set_sink(&capture);
    flog::set_level(flog::Level::info);

    flog::debug("test", "hidden {}", 1);
    flog::info("test", "shown {} {}", 2, "x");
    flog::error("test.sub", "failed");
    flog::set_level(flog::Level::off);
    flog::error("test", "suppressed");

    flog::set_sink(nullptr);
    flog::set_level(flog::Level::info);

    CHECK(captured() == std::vector<std::string>{"info test shown 2 x", "error test.sub failed"});
}
