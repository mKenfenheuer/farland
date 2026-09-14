// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/platform/portal/portal_session.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

namespace portal = farland::platform::portal;
using std::filesystem::perms;

namespace {

std::filesystem::path temp_dir()
{
    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() / ("farland-token-" + std::to_string(rd()));
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

TEST_CASE("Restore tokens are stored with mode 0600")
{
    const auto dir = temp_dir();
    const auto path = dir / "farland" / "portal-restore-token";

    const auto missing = portal::load_restore_token(path);
    REQUIRE(missing.has_value());
    CHECK_FALSE(missing->has_value());

    REQUIRE(portal::save_restore_token(path, "3f1a8f0e-6f0b-4d7a-9c55-0f6c1d2e3b4a").has_value());
    CHECK((std::filesystem::status(path).permissions() & perms::all) == (perms::owner_read | perms::owner_write));
    CHECK((std::filesystem::status(path.parent_path()).permissions() & perms::all) == perms::owner_all);
    CHECK(portal::load_restore_token(path).value() == "3f1a8f0e-6f0b-4d7a-9c55-0f6c1d2e3b4a");

    REQUIRE(portal::save_restore_token(path, "second").has_value());  // replaces
    CHECK(portal::load_restore_token(path).value() == "second");

    CHECK_FALSE(portal::save_restore_token(path, "").has_value());
    CHECK_FALSE(portal::save_restore_token(path, "has space").has_value());
    CHECK_FALSE(portal::save_restore_token(path, "line\nbreak").has_value());

    std::filesystem::permissions(path, perms::owner_read | perms::owner_write | perms::group_read);
    CHECK_FALSE(portal::load_restore_token(path).has_value());

    std::filesystem::remove(path);
    {
        std::ofstream(path) << "not a token\n";
    }
    std::filesystem::permissions(path, perms::owner_read | perms::owner_write);
    CHECK(portal::load_restore_token(path).error().code == farland::Errc::invalid_value);

    REQUIRE(portal::remove_restore_token(path).has_value());
    REQUIRE(portal::remove_restore_token(path).has_value());  // already gone
    CHECK_FALSE(portal::load_restore_token(path).value().has_value());
    std::filesystem::remove_all(dir);
}

TEST_CASE("The restore token lives in XDG_STATE_HOME")
{
    const char* old_state = std::getenv("XDG_STATE_HOME");
    const std::string saved = old_state != nullptr ? old_state : "";

    ::setenv("XDG_STATE_HOME", "/state", 1);
    CHECK(portal::default_restore_token_path() == std::filesystem::path("/state/farland/portal-restore-token"));
    ::setenv("XDG_STATE_HOME", "relative", 1);  // the spec says to ignore relative paths
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        CHECK(portal::default_restore_token_path() ==
              std::filesystem::path(home) / ".local/state/farland/portal-restore-token");
    }

    if (old_state != nullptr) {
        ::setenv("XDG_STATE_HOME", saved.c_str(), 1);
    } else {
        ::unsetenv("XDG_STATE_HOME");
    }
}
