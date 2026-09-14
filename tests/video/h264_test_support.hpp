// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

/// Helpers shared by the H.264 encoder tests: running ffmpeg and ffprobe
/// (FARLAND_FFMPEG, FARLAND_FFPROBE), temporary files and PSNR.
namespace farland::test {

/// The environment variable `name`, or `fallback` if it is unset or empty.
[[nodiscard]] std::string env_or(const char* name, const std::string& fallback);

/// Runs a program found in PATH, with stdout and stderr redirected to files.
/// Returns its exit status, or -1 if it could not run.
int run(const std::vector<std::string>& args, const std::string& out = "/dev/null",
        const std::string& err = "/dev/null");

[[nodiscard]] bool have_tool(const std::string& tool);

class TempDir {
public:
    TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir();
    [[nodiscard]] std::string file(const char* name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
};

void write_file(const std::string& path, std::span<const std::byte> bytes);
[[nodiscard]] std::vector<std::byte> read_file(const std::string& path);

/// Accumulates squared errors for a PSNR over 8-bit samples.
struct Psnr {
    double sum = 0;
    std::size_t count = 0;

    /// `skip_every`: leave out every n-th byte (4 skips the X of BGRX).
    void add(std::span<const std::byte> a, std::span<const std::byte> b, std::size_t skip_every = 0);
    [[nodiscard]] double db() const;
};

/// Decodes an Annex B stream with ffmpeg into raw frames in the decoder's
/// layout (I420 for these streams). Empty if ffmpeg failed.
[[nodiscard]] std::vector<std::byte> decode_h264(const std::string& ffmpeg, std::span<const std::byte> stream);

/// `ffprobe -show_entries <entries>` of an Annex B stream, as lists of values
/// per key in order of appearance.
[[nodiscard]] std::map<std::string, std::vector<std::string>>
probe_h264(const std::string& ffprobe, std::span<const std::byte> stream, const std::string& entries);

}  // namespace farland::test
