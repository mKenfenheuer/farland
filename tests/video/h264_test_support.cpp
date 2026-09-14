// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#include "h264_test_support.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;  // NOLINT(readability-redundant-declaration)

namespace farland::test {

std::string env_or(const char* name, const std::string& fallback)
{
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? std::string(value) : fallback;
}

int run(const std::vector<std::string>& args, const std::string& out, const std::string& err)
{
    std::vector<char*> argv;
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, argv.front(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool have_tool(const std::string& tool)
{
    return run({tool, "-version"}) == 0;
}

TempDir::TempDir()
{
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("farland-h264-" + std::to_string(getpid()) + "-" + std::to_string(counter++));
    std::filesystem::create_directories(path_);
}

TempDir::~TempDir()
{
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
}

void write_file(const std::string& path, std::span<const std::byte> bytes)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::vector<std::byte> read_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> chars{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::vector<std::byte> bytes(chars.size());
    std::ranges::transform(chars, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

void Psnr::add(std::span<const std::byte> a, std::span<const std::byte> b, std::size_t skip_every)
{
    if (a.size() != b.size()) {
        throw std::logic_error("PSNR over spans of different sizes");
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (skip_every != 0 && i % skip_every == skip_every - 1) {
            continue;
        }
        const double d = std::to_integer<int>(a[i]) - std::to_integer<int>(b[i]);
        sum += d * d;
        ++count;
    }
}

double Psnr::db() const
{
    const double mse = sum / static_cast<double>(count);
    return mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

std::vector<std::byte> decode_h264(const std::string& ffmpeg, std::span<const std::byte> stream)
{
    const TempDir dir;
    const auto h264_path = dir.file("stream.h264");
    const auto yuv_path = dir.file("decoded.yuv");
    write_file(h264_path, stream);
    // No -pix_fmt: the raw frames keep the decoder's layout (yuvj420p), unconverted.
    if (run({ffmpeg, "-v", "error", "-f", "h264", "-i", h264_path, "-f", "rawvideo", "-y", yuv_path}) != 0) {
        return {};
    }
    return read_file(yuv_path);
}

std::map<std::string, std::vector<std::string>>
probe_h264(const std::string& ffprobe, std::span<const std::byte> stream, const std::string& entries)
{
    const TempDir dir;
    const auto h264_path = dir.file("stream.h264");
    const auto probe_path = dir.file("probe.txt");
    write_file(h264_path, stream);
    std::map<std::string, std::vector<std::string>> result;
    if (run({ffprobe, "-v", "error", "-f", "h264", "-show_entries", entries, "-of", "default=nw=1", h264_path},
            probe_path) != 0) {
        return result;
    }
    std::ifstream probe(probe_path);
    for (std::string line; std::getline(probe, line);) {
        const auto eq = line.find('=');
        if (eq != std::string::npos) {
            result[line.substr(0, eq)].push_back(line.substr(eq + 1));
        }
    }
    return result;
}

}  // namespace farland::test
