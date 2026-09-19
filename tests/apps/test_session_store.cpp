// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// The table farlandd writes so that its sessions outlive it: what a line
// looks like, and that nothing an agent could put in a field breaks it.

#include "session_store.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <vector>

using farland::daemon::decode_sessions;
using farland::daemon::encode_sessions;
using farland::daemon::load_sessions;
using farland::daemon::save_sessions;
using farland::daemon::StoredSession;

namespace {

farland::server::broker::Token token_of(std::uint8_t first)
{
    farland::server::broker::Token token{};
    for (std::size_t i = 0; i < token.size(); ++i) {
        token[i] = static_cast<std::byte>(first + i);
    }
    return token;
}

StoredSession session_of(std::uint32_t id, std::string account)
{
    StoredSession s;
    s.id = id;
    s.account = std::move(account);
    s.uid = 1000 + id;
    s.token = token_of(static_cast<std::uint8_t>(id));
    return s;
}

/// A directory of this test's own, removed with it.
struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("farland-sessions-" + std::to_string(::getpid()) + "-" +
                                  std::to_string(std::filesystem::hash_value(std::filesystem::current_path())));
    TempDir() { std::filesystem::create_directories(path); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

}  // namespace

TEST_CASE("Session table: a session round-trips with every field", "[daemon][sessions]")
{
    StoredSession s = session_of(7, "alice");
    s.attached = true;
    s.gdm = true;
    s.login_session = "1234";
    s.unit = "farland-agent-7.service";

    const auto decoded = decode_sessions(encode_sessions({s}));
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0] == s);
}

TEST_CASE("Session table: the empty fields survive", "[daemon][sessions]")
{
    // A PAM session has no unit, and an agent farlandd started itself has no
    // login session it knows by name.
    const StoredSession s = session_of(1, "bob");
    CHECK(s.login_session.empty());
    CHECK(s.unit.empty());
    const auto decoded = decode_sessions(encode_sessions({s}));
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0] == s);
    CHECK(decoded[0].login_session.empty());
    CHECK(decoded[0].unit.empty());
}

TEST_CASE("Session table: several sessions keep their order and ids", "[daemon][sessions]")
{
    const std::vector<StoredSession> sessions{session_of(3, "alice"), session_of(9, "bob"), session_of(11, "carol")};
    const auto decoded = decode_sessions(encode_sessions(sessions));
    REQUIRE(decoded.size() == 3);
    CHECK(decoded == sessions);
}

TEST_CASE("Session table: a line that makes no sense is skipped, not fatal", "[daemon][sessions]")
{
    const std::string text = encode_sessions({session_of(1, "alice"), session_of(2, "bob")});
    // A file cut in the middle of a write, and rubbish between good lines.
    const auto cut = text.substr(0, text.size() - 10);
    CHECK(decode_sessions(cut).size() == 1);

    std::string mixed = text;
    mixed += "nonsense\n";
    mixed += "4 dave notanumber 0 0 - - ";
    mixed += std::string(64, 'z');
    mixed += "\n";
    mixed += "5 eve 1005 0 0 - - deadbeef\n";  // token too short
    const auto decoded = decode_sessions(mixed);
    REQUIRE(decoded.size() == 2);
    CHECK(decoded[0].account == "alice");
    CHECK(decoded[1].account == "bob");
}

TEST_CASE("Session table: a session without an id or an account is not written", "[daemon][sessions]")
{
    StoredSession no_id = session_of(0, "alice");
    StoredSession no_account = session_of(4, "");
    CHECK(decode_sessions(encode_sessions({no_id, no_account})).empty());
}

TEST_CASE("Session table: a field with a space in it cannot break the format", "[daemon][sessions]")
{
    StoredSession s = session_of(2, "alice");
    s.unit = "a unit with spaces";  // nothing should produce this, but a line must still parse
    const auto decoded = decode_sessions(encode_sessions({s}));
    REQUIRE(decoded.size() == 1);
    CHECK(decoded[0].id == 2);
    CHECK(decoded[0].account == "alice");
    CHECK(decoded[0].unit.empty());  // dropped rather than written as two fields
}

TEST_CASE("Session table: the file is written 0600 and replaced whole", "[daemon][sessions]")
{
    const TempDir dir;
    const auto path = dir.path / "sessions";
    CHECK(load_sessions(path).empty());  // no file yet is no sessions

    REQUIRE(save_sessions(path, {session_of(1, "alice")}).has_value());
    struct ::stat info{};
    REQUIRE(::stat(path.c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0600);  // it holds a token per session
    REQUIRE(load_sessions(path).size() == 1);

    // Writing again replaces the lot, and leaves no temporary behind.
    REQUIRE(save_sessions(path, {session_of(2, "bob"), session_of(3, "carol")}).has_value());
    const auto reloaded = load_sessions(path);
    REQUIRE(reloaded.size() == 2);
    CHECK(reloaded[0].account == "bob");
    CHECK_FALSE(std::filesystem::exists(path.string() + ".new"));

    // And an empty table empties the file.
    REQUIRE(save_sessions(path, {}).has_value());
    CHECK(load_sessions(path).empty());
}
