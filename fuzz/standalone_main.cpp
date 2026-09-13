// SPDX-FileCopyrightText: 2026 Maximilian Kenfenheuer
// SPDX-License-Identifier: Apache-2.0

// Stand-in for libFuzzer's main in non-fuzzing builds: runs every file given
// on the command line (directories recursively) through the fuzz target once.

#include "fuzz.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <vector>

int main(int argc, char** argv)
{
    std::vector<std::filesystem::path> files;
    for (const char* arg : std::span(argv, static_cast<std::size_t>(argc)).subspan(1)) {
        const std::filesystem::path path(arg);
        if (std::filesystem::is_directory(path)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
                if (entry.is_regular_file()) {
                    files.push_back(entry.path());
                }
            }
        } else {
            files.push_back(path);
        }
    }
    std::ranges::sort(files);

    for (const auto& file : files) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            std::cerr << "cannot open " << file << '\n';
            return 1;
        }
        const std::vector<char> content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(content.data()), content.size());
    }
    std::cout << "replayed " << files.size() << " inputs\n";
    return files.empty() ? 1 : 0;
}
