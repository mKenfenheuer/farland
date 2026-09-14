// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include <farland/server/clipboard_files.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <sys/stat.h>
#include <unistd.h>

using farland::Errc;
using namespace farland::server::clipboard_files;
namespace cliprdr = farland::channels::cliprdr;
namespace fs = std::filesystem;

namespace {

/// A private temporary directory, removed afterwards.
struct TempDir {
    TempDir()
    {
        std::random_device random;
        path = fs::temp_directory_path() / ("farland-clipboard-" + std::to_string(random()));
        fs::create_directories(path);
        fs::permissions(path, fs::perms::owner_all);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path path;
};

void write_file(const fs::path& path, const std::string& content)
{
    std::ofstream out(path, std::ios::binary);
    out << content;
}

std::string read_text(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string text(const std::vector<std::byte>& data)
{
    std::string out;
    for (const auto b : data) {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

}  // namespace

TEST_CASE("Clipboard files: client names stay inside the staging directory")
{
    CHECK(split_client_name(R"(dir\sub\file.txt)").value() == std::vector<std::string>{"dir", "sub", "file.txt"});
    CHECK(split_client_name("caf\xC3\xA9 (1).txt").value() == std::vector<std::string>{"caf\xC3\xA9 (1).txt"});
    for (const char* bad : {"", R"(..\secret)", R"(dir\..\..\x)", R"(.\x)", R"(dir\)", R"(\x)", R"(C:\x)", "a/b",
                            "a\x01", "file.txt:stream"}) {
        INFO(bad);
        CHECK_FALSE(split_client_name(bad).has_value());
    }
    CHECK(split_client_name(std::string(256, 'x')).error().code == Errc::limit_exceeded);
    std::string deep;
    for (int i = 0; i < 33; ++i) {
        deep += "d\\";
    }
    deep += "f";
    CHECK(split_client_name(deep).error().code == Errc::limit_exceeded);
}

TEST_CASE("Clipboard files: URI lists")
{
    CHECK(parse_uri_list("file:///home/u/a%20b.txt\r\n# comment\r\nfile://localhost/tmp/x\r\n"
                         "http://example.com/y\r\nfile://otherhost/z\r\nfile:///bad%0\r\nfile:///nul%00x\r\n") ==
          std::vector<fs::path>{"/home/u/a b.txt", "/tmp/x"});
    CHECK(parse_uri_list("copy\nfile:///a\nfile:///b") == std::vector<fs::path>{"/a", "/b"});
    CHECK(parse_uri_list("cut\nfile:///a") == std::vector<fs::path>{"/a"});
    CHECK(file_uri("/run/user/1000/farland/a b#%\xC3\xA9.txt") ==
          "file:///run/user/1000/farland/a%20b%23%25%C3%A9.txt");
    CHECK(parse_uri_list(file_uri("/x/y z%") + "\r\n") == std::vector<fs::path>{"/x/y z%"});
}

TEST_CASE("Clipboard files: a staging directory")
{
    const TempDir temp;
    const auto root = temp.path / "farland";
    fs::path path;
    {
        auto staging = StagingDirectory::create(root);
        REQUIRE(staging.has_value());
        path = staging->path();
        struct stat info{};
        REQUIRE(::stat(path.c_str(), &info) == 0);
        CHECK((info.st_mode & 0777) == 0700);
        REQUIRE(::stat(root.c_str(), &info) == 0);
        CHECK((info.st_mode & 0777) == 0700);

        const std::vector<std::string> dir{"dir"};
        const std::vector<std::string> file{"dir", "sub", "a.txt"};
        REQUIRE(staging->make_directory(dir).has_value());
        REQUIRE(staging->create_file(file).has_value());
        const std::string content = "hello";
        REQUIRE(staging->append(std::as_bytes(std::span(content))).has_value());
        staging->close_file();
        CHECK(read_text(path / "dir" / "sub" / "a.txt") == "hello");
        CHECK(staging->create_file(file).error().code == Errc::invalid_value);  // exists

        // A symlink planted in the tree is not followed.
        fs::create_directory_symlink(temp.path, path / "link");
        CHECK_FALSE(staging->create_file(std::vector<std::string>{"link", "escape.txt"}).has_value());
        CHECK_FALSE(fs::exists(temp.path / "escape.txt"));

        auto moved = std::move(*staging);
        CHECK(fs::exists(moved.path()));
    }
    CHECK_FALSE(fs::exists(path));

    SECTION("a root others can read is refused")
    {
        fs::permissions(root, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
        CHECK_FALSE(StagingDirectory::create(root).has_value());
    }
    SECTION("a symlinked root is refused")
    {
        const auto link = temp.path / "link-root";
        fs::create_directory_symlink(root, link);
        CHECK_FALSE(StagingDirectory::create(link).has_value());
    }
}

TEST_CASE("Clipboard files: local files for the client")
{
    const TempDir temp;
    fs::create_directories(temp.path / "folder" / "inner");
    write_file(temp.path / "folder" / "inner" / "deep.txt", "deep");
    write_file(temp.path / "folder" / "b.txt", "bbbb");
    write_file(temp.path / "top.bin", "0123456789");
    write_file(temp.path / "outside.txt", "secret");
    fs::create_symlink(temp.path / "outside.txt", temp.path / "folder" / "a-link.txt");
    fs::create_symlink(temp.path / "top.bin", temp.path / "picked-link.bin");

    const std::vector<fs::path> paths{temp.path / "folder", temp.path / "top.bin", temp.path / "missing",
                                      temp.path / "picked-link.bin"};
    auto list = LocalFileList::from_paths(paths);
    REQUIRE(list.has_value());
    const auto& files = list->descriptors();
    REQUIRE(files.size() == 6);
    CHECK(files[0].name == "folder");
    CHECK((files[0].attributes & cliprdr::file_attribute::directory) != 0);
    CHECK(files[1].name == R"(folder\b.txt)");
    CHECK(files[1].size == 4);
    CHECK((files[1].flags & cliprdr::fd_flag::file_size) != 0);
    CHECK(files[2].name == R"(folder\inner)");
    CHECK(files[3].name == R"(folder\inner\deep.txt)");
    CHECK(files[4].name == "top.bin");
    CHECK(files[5].name == "picked-link.bin");  // picked by the user: resolved
    CHECK(files[5].last_write_time > 116444736000000000ULL);

    CHECK(list->size(4).value() == 10);
    CHECK(text(list->read(4, 2, 5).value()) == "23456");
    CHECK(text(list->read(4, 8, 100).value()) == "89");
    CHECK(list->read(4, 20, 5).value().empty());
    CHECK_FALSE(list->read(0, 0, 1).has_value());  // a directory
    CHECK_FALSE(list->read(6, 0, 1).has_value());
    CHECK_FALSE(list->size(-1).has_value());

    // Replacing a listed file (here with a symlink to another) is noticed.
    fs::remove(temp.path / "folder" / "b.txt");
    fs::create_symlink(temp.path / "outside.txt", temp.path / "folder" / "b.txt");
    CHECK_FALSE(list->read(1, 0, 4).has_value());
    fs::remove(temp.path / "folder" / "b.txt");
    write_file(temp.path / "folder" / "b.txt", "new!");
    CHECK_FALSE(list->read(1, 0, 4).has_value());

    SECTION("limits")
    {
        CHECK(LocalFileList::from_paths(paths, LocalFileLimits{3, 32}).error().code == Errc::limit_exceeded);
        CHECK(LocalFileList::from_paths(paths, LocalFileLimits{100, 1}).error().code == Errc::limit_exceeded);
    }
    SECTION("names Windows cannot take are skipped")
    {
        write_file(temp.path / "back\\slash", "x");
        const std::vector<fs::path> odd{temp.path / "back\\slash"};
        CHECK(LocalFileList::from_paths(odd).value().descriptors().empty());
    }
}
