// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/auth/credential_store.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>

using farland::Errc;
using farland::auth::CredentialStore;
using farland::auth::NtHash;

namespace {

NtHash hash_of(std::uint8_t seed)
{
    NtHash h{};
    for (std::size_t i = 0; i < h.size(); ++i) {
        h.at(i) = static_cast<std::byte>(seed + i);
    }
    return h;
}

std::filesystem::path temp_dir()
{
    std::random_device rd;
    auto dir = std::filesystem::temp_directory_path() / ("farland-creds-" + std::to_string(rd()));
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

TEST_CASE("Credential lines parse and serialize")
{
    const auto store = CredentialStore::parse("# comment\n"
                                              "alice::00112233445566778899aabbccddeeff\n"
                                              "  bob:LAB:ffeeddccbbaa99887766554433221100  # trailing\n"
                                              "\n")
                           .value();
    REQUIRE(store.entries().size() == 2);
    CHECK(store.entries()[0].user == "alice");
    CHECK(store.entries()[0].domain.empty());
    CHECK(store.entries()[1].domain == "LAB");
    CHECK(std::to_integer<unsigned>(store.entries()[1].hash[0]) == 0xff);

    const auto again = CredentialStore::parse(store.serialize()).value();
    REQUIRE(again.entries().size() == 2);
    CHECK(again.entries()[1].hash == store.entries()[1].hash);
    CHECK(store.serialize().starts_with("# farland NLA users"));
}

TEST_CASE("Lookups ignore case and prefer an exact domain over the wildcard")
{
    CredentialStore store;
    store.set("alice", "", hash_of(1));
    store.set("alice", "LAB", hash_of(2));
    store.set("bob", "LAB", hash_of(3));

    CHECK(store.lookup("ALICE", "lab") == hash_of(2));
    CHECK(store.lookup("alice", "WORKSTATION") == hash_of(1));
    CHECK(store.lookup("alice", "") == hash_of(1));
    CHECK(store.lookup("Bob", "Lab") == hash_of(3));
    CHECK_FALSE(store.lookup("bob", "OTHER").has_value());
    CHECK_FALSE(store.lookup("carol", "").has_value());

    store.set("ALICE", "lab", hash_of(4));  // replaces, does not duplicate
    CHECK(store.entries().size() == 3);
    CHECK(store.lookup("alice", "LAB") == hash_of(4));
    CHECK(store.remove("alice", "LAB"));
    CHECK_FALSE(store.remove("alice", "LAB"));
    CHECK(store.lookup("alice", "LAB") == hash_of(1));
}

TEST_CASE("Malformed credential files are rejected with the line number")
{
    const auto check = [](std::string_view text, std::size_t line) {
        const auto store = CredentialStore::parse(text);
        REQUIRE_FALSE(store.has_value());
        CHECK(store.error().code == Errc::invalid_value);
        CHECK(store.error().offset == line);
    };
    check("alice:00112233445566778899aabbccddeeff\n", 1);                                    // missing domain field
    check("\nalice::0011\n", 2);                                                             // short hash
    check(":LAB:00112233445566778899aabbccddeeff\n", 1);                                     // empty user
    check("a::00112233445566778899aabbccddeeff\nA::00112233445566778899aabbccddeeff\n", 2);  // duplicate
    CHECK_FALSE(CredentialStore::valid_name("a:b", false));
    CHECK_FALSE(CredentialStore::valid_name("line\nbreak", false));
    CHECK(CredentialStore::valid_name("", true));
}

TEST_CASE("The store saves atomically with private permissions and reloads")
{
    const auto dir = temp_dir();
    const auto path = dir / "nested" / "users";
    CHECK(CredentialStore::load(path).value().entries().empty());  // missing file: empty store

    CredentialStore store;
    store.set("alice", "LAB", hash_of(7));
    REQUIRE(store.save(path).has_value());
    const auto perms = std::filesystem::status(path).permissions();
    CHECK((perms & std::filesystem::perms::all) ==
          (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
    CHECK((std::filesystem::status(dir / "nested").permissions() & std::filesystem::perms::all) ==
          std::filesystem::perms::owner_all);
    CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));

    const auto loaded = CredentialStore::load(path).value();
    CHECK(loaded.lookup("alice", "lab") == hash_of(7));

    std::filesystem::permissions(path, std::filesystem::perms::group_read, std::filesystem::perm_options::add);
    const auto refused = CredentialStore::load(path);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == Errc::io);
    std::filesystem::remove_all(dir);
}
